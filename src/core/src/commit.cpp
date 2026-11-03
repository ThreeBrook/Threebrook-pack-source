// 文件压缩的原子提交与启动恢复（阶段 1 T4）。协议见 iis.h 中 iis_compress_file / iis_recover_dir 的说明。
//
// 不变式：原文件只在 X.iis 已完整落盘、读回一致、改名完成之后，且独占复核内容未变时才会被删除；
// 临时文件只在能证明它多余（原文件仍在）时才删除。由此任何时刻终止进程或断电，
// 原文件与完整的 .iis 至少存在其一，且 .iis 一旦以正式名字出现就是完整、已自检的。
#include "commit.hpp"

#include "container.hpp"
#include "fs.hpp"
#include "pipeline.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <random>
#include <thread>
#include <unordered_set>

namespace iis::commit {

namespace {

constexpr const char* kIis = ".iis";
constexpr const char* kTmp = ".iistmp";
constexpr size_t kWriteChunk = 256u << 10;
constexpr uint64_t kMaxSidecar = 1u << 20;
constexpr int kMaxDepth = 64;

bool ends_with(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

using clock = std::chrono::steady_clock;
double since(clock::time_point t) { return std::chrono::duration<double>(clock::now() - t).count(); }

std::string make_temp_name(const std::string& final_path) {
    static std::atomic<uint32_t> counter{0};
    thread_local std::mt19937 rng(std::random_device{}() ^
                                  uint32_t(std::hash<std::thread::id>{}(std::this_thread::get_id())));
    char hex[9];
    std::snprintf(hex, sizeof hex, "%08x", unsigned(rng() ^ (counter.fetch_add(1) * 0x9E3779B9u)));
    return final_path + "." + hex + kTmp;
}

// 临时文件名 → 对应的 .iis 文件名；不是本程序生成的名字返回空串
std::string temp_target(const std::string& name) {
    if (!ends_with(name, kTmp)) return {};
    std::string s = name.substr(0, name.size() - std::strlen(kTmp));
    if (s.size() < 9 || s[s.size() - 9] != '.') return {};
    for (size_t i = s.size() - 8; i < s.size(); ++i)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return {};
    s.resize(s.size() - 9);
    if (!ends_with(s, kIis) || s.size() == std::strlen(kIis)) return {};
    return s;
}

// 同名参数文件：a.raw → a.json（与阶段 0 Path.with_suffix 一致）
std::string sidecar_path(const std::string& path) {
    const std::string name = fs::base_name(path);
    const size_t dot = name.rfind('.');
    const std::string stem = dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
    return path.substr(0, path.size() - name.size()) + stem + ".json";
}

bool is_container(const std::vector<uint8_t>& d) {
    static const uint8_t magic[8] = {'I', 'I', 'S', 'C', 'O', 'N', 'T', 0};
    return d.size() >= 8 && std::memcmp(d.data(), magic, 8) == 0;
}

enum class Existing { None, Match, Stale, Repair, Foreign };

// 检查已存在的 X.iis 与当前原文件的关系
int probe(const std::string& final_path, uint64_t size, const uint8_t sha[32], Existing& ex, uint64_t& csize,
          std::string& why) {
    ex = Existing::None;
    const fs::Kind k = fs::kind(final_path);
    if (k == fs::Kind::Missing) return IIS_OK;
    if (k != fs::Kind::File) {
        ex = Existing::Foreign;
        why = "目标位置已有同名的目录或特殊文件：" + final_path;
        return IIS_OK;
    }
    std::unique_ptr<io::Source> src;
    if (int st = io::open_file_source(final_path.c_str(), src)) return st;  // 被占用等：稍后重试
    csize = src->size();
    cont::Reader r;
    if (r.open(std::move(src)) != IIS_OK) {
        ex = Existing::Foreign;
        why = "已有的 " + fs::base_name(final_path) + " 不是有效的 IIS 容器（" + iis_last_error() + "），不覆盖";
        return IIS_OK;
    }
    if (r.orig_size() != size || std::memcmp(r.orig_sha256(), sha, 32) != 0) {
        ex = Existing::Stale;
        return IIS_OK;
    }
    cont::Buf out;
    ex = r.restore(out) == IIS_OK ? Existing::Match : Existing::Repair;
    if (ex == Existing::Repair) why = std::string("已有 .iis 校验失败（") + iis_last_error() + "），用原文件重新生成";
    return IIS_OK;
}

int injected(int st, const char* what) { return fail(st, std::string("注入故障：") + what); }

// 写临时文件 → 落盘 → 读回比较 → 改名为正式名字。失败时删除临时文件，不动正式名字
int write_and_rename(const std::string& final_path, const std::vector<uint8_t>& c, bool replace, bool sync) {
    std::string tmp;
    std::unique_ptr<io::Sink> sink;
    for (int attempt = 0;; ++attempt) {
        tmp = make_temp_name(final_path);
        if (int st = fault::at("temp.create")) return injected(st, "无法创建临时文件");
        const int st = io::open_file_sink(tmp.c_str(), sink);
        if (st == IIS_OK) break;
        if (st != IIS_E_CONFLICT || attempt >= 3) return st;  // 随机名撞上了：换一个
    }
    if (int st = fault::at("temp.created")) {
        sink->abort();
        return injected(st, "临时文件已创建");
    }
    for (size_t off = 0; off < c.size(); off += kWriteChunk) {
        int st = fault::at("temp.write");
        if (st) st = injected(st, "写入失败");
        else st = sink->write(c.data() + off, std::min(kWriteChunk, c.size() - off));
        if (st) {
            sink->abort();  // 删除不完整的临时文件
            return st;
        }
    }
    if (int st = fault::at("temp.flush")) {
        sink->abort();
        return injected(st, "落盘失败");
    }
    if (int st = sink->finish(sync)) return st;  // 失败时 sink 删除临时文件
    sink.reset();
    auto drop = [&](int st) {
        fs::remove_file(tmp);
        return st;
    };
    if (int st = fault::at("temp.flushed")) return drop(injected(st, "已落盘"));

    // 读回：与已经自检过的内存容器逐字节相同，等价于对磁盘上的文件再做一遍完整自检
    {
        std::unique_ptr<io::Source> src;
        if (int st = io::open_file_source(tmp.c_str(), src)) return drop(st);
        if (src->size() != c.size()) return drop(fail(IIS_E_IO, "临时文件读回长度不一致：" + tmp));
        std::vector<uint8_t> buf(std::min<size_t>(c.size(), 1u << 20));
        for (size_t off = 0; off < c.size(); off += buf.size()) {
            const size_t n = std::min(buf.size(), c.size() - off);
            if (int st = src->read_at(off, buf.data(), n)) return drop(st);
            if (std::memcmp(buf.data(), c.data() + off, n) != 0)
                return drop(fail(IIS_E_IO, "临时文件读回内容不一致（磁盘或内存错误）：" + tmp));
        }
        if (int st = fault::at("temp.verify")) return drop(injected(st, "读回不一致"));
    }
    if (int st = fault::at("temp.verified")) return drop(injected(st, "已读回"));
    if (int st = fault::at("rename")) return drop(injected(st, "改名失败"));
    if (int st = fs::rename_file(tmp, final_path, replace)) return drop(st);
    fault::at("renamed");  // 之后只用于测试“提交后终止”，不再失败
    return IIS_OK;
}

void release(const std::string& path, uint64_t size, const uint8_t sha[32], iis_file_result& res, std::string& msg) {
    const auto t0 = clock::now();
    std::string why;
    switch (fs::release_if_unchanged(path, size, sha, why)) {
        case fs::Release::Released:
            res.original = IIS_ORIG_RELEASED;
            msg = "已释放原文件";
            break;
        case fs::Release::Busy:
            res.original = IIS_ORIG_BUSY;
            msg = "原文件暂未释放：" + why;
            break;
        case fs::Release::Changed:
            res.original = IIS_ORIG_CHANGED;
            msg = "原文件在压缩后被修改，未释放（" + why + "），需重新压缩";
            break;
        case fs::Release::Gone:
            res.original = IIS_ORIG_GONE;
            msg = "原文件已不存在";
            break;
        case fs::Release::Failed:
            res.original = IIS_ORIG_FAILED;
            msg = "原文件未释放：" + why;
            break;
    }
    res.seconds_release = since(t0);
    fault::at("released");
}

}  // namespace

int compress_file(const std::string& path, const iis_file_options& fo, iis_file_result& res) {
    res = {};
    if (fo.release != IIS_RELEASE_KEEP && fo.release != IIS_RELEASE_DELETE)
        return fail(IIS_E_INVALID_ARG, "release 取值无效");
    const std::string name = fs::base_name(path);
    if (ends_with(name, kIis) || ends_with(name, kTmp)) {
        res.outcome = IIS_FILE_SKIPPED;
        pipeline::set_detail(res.detail, "压缩文件或临时文件，不处理");
        return IIS_OK;
    }
    iis_compress_options co;
    if (int st = pipeline::normalize(&fo.compress, co)) return st;

    auto t0 = clock::now();
    std::vector<uint8_t> data;
    if (int st = fs::read_file(path, data, cont::kMaxRaw)) return st;
    uint8_t sha[32];
    iis_sha256(data.data(), data.size(), sha);
    res.seconds_read = since(t0);
    if (int st = fault::at("read.done")) return injected(st, "已读入");
    if (is_container(data)) {
        res.outcome = IIS_FILE_SKIPPED;
        pipeline::set_detail(res.detail, "内容已是 IIS 容器，不处理");
        return IIS_OK;
    }

    std::string sidecar;
    if (!co.raw_sidecar_json && !fo.no_sidecar) {
        const std::string sp = sidecar_path(path);
        std::vector<uint8_t> s;
        if (sp != path && fs::kind(sp) == fs::Kind::File && fs::read_file(sp, s, kMaxSidecar) == IIS_OK) {
            sidecar.assign(s.begin(), s.end());
            co.raw_sidecar_json = sidecar.c_str();
        }
    }
    if (!co.rel_path_utf8) co.rel_path_utf8 = name.c_str();

    const std::string final_path = path + kIis;
    Existing ex;
    uint64_t csize = 0;
    std::string why, msg;
    if (int st = probe(final_path, data.size(), sha, ex, csize, why)) return st;
    if (ex == Existing::Foreign) return fail(IIS_E_CONFLICT, why);
    if (ex == Existing::Stale && fo.keep_stale)
        return fail(IIS_E_CONFLICT, "已有的 " + fs::base_name(final_path) + " 与原文件内容不同（原文件被同名覆盖过），按设置不替换");

    if (ex == Existing::Match) {
        res.outcome = IIS_FILE_ALREADY;
        res.container_size = csize;
        msg = "已有一致的 .iis";
    } else {
        std::vector<uint8_t> c;
        if (int st = pipeline::compress(data.data(), data.size(), sha, co, c, res.compress)) return st;
        if (int st = fault::at("compress.done")) return injected(st, "已压缩");
        if (res.compress.method == IIS_METHOD_STORE) {
            res.outcome = IIS_FILE_STORED;
            res.original = IIS_ORIG_KEPT;
            pipeline::set_detail(res.detail, ex == Existing::None ? "保留原文件" : "保留原文件；已有的 .iis 是旧内容，未删除");
            return IIS_OK;
        }
        t0 = clock::now();
        // 旧内容（原文件被同名覆盖）或已损坏的 .iis：原文件优先，原子替换
        if (int st = write_and_rename(final_path, c, ex != Existing::None, !fo.no_sync)) return st;
        res.seconds_write = since(t0);
        res.outcome = IIS_FILE_COMPRESSED;
        res.container_size = c.size();
        msg = ex == Existing::Stale ? "已替换旧内容的 .iis" : ex == Existing::Repair ? why : "已提交";
    }
    if (fo.release == IIS_RELEASE_DELETE) {
        std::string r;
        release(path, data.size(), sha, res, r);
        msg += "；" + r;
    }
    pipeline::set_detail(res.detail, msg);
    return IIS_OK;
}

namespace {

struct Recovery {
    const iis_file_options& fo;
    iis_event_callback cb;
    void* user;
    iis_recover_report& rep;

    void event(const std::string& path, const std::string& action) const {
        if (cb) cb(user, path.c_str(), action.c_str());
    }

    // 深度校验：结构、每段 CRC、解码重组、SHA-256
    static int deep_check(const std::string& path, cont::Reader& r) {
        std::unique_ptr<io::Source> src;
        if (int st = io::open_file_source(path.c_str(), src)) return st;
        if (int st = r.open(std::move(src))) return st;
        cont::Buf out;
        return r.restore(out);
    }

    void temp(const std::string& dir, const std::string& name) {
        const std::string target = temp_target(name);
        const std::string tmp = fs::join(dir, name);
        if (target.empty()) {
            ++rep.temps_kept;
            event(tmp, "名字不符合临时文件格式，未处理");
            return;
        }
        const std::string final_path = fs::join(dir, target);
        const std::string orig = final_path.substr(0, final_path.size() - std::strlen(kIis));
        if (fs::kind(orig) != fs::Kind::Missing) {
            if (fs::remove_file(tmp) == IIS_OK) {
                ++rep.temps_removed;
                event(tmp, "原文件在，删除未完成的临时文件");
            } else {
                ++rep.errors;
                event(tmp, std::string("删除临时文件失败：") + iis_last_error());
            }
            return;
        }
        if (fs::kind(final_path) != fs::Kind::Missing) {
            ++rep.temps_kept;
            event(tmp, "原文件不在而 .iis 已存在，临时文件保留待人工确认");
            return;
        }
        cont::Reader r;
        if (deep_check(tmp, r) == IIS_OK && fs::rename_file(tmp, final_path, false) == IIS_OK) {
            ++rep.temps_promoted;
            event(final_path, "原文件不在，临时文件完整且校验通过，改名为 .iis");
        } else {
            ++rep.temps_kept;
            event(tmp, std::string("原文件不在，临时文件不完整或无法改名，保留：") + iis_last_error());
        }
    }

    void pair(const std::string& orig, const std::string& final_path) {
        cont::Reader r;
        if (deep_check(final_path, r) != IIS_OK) {
            ++rep.pending;
            event(final_path, std::string("压缩文件校验失败，原文件保留，待重新压缩：") + iis_last_error());
            return;
        }
        std::string why;
        switch (fs::release_if_unchanged(orig, r.orig_size(), r.orig_sha256(), why)) {
            case fs::Release::Released:
                ++rep.originals_released;
                event(orig, "压缩文件完整且内容一致，释放原文件");
                break;
            case fs::Release::Changed:
                ++rep.pending;
                event(orig, "原文件与 .iis 内容不同（" + why + "），原文件保留，待重新压缩");
                break;
            case fs::Release::Busy:
                ++rep.pending;
                event(orig, "原文件被占用，稍后释放：" + why);
                break;
            case fs::Release::Gone: break;
            case fs::Release::Failed:
                ++rep.errors;
                event(orig, "释放原文件失败：" + why);
                break;
        }
    }

    void dir(const std::string& d, bool recursive, int depth) {
        std::vector<fs::DirEntry> entries;
        if (fs::list_dir(d, entries) != IIS_OK) {
            ++rep.errors;
            event(d, std::string("无法读取目录：") + iis_last_error());
            return;
        }
        std::unordered_set<std::string> files;
        for (const auto& e : entries)
            if (!e.dir && !e.link) files.insert(e.name);
        rep.files_scanned += files.size();
        for (const auto& e : entries)
            if (!e.dir && !e.link && ends_with(e.name, kTmp)) temp(d, e.name);
        if (fo.release == IIS_RELEASE_DELETE) {
            for (const auto& e : entries) {
                if (e.dir || e.link || !ends_with(e.name, kIis) || e.name.size() == std::strlen(kIis)) continue;
                const std::string orig = e.name.substr(0, e.name.size() - std::strlen(kIis));
                if (files.count(orig)) pair(fs::join(d, orig), fs::join(d, e.name));
            }
        }
        if (!recursive) return;
        for (const auto& e : entries) {
            if (!e.dir || e.link) continue;
            if (depth >= kMaxDepth) {
                ++rep.errors;
                event(fs::join(d, e.name), "目录层级过深，未进入");
                continue;
            }
            dir(fs::join(d, e.name), true, depth + 1);
        }
    }
};

}  // namespace

int recover_dir(const std::string& d, bool recursive, const iis_file_options& fo, iis_event_callback cb, void* user,
                iis_recover_report& rep) {
    rep = {};
    if (fs::kind(d) != fs::Kind::Dir) return fail(IIS_E_NOT_FOUND, "目录不存在：" + d);
    Recovery r{fo, cb, user, rep};
    r.dir(d, recursive, 0);
    return IIS_OK;
}

}  // namespace iis::commit

using namespace iis;

extern "C" {

int iis_compress_file(const char* path_utf8, const iis_file_options* opt, iis_file_result* result) {
    IIS_GUARD_BEGIN
    if (!path_utf8 || !*path_utf8) return fail(IIS_E_INVALID_ARG, "路径为空");
    iis_file_options fo{};
    if (opt) fo = *opt;
    iis_file_result r{};
    const int st = commit::compress_file(path_utf8, fo, r);
    if (result) *result = r;
    return st;
    IIS_GUARD_END
}

int iis_recover_dir(const char* dir_utf8, int recursive, const iis_file_options* opt, iis_event_callback cb,
                    void* user, iis_recover_report* report) {
    IIS_GUARD_BEGIN
    if (!dir_utf8 || !*dir_utf8) return fail(IIS_E_INVALID_ARG, "路径为空");
    iis_file_options fo{};
    if (opt) fo = *opt;
    iis_recover_report rep{};
    const int st = commit::recover_dir(dir_utf8, recursive != 0, fo, cb, user, rep);
    if (report) *report = rep;
    return st;
    IIS_GUARD_END
}

}  // extern "C"
