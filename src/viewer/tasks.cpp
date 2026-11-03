#include "tasks.hpp"

#include "i18n.hpp"

#include "iis/iis.h"
#include "package.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace iis::view {

namespace fs = std::filesystem;
using i18n::tr;
using i18n::trf;

namespace {

std::string u8s(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

// 去掉末尾的分隔符（“E:\图像\”），盘符根目录保持不变
fs::path clean(const std::string& path) {
    fs::path p = fs::u8path(path);
    if (!p.has_filename() && p.has_parent_path() && p.parent_path() != p.root_path()) p = p.parent_path();
    return p;
}

std::string human(uint64_t b) {
    char buf[32];
    if (b >= 1000000000ull) std::snprintf(buf, sizeof buf, "%.2f GB", b / 1e9);
    else if (b >= 1000000) std::snprintf(buf, sizeof buf, "%.2f MB", b / 1e6);
    else if (b >= 1000) std::snprintf(buf, sizeof buf, "%.1f KB", b / 1e3);
    else std::snprintf(buf, sizeof buf, "%llu B", (unsigned long long)b);
    return buf;
}

std::string ratio(uint64_t in, uint64_t out) {
    char buf[32];
    std::snprintf(buf, sizeof buf, tr("%.2f 倍", "%.2f×"), out ? double(in) / double(out) : 0.0);
    return buf;
}

bool ends_ci(const std::string& s, const char* e) {
    const size_t n = std::strlen(e);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower(uint8_t(s[s.size() - n + i])) != std::tolower(uint8_t(e[i]))) return false;
    return true;
}

// “名字：说明”
std::string say(const std::string& n, const std::string& what) { return n + tr("：", ": ") + what; }

bool is_iis(const std::string& name) { return ends_ci(name, ".iis"); }
bool is_compressed(const std::string& name) { return is_iis(name) || pkg::is_package_name(name); }

void add_line(TaskState& st, std::string s) {
    std::lock_guard<std::mutex> lk(st.mu);
    st.lines.push_back(std::move(s));
}

void add_problem(TaskState& st, std::string s) {
    std::lock_guard<std::mutex> lk(st.mu);
    if (st.problems.size() < 500) st.problems.push_back(std::move(s));
}

void set_progress(TaskState& st, double f, const std::string& cur) {
    std::lock_guard<std::mutex> lk(st.mu);
    st.fraction = std::clamp(f, 0.0, 1.0);
    st.current = cur;
}

void begin_item(TaskState& st, size_t k) {
    std::lock_guard<std::mutex> lk(st.mu);
    st.item = k;
    st.fraction = 0;
    st.current.clear();
}

void fail(TaskState& st, int n = 1) {
    std::lock_guard<std::mutex> lk(st.mu);
    st.failed += n;
}

void open_here(TaskState& st, const fs::path& dir) {
    std::lock_guard<std::mutex> lk(st.mu);
    st.open_dir = u8s(dir);
}

pkg::ProgressFn pkg_progress(TaskState& st) {
    return [&st](const pkg::Progress& g) {
        const double f = g.bytes_total ? double(g.bytes_done) / double(g.bytes_total)
                                       : g.files_total ? double(g.files_done) / double(g.files_total) : 1.0;
        set_progress(st, f, g.current);
    };
}

// .iis → 旁边的原文件。0 已还原，1 已存在（不覆盖），-1 失败
int restore_one(const fs::path& iis, std::string& why) {
    fs::path target = iis;
    target.replace_extension();
    std::error_code ec;
    if (fs::exists(target, ec)) {
        why = u8s(target.filename()) + tr(" 已存在，未覆盖", " already exists, not overwritten");
        return 1;
    }
    iis_reader* r = nullptr;
    int st = iis_reader_open_file(u8s(iis).c_str(), &r);
    if (st == IIS_OK) st = iis_reader_restore_file(r, u8s(target).c_str(), 1);  // 核对 SHA-256，失败不留下文件
    if (st != IIS_OK) why = iis_last_error();
    iis_reader_close(r);
    if (st == IIS_E_CONFLICT) return 1;
    return st == IIS_OK ? 0 : -1;
}

}  // namespace

std::string compress_preview(const std::string& path) {
    const fs::path p = clean(path);
    const std::string n = u8s(p.filename());
    std::error_code ec;
    if (fs::is_directory(p, ec)) {
        const std::string pk = pkg::unique_path(u8s(p.parent_path() / fs::u8path(n + ".iisz")), false);
        return tr("[文件夹] ", "[Folder] ") + n + " → " + u8s(fs::u8path(pk).filename());
    }
    if (is_compressed(n)) return tr("[跳过] ", "[Skip] ") + n + tr("（已压缩过）", " (already compressed)");
    return tr("[文件] ", "[File] ") + n + " → " + n + ".iis";
}

void run_compress(const std::vector<std::string>& paths, bool delete_originals, const std::atomic<bool>& cancel,
                  TaskState& st) {
    {
        std::lock_guard<std::mutex> lk(st.mu);
        st.items = paths.size();
    }
    for (size_t k = 0; k < paths.size(); ++k) {
        if (cancel) {
            std::lock_guard<std::mutex> lk(st.mu);
            st.cancelled = true;
            break;
        }
        begin_item(st, k);
        const fs::path p = clean(paths[k]);
        const std::string n = u8s(p.filename()), path = u8s(p);
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
            pkg::PackOptions o;
            o.delete_originals = delete_originals;
            o.cancel = &cancel;
            const pkg::PackResult r = pkg::pack_folder(path, "", o, pkg_progress(st));
            for (const auto& e : r.problems) add_problem(st, e);  // 已带“文件夹名/…”相对路径
            if (r.status == pkg::kCancelled) {
                add_line(st, say(n, tr("已取消，没有生成包，原文件未动", "cancelled; no package written, originals untouched")));
                std::lock_guard<std::mutex> lk(st.mu);
                st.cancelled = true;
                break;
            }
            if (r.status) {
                fail(st);
                add_line(st, say(n, tr("失败：", "failed: ") + r.error));
                continue;
            }
            std::string s = n + " → " + say(u8s(fs::u8path(r.package).filename()), trf("%llu 个文件，", "%llu files, ", (unsigned long long)r.files)) +
                            human(r.orig_bytes) + " → " + human(r.package_bytes) + " (" + ratio(r.orig_bytes, r.package_bytes) + ")";
            if (delete_originals) {
                s += trf("，已删除原文件 %llu 个", "; %llu originals deleted", (unsigned long long)r.released);
                if (r.not_released) s += trf("，保留 %llu 个", ", %llu kept", (unsigned long long)r.not_released);
            }
            if (r.unreadable) {
                s += trf("；%llu 个文件读不了，没有打进包", "; %llu files could not be read and were left out", (unsigned long long)r.unreadable);
                fail(st, int(r.unreadable));
            }
            add_line(st, s);
            std::lock_guard<std::mutex> lk(st.mu);
            st.in_bytes += r.orig_bytes;
            st.out_bytes += r.package_bytes;
            st.open_dir = u8s(fs::u8path(r.package).parent_path());
        } else if (fs::is_regular_file(p, ec)) {
            open_here(st, p.parent_path());
            if (is_compressed(n)) {
                add_line(st, say(n, tr("已压缩过，跳过", "already compressed, skipped")));
                continue;
            }
            set_progress(st, 0, n);
            iis_file_options fo{};
            fo.release = delete_originals ? IIS_RELEASE_DELETE : IIS_RELEASE_KEEP;
            fo.keep_stale = 1;  // 已有内容不同的 .iis：不替换
            fo.compress.min_saving = 0.01;
            fo.compress.raw_auto = 1;  // 无文件头的 RAW 自动识别参数
            iis_file_result res{};
            if (iis_compress_file(path.c_str(), &fo, &res) != IIS_OK) {
                fail(st);
                add_line(st, say(n, std::string(tr("失败：", "failed: ")) + iis_last_error()));
                continue;
            }
            std::string s;
            switch (res.outcome) {
                case IIS_FILE_COMPRESSED:
                    s = n + " → " + say(n + ".iis", human(res.compress.orig_size) + " → " + human(res.container_size) + " (" +
                        ratio(res.compress.orig_size, res.container_size) + ")");
                    {
                        std::lock_guard<std::mutex> lk(st.mu);
                        st.in_bytes += res.compress.orig_size;
                        st.out_bytes += res.container_size;
                    }
                    break;
                case IIS_FILE_STORED:
                    s = say(n, tr("压缩后不会更小，没有生成 .iis（原文件不变）", "would not get smaller; no .iis written (original unchanged)"));
                    break;
                case IIS_FILE_ALREADY: s = say(n, trf("已有内容一致的 %s.iis", "an identical %s.iis already exists", n.c_str())); break;
                default: s = say(n, tr("跳过（", "skipped (") + std::string(res.detail) + tr("）", ")")); break;
            }
            if (delete_originals && (res.outcome == IIS_FILE_COMPRESSED || res.outcome == IIS_FILE_ALREADY)) {
                if (res.original == IIS_ORIG_RELEASED) s += tr("，已删除原文件", "; original deleted");
                else if (res.original != IIS_ORIG_GONE)
                    add_problem(st, say(n, tr("未删除原文件（", "original not deleted (") + std::string(res.detail) + tr("）", ")")));
            }
            add_line(st, s);
        } else {
            fail(st);
            add_line(st, say(n, tr("找不到", "not found")));
        }
        set_progress(st, 1, "");
    }
}

void run_extract(const std::vector<std::string>& paths, const std::atomic<bool>& cancel, TaskState& st) {
    {
        std::lock_guard<std::mutex> lk(st.mu);
        st.items = paths.size();
    }
    pkg::ExtractOptions eo;  // 解压到此处
    eo.cancel = &cancel;
    for (size_t k = 0; k < paths.size(); ++k) {
        if (cancel) {
            std::lock_guard<std::mutex> lk(st.mu);
            st.cancelled = true;
            break;
        }
        begin_item(st, k);
        const fs::path p = clean(paths[k]);
        const std::string n = u8s(p.filename()), path = u8s(p);
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
            std::vector<fs::path> found;
            for (const auto& e : fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec)) {
                std::error_code e2;
                if (e.is_regular_file(e2) && is_compressed(u8s(e.path().filename()))) found.push_back(e.path());
            }
            std::sort(found.begin(), found.end());
            int restored = 0, packs = 0, skipped = 0, bad = 0;
            for (size_t i = 0; i < found.size() && !cancel; ++i) {
                const std::string rel = u8s(fs::relative(found[i], p, ec));
                set_progress(st, double(i) / double(found.size()), rel);
                if (pkg::is_package_name(rel)) {
                    const pkg::ExtractResult r = pkg::extract(u8s(found[i]), eo);
                    for (const auto& e : r.problems) add_problem(st, say(rel, e));
                    if (r.status && r.status != pkg::kCancelled) add_problem(st, say(rel, r.error)), ++bad;
                    else ++packs;
                    skipped += int(r.skipped);
                    bad += int(r.failed);
                } else {
                    std::string why;
                    const int rc = restore_one(found[i], why);
                    if (rc == 0) ++restored;
                    else if (rc == 1) ++skipped;
                    else ++bad, add_problem(st, say(rel, why));
                }
            }
            if (found.empty()) {
                add_line(st, say(n, tr("里面没有 .iis 或 .iisz 文件", "no .iis or .iisz files inside")));
            } else {
                std::string s = say(n, trf("还原 %d 个 .iis 文件", "restored %d .iis files", restored));
                if (packs) s += trf("，解开 %d 个包", ", unpacked %d packages", packs);
                if (skipped) s += trf("，跳过 %d 个（原文件已存在）", ", skipped %d (original already exists)", skipped);
                if (bad) s += trf("，失败 %d 个", ", %d failed", bad);
                add_line(st, s);
            }
            fail(st, bad);
            open_here(st, p);
        } else if (pkg::is_package_name(n)) {
            const pkg::ExtractResult r = pkg::extract(path, eo, pkg_progress(st));
            for (const auto& e : r.problems) add_problem(st, say(n, e));
            if (r.status && r.status != pkg::kCancelled) {
                fail(st);
                add_line(st, say(n, tr("失败：", "failed: ") + r.error));
                continue;
            }
            std::string s = n + " → " + say(u8s(fs::u8path(r.out_dir).filename()) + "\\",
                                            trf("解出 %llu 个文件", "extracted %llu files", (unsigned long long)r.ok));
            if (r.skipped) s += trf("，跳过 %llu 个（已存在）", ", skipped %llu (already exist)", (unsigned long long)r.skipped);
            if (r.failed) s += trf("，失败 %llu 个", ", %llu failed", (unsigned long long)r.failed);
            if (r.status == pkg::kCancelled) s += tr("（已取消）", " (cancelled)");
            add_line(st, s);
            fail(st, int(r.failed));
            open_here(st, fs::u8path(r.out_dir));
        } else if (is_iis(n)) {
            std::string why;
            const int rc = restore_one(p, why);
            fs::path t = p.filename();
            t.replace_extension();
            if (rc == 0) add_line(st, n + " → " + u8s(t));
            else if (rc == 1) add_line(st, say(n, why));
            else fail(st), add_line(st, say(n, tr("失败：", "failed: ") + why));
            open_here(st, p.parent_path());
        } else {
            fail(st);
            add_line(st, say(n, tr("不是 .iis 或 .iisz 文件", "not an .iis or .iisz file")));
        }
        set_progress(st, 1, "");
    }
    if (cancel) {
        std::lock_guard<std::mutex> lk(st.mu);
        st.cancelled = true;
    }
}

}  // namespace iis::view
