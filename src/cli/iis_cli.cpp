// iis-cli：命令行工具（T8），不依赖后台服务，可用于任何目录（含拷出来的 U 盘、移动硬盘）。
//
//   iis-cli info <文件.iis>…                         容器信息：原文件大小与 SHA-256、格式、像素平面、各段
//   iis-cli verify <文件或目录>… [-r] [--deep]        结构与每段 CRC；--deep 再完整还原并比对 SHA-256（.iisz 包同样适用）
//   iis-cli restore <文件或目录>… [-r] [-o 输出目录]   还原为原文件（SHA-256 一致才写出，已存在不覆盖）
//   iis-cli compress <文件或目录>… [-r] [--release] [--rules 规则.json] [--min-saving 0.05]
//                                                     逐个压缩为同目录的 .iis（默认保留原文件；--release 提交后删除原文件）
//   iis-cli pack <文件夹>… [-o 包.iisz] [--delete]    每个文件夹打成一个包（.iisz，标准 ZIP），默认在文件夹旁边；
//                                                     --delete：包写好并核对后，删除内容未变的原文件与变空的文件夹
//   iis-cli unpack <包.iisz>… [-o 输出目录]           解包。默认解到包旁边（同名文件夹已存在时加 (2)）；已存在的文件不覆盖
//   iis-cli list <包.iisz>…                           列出包内文件
//   iis-cli export <源目录> <输出目录> [--copy] [--from 日期] [--to 日期] [--include 通配]…
//                                                     批量导出：默认把 .iis 还原为原始格式、其他文件照抄；--copy 原样复制
//
// 退出码：0 全部成功；1 有失败；2 用法错误。
#include "iis/iis.h"

#include "formats.hpp"  // glob_match
#include "package.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#  define NOMINMAX
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <shellapi.h>
#  pragma comment(lib, "shell32.lib")
#endif

namespace fs = std::filesystem;

namespace {

std::string u8(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

bool ends_with(const std::string& s, const std::string& e) {
    if (s.size() < e.size()) return false;
    for (size_t i = 0; i < e.size(); ++i)
        if (tolower(uint8_t(s[s.size() - e.size() + i])) != tolower(uint8_t(e[i]))) return false;
    return true;
}

std::string hex(const uint8_t* d, size_t n) {
    static const char* x = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) s += x[d[i] >> 4], s += x[d[i] & 15];
    return s;
}

std::string human(uint64_t b) {
    char buf[32];
    if (b >= 1000000000ull) std::snprintf(buf, sizeof buf, "%.2f GB", b / 1e9);
    else if (b >= 1000000) std::snprintf(buf, sizeof buf, "%.2f MB", b / 1e6);
    else if (b >= 1000) std::snprintf(buf, sizeof buf, "%.1f KB", b / 1e3);
    else std::snprintf(buf, sizeof buf, "%llu B", (unsigned long long)b);
    return buf;
}

struct Opts {
    std::vector<std::string> paths, includes;
    bool recursive = false, deep = false, release = false, copy = false;
    std::string out, rules, from, to;
    double min_saving = 0;
};

// 展开命令行里的文件 / 目录（目录按 -r 决定是否进子目录）
std::vector<fs::path> expand(const std::vector<std::string>& paths, bool recursive, bool want_iis) {
    std::vector<fs::path> out;
    for (const auto& p : paths) {
        const fs::path fp = fs::u8path(p);
        std::error_code ec;
        if (fs::is_directory(fp, ec)) {
            auto take = [&](const fs::directory_entry& e) {
                if (!e.is_regular_file()) return;
                const std::string n = u8(e.path().filename());
                if (ends_with(n, ".iistmp")) return;
                if (want_iis == ends_with(n, ".iis")) out.push_back(e.path());
            };
            if (recursive)
                for (const auto& e : fs::recursive_directory_iterator(fp, fs::directory_options::skip_permission_denied, ec))
                    take(e);
            else
                for (const auto& e : fs::directory_iterator(fp, ec)) take(e);
        } else {
            out.push_back(fp);
        }
    }
    return out;
}

int cmd_info(const Opts& o) {
    int bad = 0;
    for (const auto& p : expand(o.paths, o.recursive, true)) {
        iis_reader* r = nullptr;
        if (iis_reader_open_file(u8(p).c_str(), &r) != IIS_OK) {
            std::printf("%s：不是有效的 IIS 容器：%s\n", u8(p).c_str(), iis_last_error());
            ++bad;
            continue;
        }
        iis_container_info info{};
        iis_reader_info(r, &info);
        iis_buffer recipe{};
        std::string fmt = "?";
        if (iis_reader_recipe(r, &recipe) == IIS_OK) {
            const std::string t(reinterpret_cast<char*>(recipe.data), recipe.size);
            const size_t k = t.find("\"format\":\"");
            if (k != std::string::npos) fmt = t.substr(k + 10, t.find('"', k + 10) - k - 10);
        }
        iis_buffer_free(&recipe);
        std::printf("%s\n", u8(p).c_str());
        std::printf("  容器版本 %u.%u，%s\n", info.version_major, info.version_minor,
                    info.restorable ? "本程序可还原" : "含本程序不支持的段，不能还原（需更新版本）");
        std::printf("  原文件 %s（%llu 字节），SHA-256 %s\n", human(info.orig_size).c_str(),
                    (unsigned long long)info.orig_size, hex(info.orig_sha256, 32).c_str());
        std::printf("  容器 %s，压缩比 %.2f 倍，原格式 %s\n", human(info.container_size).c_str(),
                    info.container_size ? double(info.orig_size) / double(info.container_size) : 0.0, fmt.c_str());
        static const char* types[] = {"?", "配方", "原样字节", "像素平面", "附加字节", "描述信息"};
        static const char* codecs[] = {"无压缩", "zstd", "JPEG-LS", "HTJ2K", "JPEG XL"};
        for (uint32_t i = 0; i < info.segment_count; ++i) {
            iis_segment_info s{};
            iis_reader_segment(r, i, &s);
            std::printf("  段 %-2u %-8s id %-3u %-8s %10s → %s\n", i, s.type < 6 ? types[s.type] : "未知",
                        s.id, s.codec < 5 ? codecs[s.codec] : "?", human(s.data_len).c_str(), human(s.raw_len).c_str());
        }
        iis_reader_close(r);
    }
    return bad ? 1 : 0;
}

int cmd_verify(const Opts& o) {
    int ok = 0, bad = 0;
    std::vector<std::string> plain;
    for (const auto& a : o.paths) {
        if (!iis::pkg::is_package_name(a)) {
            plain.push_back(a);
            continue;
        }
        const auto r = iis::pkg::verify(a, o.deep);
        if (r.status) {
            ++bad;
            std::printf("损坏：%s：%s\n", a.c_str(), r.error.c_str());
            continue;
        }
        for (const auto& e : r.problems) std::printf("损坏：%s：%s\n", a.c_str(), e.c_str());
        ok += int(r.ok), bad += int(r.failed);
    }
    for (const auto& p : expand(plain, o.recursive, true)) {
        iis_reader* r = nullptr;
        int st = iis_reader_open_file(u8(p).c_str(), &r);
        if (st == IIS_OK) st = iis_reader_verify(r, o.deep ? 1 : 0);
        if (st == IIS_OK) ++ok;
        else {
            ++bad;
            std::printf("损坏：%s：%s\n", u8(p).c_str(), iis_last_error());
        }
        iis_reader_close(r);
    }
    std::printf("校验%s：%d 个完好，%d 个有问题\n", o.deep ? "（完整还原 + SHA-256）" : "（结构 + CRC）", ok, bad);
    return bad ? 1 : 0;
}

int cmd_restore(const Opts& o) {
    int ok = 0, bad = 0, skipped = 0;
    for (const auto& p : expand(o.paths, o.recursive, true)) {
        fs::path target = p;
        target.replace_extension();  // 去掉 .iis
        if (!o.out.empty()) {
            // 输出目录：保持相对于命令行所给目录的层次
            fs::path rel = target.filename();
            for (const auto& root : o.paths) {
                const fs::path rp = fs::u8path(root);
                std::error_code ec;
                if (fs::is_directory(rp, ec)) {
                    const auto r = fs::relative(target, rp, ec);
                    if (!ec && !r.empty() && r.native()[0] != '.') rel = r;
                }
            }
            target = fs::u8path(o.out) / rel;
            std::error_code ec;
            fs::create_directories(target.parent_path(), ec);
        }
        std::error_code ec;
        if (fs::exists(target, ec)) {
            ++skipped;
            std::printf("跳过（已存在，不覆盖）：%s\n", u8(target).c_str());
            continue;
        }
        iis_reader* r = nullptr;
        int st = iis_reader_open_file(u8(p).c_str(), &r);
        if (st == IIS_OK) st = iis_reader_restore_file(r, u8(target).c_str(), 1);
        iis_reader_close(r);
        if (st == IIS_OK) ++ok;
        else {
            ++bad;
            std::printf("失败：%s：%s\n", u8(p).c_str(), iis_last_error());
        }
    }
    std::printf("还原 %d 个（均已比对 SHA-256），跳过 %d，失败 %d\n", ok, skipped, bad);
    return bad ? 1 : 0;
}

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

int cmd_compress(const Opts& o) {
    iis_raw_rules* rules = nullptr;
    if (!o.rules.empty()) {
        const std::string t = slurp(fs::u8path(o.rules));
        if (iis_raw_rules_parse(t.data(), t.size(), &rules) != IIS_OK) {
            std::printf("RAW 规则有误：%s\n", iis_last_error());
            return 2;
        }
    }
    iis_file_options fo{};
    fo.compress.raw_rules = rules;
    fo.compress.min_saving = o.min_saving;
    fo.compress.raw_auto = 1;  // 没有规则与参数文件的无文件头 RAW：自动识别
    fo.release = o.release ? IIS_RELEASE_DELETE : IIS_RELEASE_KEEP;
    fo.keep_stale = 1;  // 已有内容不同的 .iis：不替换，报冲突
    int bad = 0, done = 0;
    uint64_t in = 0, out = 0;
    for (const auto& p : expand(o.paths, o.recursive, false)) {
        // 相对路径用于匹配 RAW 规则
        std::string rel = u8(p.filename());
        for (const auto& root : o.paths) {
            std::error_code ec;
            if (fs::is_directory(fs::u8path(root), ec)) {
                const auto r = fs::relative(p, fs::u8path(root), ec);
                if (!ec && !r.empty()) rel = r.generic_u8string();
            }
        }
        fo.compress.rel_path_utf8 = rel.c_str();
        iis_file_result res{};
        const int st = iis_compress_file(u8(p).c_str(), &fo, &res);
        if (st != IIS_OK) {
            ++bad;
            std::printf("失败：%s：%s\n", u8(p).c_str(), iis_last_error());
            continue;
        }
        ++done;
        static const char* outcome[] = {"", "已压缩", "压不小，保留原文件", "已有一致的 .iis", "跳过"};
        if (res.outcome == IIS_FILE_COMPRESSED) in += res.compress.orig_size, out += res.container_size;
        std::printf("%s：%s%s%s\n", u8(p).c_str(), outcome[res.outcome & 7],
                    res.outcome == IIS_FILE_COMPRESSED
                        ? ("，" + human(res.compress.orig_size) + " → " + human(res.container_size)).c_str()
                        : "",
                    res.original == IIS_ORIG_RELEASED ? "，已删除原文件" : "");
    }
    iis_raw_rules_free(rules);
    std::printf("处理 %d 个，失败 %d；压缩部分 %s → %s（%.2f 倍）\n", done, bad, human(in).c_str(), human(out).c_str(),
                out ? double(in) / double(out) : 0.0);
    return bad ? 1 : 0;
}

int cmd_pack(const Opts& o) {
    if (!o.out.empty() && o.paths.size() != 1) return std::printf("-o 只能用于一个文件夹\n"), 2;
    int bad = 0;
    for (const auto& p : o.paths) {
        iis::pkg::PackOptions po;
        po.delete_originals = o.release;
        const auto r = iis::pkg::pack_folder(p, o.out, po);
        for (const auto& e : r.problems) std::printf("  %s\n", e.c_str());
        if (r.status) {
            ++bad;
            std::printf("失败：%s：%s\n", p.c_str(), r.error.c_str());
            continue;
        }
        std::printf("%s → %s：%llu 个文件（图像压缩 %llu，zstd %llu，原样 %llu），%s → %s（%.2f 倍），%.1f 秒\n", p.c_str(),
                    r.package.c_str(), (unsigned long long)r.files, (unsigned long long)r.as_image,
                    (unsigned long long)r.as_zstd, (unsigned long long)r.as_stored, human(r.orig_bytes).c_str(),
                    human(r.package_bytes).c_str(), r.package_bytes ? double(r.orig_bytes) / double(r.package_bytes) : 0.0,
                    r.seconds);
        if (r.unreadable) ++bad, std::printf("  %llu 个文件读不了，没有打进包\n", (unsigned long long)r.unreadable);
        if (o.release)
            std::printf("  已删除原文件 %llu 个%s\n", (unsigned long long)r.released,
                        r.not_released ? ("，保留 " + std::to_string(r.not_released) + " 个（见上）").c_str() : "");
    }
    return bad ? 1 : 0;
}

int cmd_unpack(const Opts& o) {
    int bad = 0;
    for (const auto& p : o.paths) {
        iis::pkg::ExtractOptions eo;
        eo.here = o.out.empty();
        eo.dest = o.out;
        const auto r = iis::pkg::extract(p, eo);
        if (r.status) {
            ++bad;
            std::printf("失败：%s：%s\n", p.c_str(), r.error.c_str());
            continue;
        }
        for (const auto& e : r.problems) std::printf("  %s\n", e.c_str());
        std::printf("%s → %s：解出 %llu 个（均已核对 SHA-256），跳过（已存在）%llu，失败 %llu，%.1f 秒\n", p.c_str(),
                    r.out_dir.c_str(), (unsigned long long)r.ok, (unsigned long long)r.skipped,
                    (unsigned long long)r.failed, r.seconds);
        if (r.failed) ++bad;
    }
    return bad ? 1 : 0;
}

int cmd_list(const Opts& o) {
    int bad = 0;
    for (const auto& p : o.paths) {
        iis::pkg::Reader rd;
        if (rd.open(p)) {
            ++bad;
            std::printf("%s：%s\n", p.c_str(), rd.error().c_str());
            continue;
        }
        std::printf("%s\n", p.c_str());
        uint64_t in = 0, out = 0, n = 0;
        for (const auto& e : rd.entries()) {
            if (e.dir) continue;
            ++n, in += e.orig_size, out += e.size;
            std::printf("  %10s → %10s  %s  %s%s\n", human(e.orig_size).c_str(), human(e.size).c_str(),
                        e.iis ? "压缩" : "原样", e.path.c_str(), e.bad.empty() ? "" : ("  [不安全的名称：" + e.bad + "]").c_str());
        }
        std::printf("  共 %llu 个文件，%s → %s（%.2f 倍）\n", (unsigned long long)n, human(in).c_str(), human(out).c_str(),
                    out ? double(in) / double(out) : 0.0);
    }
    return bad ? 1 : 0;
}

bool parse_day(const std::string& s, std::time_t& t) {
    std::tm tm{};
    if (std::sscanf(s.c_str(), "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) != 3) return false;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    tm.tm_isdst = -1;
    t = std::mktime(&tm);
    return t != -1;
}

int cmd_export(const Opts& o) {
    if (o.paths.size() != 2) {
        std::printf("用法：iis-cli export <源目录> <输出目录> [--copy] [--from 日期] [--to 日期] [--include 通配]…\n");
        return 2;
    }
    const fs::path src = fs::u8path(o.paths[0]), dst = fs::u8path(o.paths[1]);
    std::time_t from = 0, to = 0;
    if (!o.from.empty() && !parse_day(o.from, from)) return std::printf("--from 应为 YYYY-MM-DD\n"), 2;
    if (!o.to.empty()) {
        if (!parse_day(o.to, to)) return std::printf("--to 应为 YYYY-MM-DD\n"), 2;
        to += 86400;
    }
    int ok = 0, bad = 0, skipped = 0;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(src, fs::directory_options::skip_permission_denied, ec)) {
        if (!e.is_regular_file()) continue;
        const std::string rel = fs::relative(e.path(), src, ec).generic_u8string();
        if (ends_with(rel, ".iistmp")) continue;
        const bool is_iis = ends_with(rel, ".iis");
        const std::string logical = is_iis ? rel.substr(0, rel.size() - 4) : rel;
        if (!o.includes.empty()) {
            bool hit = false;
            for (const auto& g : o.includes) hit = hit || iis::fmt::glob_match(g, logical);
            if (!hit) continue;
        }
        if (from || to) {
            // 按修改时间筛选（.iis 的修改时间即压缩时间，接近原文件写入时间）
            const auto ft = fs::last_write_time(e.path(), ec);
            const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
            const std::time_t t = std::chrono::system_clock::to_time_t(sys);
            if ((from && t < from) || (to && t >= to)) continue;
        }
        const bool restore = is_iis && !o.copy;
        const fs::path out = dst / fs::u8path(restore ? logical : rel);
        fs::create_directories(out.parent_path(), ec);
        if (fs::exists(out, ec)) {
            ++skipped;
            continue;
        }
        int st = IIS_OK;
        if (restore) {
            iis_reader* r = nullptr;
            st = iis_reader_open_file(u8(e.path()).c_str(), &r);
            if (st == IIS_OK) st = iis_reader_restore_file(r, u8(out).c_str(), 1);
            iis_reader_close(r);
        } else if (!fs::copy_file(e.path(), out, fs::copy_options::none, ec)) {
            st = IIS_E_IO;
        }
        if (st == IIS_OK) ++ok;
        else {
            ++bad;
            std::printf("失败：%s：%s\n", rel.c_str(), restore ? iis_last_error() : ec.message().c_str());
        }
    }
    std::printf("导出 %d 个%s，跳过已存在 %d，失败 %d\n", ok, o.copy ? "（原样复制）" : "（.iis 已还原并比对 SHA-256）", skipped, bad);
    return bad ? 1 : 0;
}

void usage() {
    std::printf(
        "iis-cli %s —— 三溪缩影命令行工具\n"
        "  info <文件.iis>…                         查看容器信息\n"
        "  verify <文件或目录>… [-r] [--deep]        校验（--deep：完整还原并比对 SHA-256）\n"
        "  restore <文件或目录>… [-r] [-o 输出目录]   还原为原文件（已存在不覆盖）\n"
        "  compress <文件或目录>… [-r] [--release] [--rules 规则.json] [--min-saving 0.05]\n"
        "  pack <文件夹>… [-o 包.iisz] [--delete]     打成一个包（默认保留原文件）\n"
        "  unpack <包.iisz>… [-o 输出目录]            解包（默认解到包旁边，不覆盖）\n"
        "  list <包.iisz>…                           列出包内文件\n"
        "  export <源目录> <输出目录> [--copy] [--from 日期] [--to 日期] [--include 通配]…\n",
        iis_version());
}

int run(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        usage();
        return 2;
    }
    Opts o;
    const std::string cmd = args[1];
    for (size_t i = 2; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto val = [&]() -> std::string { return i + 1 < args.size() ? args[++i] : std::string(); };
        if (a == "-r" || a == "--recursive") o.recursive = true;
        else if (a == "--deep") o.deep = true;
        else if (a == "--release" || a == "--delete") o.release = true;
        else if (a == "--copy") o.copy = true;
        else if (a == "-o" || a == "--out") o.out = val();
        else if (a == "--rules") o.rules = val();
        else if (a == "--from") o.from = val();
        else if (a == "--to") o.to = val();
        else if (a == "--include") o.includes.push_back(val());
        else if (a == "--min-saving") o.min_saving = std::atof(val().c_str());
        else if (a.size() > 1 && a[0] == '-') return std::printf("不认识的选项 %s\n", a.c_str()), 2;
        else o.paths.push_back(a);
    }
    if (o.paths.empty()) {
        usage();
        return 2;
    }
    if (cmd == "info") return cmd_info(o);
    if (cmd == "verify") return cmd_verify(o);
    if (cmd == "restore") return cmd_restore(o);
    if (cmd == "compress") return cmd_compress(o);
    if (cmd == "export") return cmd_export(o);
    if (cmd == "pack") return cmd_pack(o);
    if (cmd == "unpack") return cmd_unpack(o);
    if (cmd == "list") return cmd_list(o);
    usage();
    return 2;
}

}  // namespace

#ifdef _WIN32
int main() {
    SetConsoleOutputCP(CP_UTF8);
    int n = 0;
    LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);  // 按 UTF-16 取参数，中文路径不乱
    std::vector<std::string> args;
    for (int i = 0; i < n; ++i) args.push_back(u8(fs::path(w[i])));
    LocalFree(w);
    return run(args);
}
#else
int main(int argc, char** argv) { return run(std::vector<std::string>(argv, argv + argc)); }
#endif
