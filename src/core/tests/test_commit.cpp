// T4：文件原子提交、启动恢复、线程池。故障注入覆盖每一步失败、每一步强制终止进程（子进程）、真实磁盘写满（可选）。
// 核心断言：任何时刻原文件与完整的 .iis 至少存在其一，恢复后状态收敛，还原结果与原文件逐字节相同。
#include "testing.hpp"

#include "iis/iis.h"
#include "internal.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>

#ifdef _WIN32
#  define NOMINMAX
#  include <windows.h>
#  include <shellapi.h>
#  pragma comment(lib, "shell32.lib")
#else
#  include <csignal>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace stdfs = std::filesystem;

namespace {

using Bytes = std::vector<uint8_t>;

stdfs::path P(const std::string& utf8) { return stdfs::u8path(utf8); }
std::string U(const stdfs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

// Windows 下给标准库用的长路径形式（目录树可能超过 260 字符）
stdfs::path longp(const stdfs::path& p) {
#ifdef _WIN32
    return stdfs::path(LR"(\\?\)" + stdfs::absolute(p).make_preferred().wstring());
#else
    return p;
#endif
}

std::string fresh_dir(const std::string& name) {
    const stdfs::path d = stdfs::current_path() / "t4_work" / P(name);
    stdfs::remove_all(longp(d));
    stdfs::create_directories(longp(d));
    return U(d);
}

void write_file(const std::string& path, const Bytes& b) {
    std::ofstream f(P(path), std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

bool exists(const std::string& path) { return stdfs::exists(P(path)); }

Bytes read_file(const std::string& path) {
    std::ifstream f(P(path), std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// 只凭 .iis 还原；失败返回空并记失败
bool restore_equals(const std::string& iis_path, const Bytes& expect) {
    iis_reader* r = nullptr;
    if (iis_reader_open_file(iis_path.c_str(), &r) != IIS_OK) return false;
    iis_buffer out{};
    const bool ok = iis_reader_restore(r, &out) == IIS_OK && out.size == expect.size() &&
                    std::equal(expect.begin(), expect.end(), out.data);
    iis_buffer_free(&out);
    iis_reader_close(r);
    return ok;
}

std::set<std::string> listing(const std::string& dir) {
    std::set<std::string> s;
    for (const auto& e : stdfs::directory_iterator(P(dir))) s.insert(U(e.path().filename()));
    return s;
}

bool has_temp(const std::string& dir) {
    for (const auto& n : listing(dir))
        if (n.size() > 7 && n.compare(n.size() - 7, 7, ".iistmp") == 0) return true;
    return false;
}

// 8 位灰度 BMP：渐变 + 噪声，可压但压不到很小（容器约数百 KB，写临时文件会分多块）
Bytes make_bmp(uint32_t w, uint32_t h, uint32_t seed) {
    std::mt19937 rng(seed);
    const uint32_t stride = (w + 3) & ~3u, off = 14 + 40 + 1024;
    const uint32_t size = off + stride * h;
    Bytes b;
    auto le = [&](uint32_t v, int n) {
        for (int i = 0; i < n; ++i) b.push_back(uint8_t(v >> (8 * i)));
    };
    b.push_back('B');
    b.push_back('M');
    le(size, 4), le(0, 4), le(off, 4), le(40, 4), le(w, 4), le(h, 4), le(1, 2), le(8, 2), le(0, 4);
    le(stride * h, 4), le(2835, 4), le(2835, 4), le(256, 4), le(0, 4);
    for (int i = 0; i < 256; ++i) le(uint32_t(i) * 0x010101u, 4);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) b.push_back(uint8_t((x + y) / 8 + rng() % 24));
        for (uint32_t x = w; x < stride; ++x) b.push_back(0);
    }
    return b;
}

Bytes make_text(size_t n) {
    std::string s;
    while (s.size() < n) s += "station=3 result=OK exposure=1200us gain=2.0 frame=" + std::to_string(s.size()) + "\n";
    return Bytes(s.begin(), s.begin() + long(n));
}

iis_file_options opts(int release) {
    iis_file_options o{};
    o.release = release;
    return o;
}

// ---- 故障注入钩子 ----
struct Fault {
    std::string point;
    int nth = 1, seen = 0, status = 0;
    bool kill = false;
    std::set<std::string> always;  // 这些步骤每次都返回 status
    std::string touch;             // 非空：到达 point 时向该文件追加 1 字节（模拟原文件被改写）
};
Fault g_fault;

[[noreturn]] void kill_self() {
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), 77);
#else
    kill(getpid(), SIGKILL);
#endif
    std::abort();
}

int hook(const char* p) {
    if (g_fault.always.count(p)) return g_fault.status;
    if (g_fault.point != p || ++g_fault.seen != g_fault.nth) return 0;
    if (g_fault.kill) kill_self();
    if (!g_fault.touch.empty()) {
        std::ofstream f(P(g_fault.touch), std::ios::binary | std::ios::app);
        f.put('x');
        return 0;
    }
    return g_fault.status;
}

struct HookScope {
    explicit HookScope(Fault f) {
        g_fault = std::move(f);
        iis::fault::set_hook(&hook);
    }
    ~HookScope() {
        iis::fault::set_hook(nullptr);
        g_fault = {};
    }
};

// ---- 子进程：在指定步骤强制终止 ----
int run_child(const std::string& mode, const std::string& point, int nth, const std::string& path, int release) {
#ifdef _WIN32
    wchar_t exe[32768];
    GetModuleFileNameW(nullptr, exe, 32768);
    auto wide = [](const std::string& s) {
        std::wstring w(s.size() + 1, L'\0');
        w.resize(size_t(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), int(w.size()))));
        return w;
    };
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --fault-child " + wide(mode) + L" " + wide(point) + L" " +
                       std::to_wstring(nth) + L" \"" + wide(path) + L"\" " + std::to_wstring(release);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return int(code);
#else
    char exe[4096];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return -1;
    exe[n] = 0;
    const std::string snth = std::to_string(nth), srel = std::to_string(release);
    const pid_t pid = fork();
    if (pid == 0) {
        execl(exe, exe, "--fault-child", mode.c_str(), point.c_str(), snth.c_str(), path.c_str(), srel.c_str(),
              static_cast<char*>(nullptr));
        _exit(126);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) return WTERMSIG(status) == SIGKILL ? 77 : 128 + WTERMSIG(status);
    return WEXITSTATUS(status);
#endif
}

}  // namespace

// test_main 遇到 --fault-child 时调用：mode 为 compress 或 recover
int fault_child_main(int argc, char** argv) {
#ifdef _WIN32
    // main 的 argv 是本地代码页（中文系统为 GBK），路径要按 UTF-8 重新取
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    std::vector<std::string> args;
    for (int i = 0; i < wargc; ++i) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string a(size_t(n), 0);
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, a.data(), n, nullptr, nullptr);
        a.resize(size_t(n - 1));
        args.push_back(std::move(a));
    }
    LocalFree(wargv);
    std::vector<char*> ptrs;
    for (auto& a : args) ptrs.push_back(a.data());
    argc = int(ptrs.size());
    argv = ptrs.data();
#endif
    if (argc < 7) return 2;
    Fault f;
    f.point = argv[3];
    f.nth = std::atoi(argv[4]);
    f.kill = true;
    HookScope scope(f);
    const std::string mode = argv[2], path = argv[5];
    const iis_file_options o = opts(std::atoi(argv[6]));
    if (mode == "compress") {
        iis_file_result r{};
        return iis_compress_file(path.c_str(), &o, &r) == IIS_OK ? 0 : 3;
    }
    iis_recover_report rep{};
    return iis_recover_dir(path.c_str(), 1, &o, nullptr, nullptr, &rep) == IIS_OK ? 0 : 3;
}

TEST(commit_basic) {
    const std::string d = fresh_dir("basic");
    const std::string f = d + "/相机1_图像.bmp";
    const Bytes data = make_bmp(300, 200, 1);
    write_file(f, data);

    iis_file_options keep = opts(IIS_RELEASE_KEEP);
    iis_file_result r{};
    CHECK_EQ(iis_compress_file(f.c_str(), &keep, &r), IIS_OK);
    CHECK_EQ(r.outcome, IIS_FILE_COMPRESSED);
    CHECK_EQ(r.compress.method, IIS_METHOD_IMAGE);
    CHECK_EQ(r.original, IIS_ORIG_KEPT);
    CHECK(exists(f) && read_file(f) == data);
    CHECK(restore_equals(f + ".iis", data));
    CHECK_EQ(uint64_t(r.container_size), uint64_t(stdfs::file_size(P(f + ".iis"))));
    CHECK(!has_temp(d));

    // 再处理一次：已有一致的 .iis，不重复压缩
    CHECK_EQ(iis_compress_file(f.c_str(), &keep, &r), IIS_OK);
    CHECK_EQ(r.outcome, IIS_FILE_ALREADY);

    // 释放模式：已有一致的 .iis → 只释放原文件
    iis_file_options del = opts(IIS_RELEASE_DELETE);
    CHECK_EQ(iis_compress_file(f.c_str(), &del, &r), IIS_OK);
    CHECK_EQ(r.outcome, IIS_FILE_ALREADY);
    CHECK_EQ(r.original, IIS_ORIG_RELEASED);
    CHECK(!exists(f));
    CHECK(restore_equals(f + ".iis", data));

    // 原文件不存在
    CHECK_EQ(iis_compress_file(f.c_str(), &del, &r), IIS_E_NOT_FOUND);

    // 一步完成压缩 + 释放
    const std::string g = d + "/b.bmp";
    const Bytes data2 = make_bmp(123, 77, 2);
    write_file(g, data2);
    CHECK_EQ(iis_compress_file(g.c_str(), &del, &r), IIS_OK);
    CHECK_EQ(r.outcome, IIS_FILE_COMPRESSED);
    CHECK_EQ(r.original, IIS_ORIG_RELEASED);
    CHECK(!exists(g) && restore_equals(g + ".iis", data2));
    CHECK(listing(d) == (std::set<std::string>{"相机1_图像.bmp.iis", "b.bmp.iis"}));
}

TEST(commit_store_zstd_threshold) {
    const std::string d = fresh_dir("store");
    iis_file_options del = opts(IIS_RELEASE_DELETE);
    iis_file_result r{};
    // 随机数据：压不小 → 不生成 .iis，原文件保留
    const std::string a = d + "/noise.dat";
    const Bytes noise = testing::random_bytes(200000, 5);
    write_file(a, noise);
    CHECK_EQ(iis_compress_file(a.c_str(), &del, &r), IIS_OK);
    CHECK_EQ(r.outcome, IIS_FILE_STORED);
    CHECK_EQ(r.original, IIS_ORIG_KEPT);
    CHECK(read_file(a) == noise && !exists(a + ".iis"));
    // 文本：整文件 zstd
    const std::string b = d + "/log.txt";
    const Bytes text = make_text(100000);
    write_file(b, text);
    CHECK_EQ(iis_compress_file(b.c_str(), &del, &r), IIS_OK);
    CHECK_EQ(r.compress.method, IIS_METHOD_ZSTD);
    CHECK(!exists(b) && restore_equals(b + ".iis", text));
    // 收益阈值：BMP 压缩比约 1.5~2，要求省 90% 时不压
    const std::string c = d + "/img.bmp";
    const Bytes img = make_bmp(200, 150, 3);
    write_file(c, img);
    iis_file_options strict = del;
    strict.compress.min_saving = 0.9;
    CHECK_EQ(iis_compress_file(c.c_str(), &strict, &r), IIS_OK);
    CHECK_EQ(r.outcome, IIS_FILE_STORED);
    CHECK(read_file(c) == img && !exists(c + ".iis"));
    strict.compress.min_saving = 0.95;  // 超出范围
    CHECK_EQ(iis_compress_file(c.c_str(), &strict, &r), IIS_E_INVALID_ARG);
    strict.compress.min_saving = 0.2;
    CHECK_EQ(iis_compress_file(c.c_str(), &strict, &r), IIS_OK);
    CHECK_EQ(r.outcome, IIS_FILE_COMPRESSED);
    CHECK(restore_equals(c + ".iis", img));
    // 空文件
    const std::string e = d + "/empty.bin";
    write_file(e, {});
    CHECK_EQ(iis_compress_file(e.c_str(), &del, &r), IIS_OK);
    CHECK_EQ(r.outcome, IIS_FILE_STORED);
    CHECK(exists(e));
    CHECK(!has_temp(d));
}

TEST(commit_skip) {
    const std::string d = fresh_dir("skip");
    iis_file_options del = opts(IIS_RELEASE_DELETE);
    iis_file_result r{};
    const std::string a = d + "/a.bmp";
    write_file(a, make_bmp(64, 64, 4));
    CHECK_EQ(iis_compress_file(a.c_str(), &del, &r), IIS_OK);
    // .iis 本身、临时文件名、内容是容器的文件：都不处理
    CHECK_EQ(iis_compress_file((a + ".iis").c_str(), &del, &r), IIS_OK);
    CHECK_EQ(r.outcome, IIS_FILE_SKIPPED);
    const std::string t = d + "/x.bmp.iis.0badf00d.iistmp";
    write_file(t, {1, 2, 3});
    CHECK_EQ(iis_compress_file(t.c_str(), &del, &r), IIS_OK);
    CHECK_EQ(r.outcome, IIS_FILE_SKIPPED);
    const std::string c = d + "/copy.dat";
    write_file(c, read_file(a + ".iis"));
    CHECK_EQ(iis_compress_file(c.c_str(), &del, &r), IIS_OK);
    CHECK_EQ(r.outcome, IIS_FILE_SKIPPED);
    CHECK(exists(c) && exists(t));
}

TEST(commit_existing_target) {
    const std::string d = fresh_dir("existing");
    iis_file_options del = opts(IIS_RELEASE_DELETE);
    iis_file_result r{};
    const std::string f = d + "/img.bmp";
    const Bytes v1 = make_bmp(160, 120, 10), v2 = make_bmp(160, 120, 11);
    write_file(f, v1);
    CHECK_EQ(iis_compress_file(f.c_str(), &del, &r), IIS_OK);
    // 视觉软件后来写了同名新图：原文件优先，替换旧 .iis
    write_file(f, v2);
    iis_file_options keep_stale = del;
    keep_stale.keep_stale = 1;
    CHECK_EQ(iis_compress_file(f.c_str(), &keep_stale, &r), IIS_E_CONFLICT);
    CHECK(read_file(f) == v2 && restore_equals(f + ".iis", v1));
    CHECK_EQ(iis_compress_file(f.c_str(), &del, &r), IIS_OK);
    CHECK_EQ(r.outcome, IIS_FILE_COMPRESSED);
    CHECK_EQ(r.original, IIS_ORIG_RELEASED);
    CHECK(!exists(f) && restore_equals(f + ".iis", v2));

    // 已有同名但不是 IIS 容器的文件：不碰任何文件
    const std::string g = d + "/other.bmp";
    write_file(g, v1);
    write_file(g + ".iis", {'n', 'o', 't', ' ', 'o', 'u', 'r', 's'});
    CHECK_EQ(iis_compress_file(g.c_str(), &del, &r), IIS_E_CONFLICT);
    CHECK(read_file(g) == v1 && read_file(g + ".iis").size() == 8);
    // 同名目录
    const std::string h = d + "/dir.bmp";
    write_file(h, v1);
    stdfs::create_directory(P(h + ".iis"));
    CHECK_EQ(iis_compress_file(h.c_str(), &del, &r), IIS_E_CONFLICT);
    CHECK(read_file(h) == v1);

    // 内容相同但已损坏的 .iis：用原文件重新生成
    const std::string k = d + "/broken.bmp";
    write_file(k, v1);
    iis_file_options keep = opts(IIS_RELEASE_KEEP);
    CHECK_EQ(iis_compress_file(k.c_str(), &keep, &r), IIS_OK);
    Bytes c = read_file(k + ".iis");
    c[c.size() / 2] ^= 0x40;  // 像素段中间
    write_file(k + ".iis", c);
    CHECK_EQ(iis_compress_file(k.c_str(), &del, &r), IIS_OK);
    CHECK_EQ(r.outcome, IIS_FILE_COMPRESSED);
    CHECK_EQ(r.original, IIS_ORIG_RELEASED);
    CHECK(restore_equals(k + ".iis", v1));
    CHECK(!has_temp(d));
}

TEST(commit_source_changed) {
    const std::string d = fresh_dir("changed");
    const std::string f = d + "/img.bmp";
    const Bytes v1 = make_bmp(200, 100, 20);
    write_file(f, v1);
    iis_file_options del = opts(IIS_RELEASE_DELETE);
    iis_file_result r{};
    {
        // 压缩期间（已读入之后）原文件被追加写入：提交照常，但原文件不释放
        Fault fl;
        fl.point = "temp.verified";
        fl.touch = f;
        HookScope s(fl);
        CHECK_EQ(iis_compress_file(f.c_str(), &del, &r), IIS_OK);
    }
    CHECK_EQ(r.outcome, IIS_FILE_COMPRESSED);
    CHECK_EQ(r.original, IIS_ORIG_CHANGED);
    Bytes v1x = v1;
    v1x.push_back('x');
    CHECK(read_file(f) == v1x);
    // 恢复：不一致 → 原文件保留，计入待重新压缩
    iis_recover_report rep{};
    CHECK_EQ(iis_recover_dir(d.c_str(), 1, &del, nullptr, nullptr, &rep), IIS_OK);
    CHECK_EQ(rep.pending, uint64_t(1));
    CHECK_EQ(rep.originals_released, uint64_t(0));
    CHECK(read_file(f) == v1x);
    // 再压缩：替换旧 .iis 并释放
    CHECK_EQ(iis_compress_file(f.c_str(), &del, &r), IIS_OK);
    CHECK_EQ(r.original, IIS_ORIG_RELEASED);
    CHECK(!exists(f) && restore_equals(f + ".iis", v1x));
}

TEST(commit_injected_failures) {
    const std::string d = fresh_dir("inject");
    const std::string f = d + "/img.bmp";
    const Bytes data = make_bmp(900, 700, 30);  // 容器超过一个写入块
    write_file(f, data);
    const iis_file_options del = opts(IIS_RELEASE_DELETE);
    struct C {
        const char* point;
        int nth, status;
    };
    const C cases[] = {{"read.done", 1, IIS_E_IO},     {"compress.done", 1, IIS_E_IO},  {"temp.create", 1, IIS_E_IO},
                       {"temp.created", 1, IIS_E_IO},  {"temp.write", 1, IIS_E_NO_SPACE}, {"temp.write", 2, IIS_E_NO_SPACE},
                       {"temp.flush", 1, IIS_E_NO_SPACE}, {"temp.flushed", 1, IIS_E_IO}, {"temp.verify", 1, IIS_E_IO},
                       {"temp.verified", 1, IIS_E_IO}, {"rename", 1, IIS_E_IO}};
    for (const auto& c : cases) {
        Fault fl;
        fl.point = c.point;
        fl.nth = c.nth;
        fl.status = c.status;
        HookScope s(fl);
        iis_file_result r{};
        const int st = iis_compress_file(f.c_str(), &del, &r);
        if (st != c.status) std::printf("  步骤 %s#%d 返回 %d\n", c.point, c.nth, st);
        CHECK_EQ(st, c.status);
        CHECK(g_fault.seen >= c.nth);
        // 原文件不变，没有 .iis，也没有残留临时文件
        CHECK(listing(d) == std::set<std::string>{"img.bmp"});
        CHECK(read_file(f) == data);
    }
    // 释放阶段的失败：.iis 已提交，原文件保留，之后由恢复释放
    for (const char* p : {"release.locked", "release.hashed"}) {
        {
            Fault fl;
            fl.point = p;
            fl.status = IIS_E_IO;
            HookScope s(fl);
            iis_file_result r{};
            CHECK_EQ(iis_compress_file(f.c_str(), &del, &r), IIS_OK);
            CHECK_EQ(r.original, IIS_ORIG_FAILED);
        }
        CHECK(read_file(f) == data && restore_equals(f + ".iis", data));
        iis_recover_report rep{};
        CHECK_EQ(iis_recover_dir(d.c_str(), 0, &del, nullptr, nullptr, &rep), IIS_OK);
        CHECK_EQ(rep.originals_released, uint64_t(1));
        CHECK(!exists(f));
        write_file(f, data);
        stdfs::remove(P(f + ".iis"));
    }
}

TEST(commit_encode_failures) {
    const std::string d = fresh_dir("encode");
    const std::string f = d + "/img.bmp";
    const Bytes data = make_bmp(300, 200, 40);
    write_file(f, data);
    const iis_file_options keep = opts(IIS_RELEASE_KEEP);
    struct C {
        std::set<std::string> points;
        int method;
    };
    const C cases[] = {{{"encode.image"}, IIS_METHOD_ZSTD},
                       {{"verify.image"}, IIS_METHOD_ZSTD},
                       {{"encode.image", "encode.zstd"}, IIS_METHOD_STORE},
                       {{"verify.image", "verify.zstd"}, IIS_METHOD_STORE}};
    for (const auto& c : cases) {
        stdfs::remove(P(f + ".iis"));
        Fault fl;
        fl.always = c.points;
        fl.status = IIS_E_CODEC;
        HookScope s(fl);
        iis_file_result r{};
        CHECK_EQ(iis_compress_file(f.c_str(), &keep, &r), IIS_OK);
        CHECK_EQ(r.compress.method, c.method);
        CHECK(read_file(f) == data);
        if (c.method == IIS_METHOD_STORE) CHECK(!exists(f + ".iis"));
        else CHECK(restore_equals(f + ".iis", data));
    }
    // 内存接口同样兜底
    Fault fl;
    fl.always = {"encode.image"};
    fl.status = IIS_E_CODEC;
    HookScope s(fl);
    iis_buffer c{};
    iis_compress_result cr{};
    CHECK_EQ(iis_compress(data.data(), data.size(), nullptr, &c, &cr), IIS_OK);
    CHECK_EQ(cr.method, IIS_METHOD_ZSTD);
    iis_buffer_free(&c);
}

// 每一步强制终止进程：之后任何时刻都满足“原文件或完整 .iis 至少其一”；恢复 + 重做后收敛到只剩 .iis
TEST(commit_kill_each_step) {
    struct Kind {
        const char* name;
        Bytes data;
    };
    const Kind kinds[] = {{"img.bmp", make_bmp(900, 700, 50)}, {"log.txt", make_text(300000)}};
    struct Step {
        const char* point;
        int nth;
    };
    const Step steps[] = {{"read.done", 1},   {"encode.image", 1},  {"verify.image", 1}, {"compress.done", 1},
                          {"temp.created", 1}, {"temp.write", 1},    {"temp.write", 2},   {"temp.flushed", 1},
                          {"temp.verified", 1}, {"renamed", 1},      {"release.locked", 1}, {"release.hashed", 1},
                          {"released", 1}};
    int killed = 0;
    for (const auto& k : kinds) {
        for (const auto& s : steps) {
            for (int stale = 0; stale < 2; ++stale) {
                const std::string d = fresh_dir(std::string("kill/") + k.name + "_" + s.point + std::to_string(s.nth) +
                                                (stale ? "_stale" : ""));
                const std::string f = d + "/" + k.name;
                if (stale) {
                    // 先有一个旧内容的 .iis，原文件随后被同名覆盖
                    Bytes old = k.data;
                    old[old.size() / 3] ^= 0xFF;
                    write_file(f, old);
                    iis_file_result r{};
                    const iis_file_options del = opts(IIS_RELEASE_DELETE);
                    CHECK_EQ(iis_compress_file(f.c_str(), &del, &r), IIS_OK);
                }
                write_file(f, k.data);
                const int code = run_child("compress", s.point, s.nth, f, IIS_RELEASE_DELETE);
                if (code == 77) ++killed;
                else CHECK_EQ(code, 0);  // 没走到该步骤（如文本没有图像编码）：正常完成
                const bool orig_ok = exists(f) && read_file(f) == k.data;
                const bool iis_ok = exists(f + ".iis") && restore_equals(f + ".iis", k.data);
                if (!(orig_ok || iis_ok))
                    std::printf("  %s 在 %s#%d 终止后原文件与 .iis 都不可用\n", k.name, s.point, s.nth);
                CHECK(orig_ok || iis_ok);
                if (exists(f)) CHECK(read_file(f) == k.data);  // 原文件从不被改动

                // 恢复（其中再在释放原文件时终止一次）
                const int rc = run_child("recover", "release.hashed", 1, d, IIS_RELEASE_DELETE);
                CHECK(rc == 0 || rc == 77);
                CHECK((exists(f) && read_file(f) == k.data) || restore_equals(f + ".iis", k.data));
                iis_recover_report rep{};
                const iis_file_options del = opts(IIS_RELEASE_DELETE);
                CHECK_EQ(iis_recover_dir(d.c_str(), 1, &del, nullptr, nullptr, &rep), IIS_OK);
                CHECK_EQ(rep.errors, uint64_t(0));
                CHECK(!has_temp(d));
                if (exists(f)) {
                    iis_file_result r{};
                    CHECK_EQ(iis_compress_file(f.c_str(), &del, &r), IIS_OK);
                }
                CHECK(listing(d) == std::set<std::string>{std::string(k.name) + ".iis"});
                CHECK(restore_equals(f + ".iis", k.data));
            }
        }
    }
    std::printf("  共 %d 次在指定步骤终止进程\n", killed);
    CHECK(killed >= 40);
}

TEST(recover_temp_cases) {
    const std::string d = fresh_dir("recover");
    const std::string sub = d + "/子目录/深一层";
    stdfs::create_directories(P(sub));
    const Bytes data = make_bmp(100, 80, 60);
    iis_buffer c{};
    iis_compress_result cr{};
    CHECK_EQ(iis_compress(data.data(), data.size(), nullptr, &c, &cr), IIS_OK);
    const Bytes cont(c.data, c.data + c.size);
    iis_buffer_free(&c);

    // 1) 原文件在 + 临时文件（完整或不完整）→ 删除临时文件
    write_file(sub + "/a.bmp", data);
    write_file(sub + "/a.bmp.iis.0000abcd.iistmp", cont);
    write_file(sub + "/a.bmp.iis.0000abce.iistmp", Bytes(cont.begin(), cont.begin() + 100));
    // 2) 原文件不在、.iis 不在、临时文件完整 → 改名为 .iis
    write_file(d + "/b.bmp.iis.12345678.iistmp", cont);
    // 3) 原文件不在、临时文件不完整 → 保留
    write_file(d + "/c.bmp.iis.87654321.iistmp", Bytes(cont.begin(), cont.end() - 1));
    // 4) 原文件不在、.iis 已在 → 保留临时文件
    write_file(d + "/e.bmp.iis", cont);
    write_file(d + "/e.bmp.iis.aaaaaaaa.iistmp", cont);
    // 5) 名字不合格式 → 不动
    write_file(d + "/weird.iistmp", {1});
    // 6) 原文件 + 一致的 .iis → 释放原文件；7) 原文件 + 别的内容的 .iis → 保留
    write_file(d + "/f.bmp", data);
    write_file(d + "/f.bmp.iis", cont);
    write_file(d + "/g.bmp", make_bmp(100, 80, 61));
    write_file(d + "/g.bmp.iis", cont);
    // 8) 原文件 + 损坏的 .iis → 保留
    Bytes bad = cont;
    bad[bad.size() / 2] ^= 1;
    write_file(d + "/h.bmp", data);
    write_file(d + "/h.bmp.iis", bad);
    // 9) 普通文件不动
    write_file(d + "/readme.txt", {'h', 'i'});

    // 只保留模式：只处理临时文件，不释放原文件
    std::vector<std::string> events;
    auto cb = [](void* u, const char* path, const char* action) {
        static_cast<std::vector<std::string>*>(u)->push_back(std::string(path) + "：" + action);
    };
    iis_recover_report rep{};
    const iis_file_options keep = opts(IIS_RELEASE_KEEP);
    CHECK_EQ(iis_recover_dir(d.c_str(), 0, &keep, cb, &events, &rep), IIS_OK);
    CHECK_EQ(rep.temps_removed, uint64_t(0));  // 不递归：子目录未处理
    CHECK_EQ(rep.temps_promoted, uint64_t(1));
    CHECK_EQ(rep.temps_kept, uint64_t(3));
    CHECK_EQ(rep.originals_released, uint64_t(0));
    CHECK(exists(d + "/f.bmp"));
    CHECK(restore_equals(d + "/b.bmp.iis", data));

    const iis_file_options del = opts(IIS_RELEASE_DELETE);
    CHECK_EQ(iis_recover_dir(d.c_str(), 1, &del, cb, &events, &rep), IIS_OK);
    CHECK_EQ(rep.temps_removed, uint64_t(2));
    CHECK_EQ(rep.temps_promoted, uint64_t(0));
    CHECK_EQ(rep.temps_kept, uint64_t(3));
    CHECK_EQ(rep.originals_released, uint64_t(1));
    CHECK_EQ(rep.pending, uint64_t(2));
    CHECK_EQ(rep.errors, uint64_t(0));
    CHECK(listing(sub) == std::set<std::string>{"a.bmp"});
    CHECK(!exists(d + "/f.bmp") && exists(d + "/g.bmp") && exists(d + "/h.bmp"));
    CHECK(exists(d + "/c.bmp.iis.87654321.iistmp") && exists(d + "/e.bmp.iis.aaaaaaaa.iistmp"));
    CHECK(exists(d + "/weird.iistmp") && exists(d + "/readme.txt"));
    CHECK(events.size() >= 10);
    CHECK_EQ(iis_recover_dir((d + "/没有这个目录").c_str(), 1, &del, nullptr, nullptr, &rep), IIS_E_NOT_FOUND);
}

TEST(commit_sidecar) {
    const std::string d = fresh_dir("sidecar");
    const uint32_t w = 320, h = 240;
    std::mt19937 rng(70);
    Bytes raw;
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t v = (x * 7 + y * 5 + rng() % 64) & 0xFFF;
            raw.push_back(uint8_t(v));
            raw.push_back(uint8_t(v >> 8));
        }
    write_file(d + "/cam.raw", raw);
    const std::string side = R"({"width": 320, "height": 240, "pixel_format": "Mono16"})";
    write_file(d + "/cam.json", Bytes(side.begin(), side.end()));
    iis_file_options o = opts(IIS_RELEASE_KEEP);
    iis_file_result r{};
    CHECK_EQ(iis_compress_file((d + "/cam.raw").c_str(), &o, &r), IIS_OK);
    CHECK_EQ(r.compress.method, IIS_METHOD_IMAGE);
    CHECK(restore_equals(d + "/cam.raw.iis", raw));
    // 关闭自动读取：无参数的 RAW 只能整文件兜底
    stdfs::remove(P(d + "/cam.raw.iis"));
    o.no_sidecar = 1;
    CHECK_EQ(iis_compress_file((d + "/cam.raw").c_str(), &o, &r), IIS_OK);
    CHECK(r.compress.method != IIS_METHOD_IMAGE);
}

TEST(commit_long_path) {
    // 超过 260 字符的中文长路径
    std::string d = fresh_dir("long");
    for (int i = 0; i < 12; ++i) d += "/很长的目录名称_" + std::to_string(i) + "_abcdefghij";
    stdfs::create_directories(longp(P(d)));
    const std::string f = d + "/图像.bmp";
    CHECK(f.size() > 300);
    const Bytes data = make_bmp(120, 90, 80);
    // 用内核自己的文件写入（标准库的 ofstream 不支持长路径）
    iis_buffer c{};
    CHECK_EQ(iis_pack_bytes(data.data(), data.size(), 0, &c), IIS_OK);
    iis_reader* rd = nullptr;
    CHECK_EQ(iis_reader_open_memory(c.data, c.size, &rd), IIS_OK);
    CHECK_EQ(iis_reader_restore_file(rd, f.c_str(), 0), IIS_OK);
    iis_reader_close(rd);
    iis_buffer_free(&c);

    const iis_file_options del = opts(IIS_RELEASE_DELETE);
    iis_file_result r{};
    CHECK_EQ(iis_compress_file(f.c_str(), &del, &r), IIS_OK);
    CHECK_EQ(r.outcome, IIS_FILE_COMPRESSED);
    CHECK_EQ(r.original, IIS_ORIG_RELEASED);
    CHECK(restore_equals(f + ".iis", data));
    iis_recover_report rep{};
    CHECK_EQ(iis_recover_dir(U(stdfs::current_path() / "t4_work" / "long").c_str(), 1, &del, nullptr, nullptr, &rep),
             IIS_OK);
    CHECK_EQ(rep.files_scanned, uint64_t(1));
    CHECK_EQ(rep.errors, uint64_t(0));
    stdfs::remove_all(longp(stdfs::current_path() / "t4_work" / "long"));
}

#ifdef _WIN32
TEST(commit_release_busy) {
    const std::string d = fresh_dir("busy");
    const std::string f = d + "/img.bmp";
    const Bytes data = make_bmp(200, 100, 90);
    write_file(f, data);
    // 别的程序以只共享读的方式打开着原文件（如查看器）：提交照常，原文件暂不释放
    HANDLE h = CreateFileW(P(f).wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    const iis_file_options del = opts(IIS_RELEASE_DELETE);
    iis_file_result r{};
    CHECK_EQ(iis_compress_file(f.c_str(), &del, &r), IIS_OK);
    CHECK_EQ(r.outcome, IIS_FILE_COMPRESSED);
    CHECK_EQ(r.original, IIS_ORIG_BUSY);
    iis_recover_report rep{};
    CHECK_EQ(iis_recover_dir(d.c_str(), 0, &del, nullptr, nullptr, &rep), IIS_OK);
    CHECK_EQ(rep.pending, uint64_t(1));
    CHECK(exists(f));
    CloseHandle(h);
    CHECK_EQ(iis_recover_dir(d.c_str(), 0, &del, nullptr, nullptr, &rep), IIS_OK);
    CHECK_EQ(rep.originals_released, uint64_t(1));
    CHECK(!exists(f) && restore_equals(f + ".iis", data));

    // 视觉软件正在写（未共享读）：读不了，返回占用，什么都不改
    write_file(f + "2", data);
    h = CreateFileW(P(f + "2").wstring().c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK_EQ(iis_compress_file((f + "2").c_str(), &del, &r), IIS_E_BUSY);
    CloseHandle(h);
    CHECK(read_file(f + "2") == data && !exists(f + "2.iis"));

    // 只读原文件：不删除
    const std::string g = d + "/ro.bmp";
    write_file(g, data);
    SetFileAttributesW(P(g).wstring().c_str(), FILE_ATTRIBUTE_READONLY);
    CHECK_EQ(iis_compress_file(g.c_str(), &del, &r), IIS_OK);
    CHECK_EQ(r.original, IIS_ORIG_FAILED);
    CHECK(exists(g));
    SetFileAttributesW(P(g).wstring().c_str(), FILE_ATTRIBUTE_NORMAL);
}
#endif

// 真实磁盘写满：设置 IIS_TEST_SMALL_DIR 为一个很小的文件系统（Linux CI 用 1 MB tmpfs）时运行
TEST(commit_disk_full) {
    const char* small_dir = std::getenv("IIS_TEST_SMALL_DIR");
    if (!small_dir || !*small_dir) {
        std::printf("  未设置 IIS_TEST_SMALL_DIR，跳过\n");
        return;
    }
    const std::string d = std::string(small_dir) + "/full";
    stdfs::remove_all(P(d));
    stdfs::create_directories(P(d));
    const std::string f = d + "/img.bmp";
    const Bytes data = make_bmp(800, 600, 100);  // 约 480 KB，容器约 250 KB
    write_file(f, data);
    // 用填充文件占满剩余空间，只留几 KB
    {
        std::ofstream fill(P(d + "/fill.bin"), std::ios::binary);
        const Bytes chunk(4096, 0x55);
        while (stdfs::space(P(d)).available > 16384 && fill.write(reinterpret_cast<const char*>(chunk.data()), 4096))
            fill.flush();
    }
    const iis_file_options del = opts(IIS_RELEASE_DELETE);
    iis_file_result r{};
    const int st = iis_compress_file(f.c_str(), &del, &r);
    std::printf("  剩余 %llu 字节时压缩返回 %d：%s\n", (unsigned long long)stdfs::space(P(d)).available, st,
                st ? iis_last_error() : "");
    CHECK_EQ(st, IIS_E_NO_SPACE);
    CHECK(read_file(f) == data);
    CHECK(!exists(f + ".iis") && !has_temp(d));
    // 腾出空间后成功
    stdfs::remove(P(d + "/fill.bin"));
    CHECK_EQ(iis_compress_file(f.c_str(), &del, &r), IIS_OK);
    CHECK(!exists(f) && restore_equals(f + ".iis", data));
    stdfs::remove_all(P(d));
}

TEST(pool_many_files) {
    const std::string d = fresh_dir("pool");
    std::map<std::string, Bytes> files;
    for (int i = 0; i < 48; ++i) {
        const std::string sub = d + "/工位" + std::to_string(i % 3);
        stdfs::create_directories(P(sub));
        Bytes b = i % 4 == 3 ? testing::random_bytes(30000, uint32_t(i)) : i % 4 == 2 ? make_text(50000 + size_t(i))
                                                                                       : make_bmp(200 + uint32_t(i), 150, uint32_t(i));
        const std::string f = sub + "/f" + std::to_string(i) + (i % 4 < 2 ? ".bmp" : ".dat");
        write_file(f, b);
        files[f] = std::move(b);
    }
    struct Seen {
        std::mutex mu;
        std::map<std::string, int> count;
        int errors = 0;
    } seen;
    auto cb = [](void* u, const char* path, int status, const iis_file_result*, const char* err) {
        auto* s = static_cast<Seen*>(u);
        std::lock_guard<std::mutex> lk(s->mu);
        ++s->count[path];
        if (status != IIS_OK) {
            ++s->errors;
            std::printf("  %s：%d %s\n", path, status, err);
        }
    };
    const iis_file_options del = opts(IIS_RELEASE_DELETE);
    iis_pool* pool = nullptr;
    CHECK_EQ(iis_pool_create(0, &del, cb, &seen, &pool), IIS_E_INVALID_ARG);
    CHECK_EQ(iis_pool_create(4, &del, cb, &seen, &pool), IIS_OK);
    iis_pool_pause(pool, 1);
    for (int round = 0; round < 2; ++round)  // 重复提交被忽略
        for (const auto& kv : files) CHECK_EQ(iis_pool_submit(pool, kv.first.c_str(), nullptr), IIS_OK);
    CHECK_EQ(iis_pool_pending(pool), files.size());
    iis_pool_pause(pool, 0);
    iis_pool_wait(pool);
    CHECK_EQ(iis_pool_pending(pool), size_t(0));
    iis_pool_destroy(pool);
    CHECK_EQ(seen.count.size(), files.size());
    CHECK_EQ(seen.errors, 0);
    for (const auto& kv : seen.count) CHECK_EQ(kv.second, 1);
    for (const auto& kv : files) {
        if (exists(kv.first)) CHECK(read_file(kv.first) == kv.second && !exists(kv.first + ".iis"));  // 压不小
        else CHECK(restore_equals(kv.first + ".iis", kv.second));
    }
    // 销毁时丢弃排队中的文件
    CHECK_EQ(iis_pool_create(1, &del, nullptr, nullptr, &pool), IIS_OK);
    iis_pool_pause(pool, 1);
    CHECK_EQ(iis_pool_submit(pool, (d + "/不存在.bmp").c_str(), nullptr), IIS_OK);
    iis_pool_destroy(pool);
}
