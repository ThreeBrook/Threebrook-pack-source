#include "storage.hpp"

#include "iis/iis.h"

#include "formats.hpp"
#include "io.hpp"

#include <algorithm>
#include <filesystem>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace iis::svc {

namespace fs = std::filesystem;

namespace {

bool exists(const std::string& p) {
    return GetFileAttributesW(io::win_path(p.c_str()).c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::string mb(uint64_t b) { return std::to_string(b / 1000000) + " MB"; }

}  // namespace

Storage::Storage(const Config& cfg, Log& log, Index& index, Audit& audit, const Classifier& cls, Claim claim,
                 Unclaim unclaim)
    : cfg_(cfg), log_(log), index_(index), audit_(audit), cls_(cls), claim_(std::move(claim)), unclaim_(std::move(unclaim)) {
    // 启动扫描先跑一会儿再做第一次保留检查
    next_retention_ms_ = now_ms() + std::min<int64_t>(5000, int64_t(cfg.retention_interval_s * 1000));
}

RootSpace Storage::measure(size_t i) {
    RootSpace s;
    const WatchDir& w = cfg_.watch[i];
    s.root = w.path;
    ULARGE_INTEGER avail{}, total{};
    s.online = GetDiskFreeSpaceExW(io::win_path(w.path.c_str()).c_str(), &avail, &total, nullptr) != 0;
    if (!s.online) return s;
    s.capacity = total.QuadPart;
    s.free = avail.QuadPart;
    if (w.quota_mb > 0) {
        const uint64_t quota = uint64_t(w.quota_mb * 1e6);
        const uint64_t used = index_.totals(w.path).used;
        s.capacity = quota;
        s.free = std::min<uint64_t>(s.free, used >= quota ? 0 : quota - used);
    }
    const DiskGuard& g = cfg_.disk;
    const double cap = double(s.capacity);
    s.warn = uint64_t(std::max(g.warn_free_mb * 1e6, g.warn_free_percent / 100 * cap));
    s.del = uint64_t(std::max(g.delete_free_mb * 1e6, g.delete_free_percent / 100 * cap));
    s.target = uint64_t(std::max(g.target_free_mb * 1e6, g.target_free_percent / 100 * cap));
    s.level = s.free < s.del ? 2 : s.free < s.warn ? 1 : 0;
    return s;
}

bool Storage::remove(const Row& r, const std::string& reason, std::vector<Audit::Entry>& audit) {
    if (!claim_(r.path)) return false;  // 正在压缩：跳过
    struct Unclaim {
        const Storage::Unclaim& f;
        const std::string& p;
        ~Unclaim() { f(p); }
    } unclaim{unclaim_, r.path};
    uint64_t freed = 0;
    for (const std::string& p : {r.path + ".iis", r.path}) {
        WIN32_FILE_ATTRIBUTE_DATA a;
        const std::wstring w = io::win_path(p.c_str());
        if (!GetFileAttributesExW(w.c_str(), GetFileExInfoStandard, &a)) continue;
        if (!DeleteFileW(w.c_str())) {
            log_.warn("删除失败（错误 " + std::to_string(GetLastError()) + "），稍后重试：" + p);
            if (freed) break;  // .iis 已删、原文件删不掉：照实记录
            return false;
        }
        freed += (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
    }
    if (exists(r.path) || exists(r.path + ".iis")) {  // 只删掉一部分：更新占用，下次再删
        Row d = r;
        d.disk_size = 0;
        for (const std::string& p : {r.path + ".iis", r.path}) {
            WIN32_FILE_ATTRIBUTE_DATA a;
            if (GetFileAttributesExW(io::win_path(p.c_str()).c_str(), GetFileExInfoStandard, &a))
                d.disk_size += (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
        }
        index_.upsert(d);
        return false;
    }
    Row d = r;
    d.state = ST_DELETED;
    d.disk_size = 0;
    index_.upsert(d);
    index_.add_daily({"", 0, 0, 0, 1, freed});
    ++deleted_;
    if (r.verdict == "NG") ++deleted_ng_;
    audit.push_back({"delete", r.path,
                     {{"reason", reason}, {"verdict", r.verdict}, {"station", r.station}, {"product", r.product},
                      {"batch", r.batch}, {"mtime_ms", r.mtime_ms}, {"orig_size", r.orig_size}, {"freed", freed},
                      {"sha256", r.sha256}, {"state", state_name(r.state)}}});
    return true;
}

void Storage::flush(std::vector<Audit::Entry>& audit) {
    if (audit.empty()) return;
    if (!audit_.append(audit)) log_.error("写审计日志失败");
    audit.clear();
}

void Storage::tick() {
    std::vector<RootSpace> now;
    bool accel = false;
    for (size_t i = 0; i < cfg_.watch.size(); ++i) {
        RootSpace s = measure(i);
        RootSpace before;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (i < spaces_.size()) before = spaces_[i];
        }
        if (s.online && s.level != before.level) {
            const std::string what = "剩余 " + mb(s.free) + " / 容量 " + mb(s.capacity) + "：" + s.root;
            if (s.level == 2) log_.warn("剩余空间低于删除阈值（" + mb(s.del) + "），开始删除最旧的 OK 图：" + what);
            else if (s.level == 1) log_.warn("剩余空间低于告警阈值（" + mb(s.warn) + "），加速压缩：" + what);
            else log_.info("剩余空间恢复正常：" + what);
        }
        if (s.online && s.level == 2) {
            // 防爆盘：删最旧的可删除图，直到回到目标值
            std::vector<Audit::Entry> audit;
            uint64_t freed_rows = 0;
            bool done = false;
            {
                index_.scan_oldest(s.root, INT64_MAX, [&](const Row& r) {
                    if (!cls_.deletable_verdict(r.verdict)) return true;
                    if (!remove(r, "防爆盘：剩余空间低于删除阈值", audit)) return true;
                    ++freed_rows;
                    if (audit.size() >= 50) flush(audit);
                    if (freed_rows % 20 == 0 || cfg_.watch[i].quota_mb > 0) {  // 配额只算索引，量起来很便宜
                        const RootSpace m = measure(i);
                        if (m.free >= m.target) {
                            done = true;
                            return false;
                        }
                    }
                    return true;
                });
            }
            flush(audit);
            s = measure(i);
            s.stuck = !done && s.free < s.target;
            if (s.stuck && !before.stuck)
                log_.error("剩余空间仍低于目标且没有可删除的 OK 图（NG / 未知不删）：" + s.root + "，剩余 " + mb(s.free) +
                           "；请人工清理或扩容");
            if (freed_rows) log_.info("防爆盘删除 " + std::to_string(freed_rows) + " 个文件，剩余 " + mb(s.free) + "：" + s.root);
        }
        accel = accel || (s.online && s.level >= 1);
        now.push_back(s);
    }
    accelerate_ = accel;
    {
        std::lock_guard<std::mutex> lk(mu_);
        spaces_ = now;
    }
    // 保留策略
    if (cfg_.retention.empty() || now_ms() < next_retention_ms_) return;
    next_retention_ms_ = now_ms() + int64_t(cfg_.retention_interval_s * 1000);
    double max_days = 0;
    for (const auto& r : cfg_.retention) max_days = std::max(max_days, r.days);
    if (max_days <= 0) return;
    double min_days = max_days;
    for (const auto& r : cfg_.retention)
        if (r.days > 0) min_days = std::min(min_days, r.days);
    const int64_t now_t = now_ms();
    uint64_t n = 0;
    for (const auto& w : cfg_.watch) {
        std::vector<Audit::Entry> audit;
        {
            index_.scan_oldest(w.path, now_t - int64_t(min_days * 86400000.0), [&](const Row& r) {
                for (size_t k = 0; k < cfg_.retention.size(); ++k) {
                    const RetentionRule& rule = cfg_.retention[k];
                    if (!fmt::glob_match(rule.station, r.station) || !fmt::glob_match(rule.product, r.product)) continue;
                    const bool vmatch = rule.verdict == "*" ? r.verdict != "NG" : rule.verdict == r.verdict;
                    if (!vmatch) continue;
                    if (rule.days > 0 && r.mtime_ms < now_t - int64_t(rule.days * 86400000.0) &&
                        remove(r, "保留期满（规则 " + std::to_string(k + 1) + "，" + std::to_string(rule.days) + " 天）",
                               audit))
                        ++n;
                    break;  // 第一条匹配的规则说了算
                }
                if (audit.size() >= 50) flush(audit);
                return true;
            });
        }
        flush(audit);
    }
    if (n) log_.info("保留策略删除 " + std::to_string(n) + " 个过期文件");
}

std::vector<RootSpace> Storage::spaces() {
    std::lock_guard<std::mutex> lk(mu_);
    return spaces_;
}

ExportResult export_files(Index& index, Audit& audit, const Filter& f, const std::string& out_dir, bool restore,
                          const std::function<void(const std::string&)>& on_error,
                          const std::function<void(const ExportResult&)>& progress) {
    ExportResult res;
    std::vector<Audit::Entry> entries;
    auto fail = [&](const std::string& path, const std::string& why) {
        ++res.failed;
        if (on_error) on_error(path + "：" + why);
        if (progress) progress(res);
    };
    index.query(f, [&](const Row& r) {
        std::string root_name = fs::u8path(r.root).filename().u8string();
        if (root_name.empty()) root_name = "root";
        fs::path dst = fs::u8path(out_dir) / fs::u8path(root_name) / fs::u8path(r.rel);
        std::error_code ec;
        fs::create_directories(fs::path(L"\\\\?\\" + fs::absolute(dst).parent_path().make_preferred().wstring()), ec);
        const std::string iis_path = r.path + ".iis";
        const bool have_iis = exists(iis_path), have_orig = exists(r.path);
        std::string mode;
        if (restore && have_iis) {
            mode = "restore";
            const auto d8 = dst.u8string();
            iis_reader* rd = nullptr;
            if (iis_reader_open_file(iis_path.c_str(), &rd) != IIS_OK ||
                iis_reader_restore_file(rd, std::string(d8.begin(), d8.end()).c_str(), 1) != IIS_OK) {
                fail(r.path, iis_last_error());
                iis_reader_close(rd);
                return true;
            }
            iis_reader_close(rd);
        } else if (have_iis || have_orig) {
            const std::string src = have_iis ? iis_path : r.path;
            if (have_iis) dst += ".iis";
            mode = have_iis ? "copy_iis" : "copy_original";
            if (!CopyFileW(io::win_path(src.c_str()).c_str(), io::win_path(dst.u8string().c_str()).c_str(), TRUE)) {
                fail(r.path, "复制失败（错误 " + std::to_string(GetLastError()) + "）");
                return true;
            }
        } else {
            fail(r.path, "文件已不在磁盘上");
            return true;
        }
        ++res.files;
        res.bytes += restore ? r.orig_size : r.disk_size;
        if (progress) progress(res);
        entries.push_back({"export", r.path, {{"to", dst.u8string()}, {"mode", mode}, {"sha256", r.sha256}}});
        if (entries.size() >= 200) {
            audit.append(entries);
            entries.clear();
        }
        return true;
    });
    entries.push_back({"export_summary", out_dir,
                       {{"files", res.files}, {"bytes", res.bytes}, {"failed", res.failed}, {"restore", restore},
                        {"filter",
                         {{"from_ms", f.from_ms == INT64_MIN ? nlohmann::json(nullptr) : nlohmann::json(f.from_ms)},
                          {"to_ms", f.to_ms == INT64_MAX ? nlohmann::json(nullptr) : nlohmann::json(f.to_ms)},
                          {"root", f.root}, {"station", f.station}, {"product", f.product}, {"batch", f.batch},
                          {"verdict", f.verdict}}}}});
    audit.append(entries);
    return res;
}

}  // namespace iis::svc
