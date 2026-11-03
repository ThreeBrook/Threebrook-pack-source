// T5 后台服务引擎测试：配置检查、写完判定（延迟 / 稳定 / 正在写）、事件溢出、目录删除重建、启动恢复、
// 检测进程 CPU 高时暂停、剩余空间保护、吞吐上限、保留模式不重复处理、系统时间回拨。
// 时间参数都缩到秒级；结果只凭 .iis 还原并与写入内容逐字节比对。
#include "testing.hpp"

#include "audit.hpp"
#include "auth.hpp"
#include "classify.hpp"
#include "config.hpp"
#include "engine.hpp"
#include "host.hpp"
#include "license.hpp"
#include "index.hpp"
#include "storage.hpp"
#include "iis/iis.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>

#include <nlohmann/json.hpp>
#include <thread>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN  // 不带旧版 winsock.h，管理界面测试要用 winsock2
#include <winsock2.h>
#include <windows.h>

namespace stdfs = std::filesystem;
using namespace iis::svc;

namespace {

using Bytes = std::vector<uint8_t>;

std::string U(const stdfs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}
stdfs::path P(const std::string& s) { return stdfs::u8path(s); }

std::string fresh_dir(const std::string& name) {
    const stdfs::path d = stdfs::current_path() / "t5_work" / P(name);
    std::error_code ec;
    stdfs::remove_all(d, ec);
    stdfs::create_directories(d);
    return U(d);
}

void write_file(const std::string& path, const Bytes& b) {
    stdfs::create_directories(P(path).parent_path());
    std::ofstream f(P(path), std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

Bytes read_file(const std::string& path) {
    std::ifstream f(P(path), std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool exists(const std::string& p) { return stdfs::exists(P(p)); }

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

Bytes make_bmp(uint32_t w, uint32_t h, uint32_t seed) {
    std::mt19937 rng(seed);
    const uint32_t stride = (w + 3) & ~3u, off = 14 + 40 + 1024;
    Bytes b;
    auto le = [&](uint32_t v, int n) {
        for (int i = 0; i < n; ++i) b.push_back(uint8_t(v >> (8 * i)));
    };
    b.push_back('B');
    b.push_back('M');
    le(off + stride * h, 4), le(0, 4), le(off, 4), le(40, 4), le(w, 4), le(h, 4), le(1, 2), le(8, 2), le(0, 4);
    le(stride * h, 4), le(2835, 4), le(2835, 4), le(256, 4), le(0, 4);
    for (int i = 0; i < 256; ++i) le(uint32_t(i) * 0x010101u, 4);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) b.push_back(uint8_t((x + y) / 4 + rng() % 16));
        for (uint32_t x = w; x < stride; ++x) b.push_back(0);
    }
    return b;
}

bool wait_for(const std::function<bool()>& pred, double seconds) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(int(seconds * 1000));
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return pred();
}

// 原文件已释放、.iis 还原等于期望内容
bool compressed_ok(const std::string& path, const Bytes& expect) {
    return !exists(path) && exists(path + ".iis") && restore_equals(path + ".iis", expect);
}

Config quick_config(const std::string& root) {
    Config c;
    WatchDir w;
    w.path = root;
    w.include = default_include();
    c.watch.push_back(w);
    c.delay_s = 0.5;
    c.stable_s = 0.2;
    c.retry_s = 0.3;
    c.rescan_s = 0;
    c.threads = 2;
    c.priority = "normal";  // 测试进程本身不要降到后台
    c.min_free_mb = 0;
    c.log_dir = root + "_logs";
    c.status_file = c.log_dir + "\\status.json";
    c.status_s = 0.5;
    return c;
}

struct Running {
    Log log;
    std::unique_ptr<Engine> engine;
    explicit Running(const Config& c) {
        log.open(c.log_dir, 30, false);
        engine = std::make_unique<Engine>(c, log);
        CHECK(engine->start());
        engine->set_licensed(true);  // 引擎测试按已授权运行；授权本身见 test_license.inc
    }
    ~Running() { engine->stop(); }
    Snapshot snap() const { return engine->snapshot(); }
};

}  // namespace

TEST(config_parse) {
    const std::string base = fresh_dir("config");
    Config c;
    std::string err;
    CHECK(parse_config(R"({"watch": [{"path": "图像"}], "delay_s": 60, "pause": {"processes": ["Vision.exe"]}})", base, c,
                       err));
    CHECK_EQ(c.watch.size(), size_t(1));
    CHECK(c.watch[0].path == U(P(base) / P("图像")));
    CHECK(c.watch[0].include == default_include());
    CHECK_EQ(c.delay_s, 60.0);
    CHECK(c.release);
    CHECK(c.log_dir == U(P(base) / "logs"));
    CHECK_EQ(c.pause.processes.size(), size_t(1));
    CHECK(parse_config(R"({"watch": []})", base, c, err));  // 刚安装：还没配置监控目录
    CHECK(c.watch.empty());
    const char* bad[] = {
        R"({})",
        R"({"watch": {}})",
        R"({"watch": [{"path": "a"}], "dealy_s": 5})",           // 拼错的字段
        R"({"watch": [{"path": "a", "recurse": true}]})",
        R"({"watch": [{"path": "a"}], "priority": "high"})",
        R"({"watch": [{"path": "a"}], "threads": 0})",
        R"({"watch": [{"path": "a"}], "min_saving": 0.95})",
        R"({"watch": [{"path": "a"}, {"path": "a"}]})",
        R"({"watch": [{"path": "a"}], "pause": {"pause_percent": 30, "resume_percent": 50}})",
        R"({"watch": [{"path": "a"}], "release": "yes"})",
        R"(not json)",
    };
    for (const char* b : bad) {
        if (parse_config(b, base, c, err)) std::printf("  应拒绝：%s\n", b);
        else CHECK(!err.empty());
    }
    // 记事本保存的 BOM
    const std::string f = base + "/c.json";
    std::string text = "\xEF\xBB\xBF{\"watch\": [{\"path\": \"x\", \"include\": [\"*\"]}], \"release\": false}";
    write_file(f, Bytes(text.begin(), text.end()));
    CHECK(load_config(f, c, err));
    CHECK(!c.release);
    CHECK(c.watch[0].include == std::vector<std::string>{"*"});
}

TEST(engine_basic) {
    const std::string root = fresh_dir("basic");
    Config c = quick_config(root);
    c.watch[0].exclude = {"skip/*"};
    std::map<std::string, Bytes> files;
    files[root + "\\启动前.bmp"] = make_bmp(200, 100, 1);  // 启动时已存在：由扫描发现
    for (const auto& kv : files) write_file(kv.first, kv.second);
    Running r(c);
    files[root + "\\a.bmp"] = make_bmp(300, 200, 2);
    files[root + "\\子目录\\深\\b.BMP"] = make_bmp(120, 90, 3);  // 扩展名大小写不敏感
    for (const auto& kv : files) write_file(kv.first, kv.second);
    const Bytes text = {'l', 'o', 'g'};
    write_file(root + "\\vision.log", text);         // 不在默认范围内
    write_file(root + "\\skip\\c.bmp", make_bmp(64, 64, 4));  // 排除
    CHECK(wait_for([&] {
        for (const auto& kv : files)
            if (!compressed_ok(kv.first, kv.second)) return false;
        return true;
    }, 30));
    std::this_thread::sleep_for(std::chrono::seconds(1));
    CHECK(read_file(root + "\\vision.log") == text);
    CHECK(exists(root + "\\skip\\c.bmp") && !exists(root + "\\skip\\c.bmp.iis"));
    const Snapshot s = r.snap();
    CHECK_EQ(s.compressed, uint64_t(3));
    CHECK_EQ(s.released, uint64_t(3));
    CHECK_EQ(s.errors, uint64_t(0));
    CHECK_EQ(s.roots_online, 1);
    CHECK(wait_for([&] { return exists(c.status_file); }, 5));
}

TEST(engine_open_and_growing) {
    const std::string root = fresh_dir("busy");
    Config c = quick_config(root);
    c.delay_s = 1;
    Running r(c);
    // 1) 视觉软件以写方式开着文件（不共享写）：一直不处理，关闭后才处理
    const std::string a = root + "\\open.bmp";
    const Bytes da = make_bmp(200, 150, 10);
    HANDLE h = CreateFileW(P(a).wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, 0, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    DWORD n = 0;
    WriteFile(h, da.data(), DWORD(da.size()), &n, nullptr);
    // 2) 持续追加的文件：在追加期间不处理
    const std::string b = root + "\\growing.bmp";
    Bytes db = make_bmp(400, 300, 11);
    {
        std::ofstream f(P(b), std::ios::binary);
        f.write(reinterpret_cast<const char*>(db.data()), 1000);
    }
    for (size_t off = 1000; off < db.size(); off += 8000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        std::ofstream f(P(b), std::ios::binary | std::ios::app);
        f.write(reinterpret_cast<const char*>(db.data() + off), std::streamsize(std::min<size_t>(8000, db.size() - off)));
        CHECK(!exists(b + ".iis"));
    }
    std::this_thread::sleep_for(std::chrono::seconds(2));
    CHECK(exists(a) && !exists(a + ".iis"));  // 仍开着
    CloseHandle(h);
    CHECK(wait_for([&] { return compressed_ok(a, da) && compressed_ok(b, db); }, 20));
    CHECK_EQ(r.snap().errors, uint64_t(0));
}

TEST(engine_overflow_many_files) {
    const std::string root = fresh_dir("many");
    Config c = quick_config(root);
    c.threads = 4;
    c.watch_buffer = 1024;  // 缓冲区调到最小、每批事件后停顿：保证溢出
    c.watch_stall_ms = 100;
    Running r(c);
    // 短时间写大量小文件：目录事件缓冲区溢出，靠全量扫描补上
    const int n = 3000;
    const Bytes img = make_bmp(48, 40, 20);
    for (int i = 0; i < 10; ++i) stdfs::create_directories(P(root + "\\d" + std::to_string(i)));
    std::vector<std::thread> writers;  // 8 个线程同时写，事件密集到达
    for (int t = 0; t < 8; ++t)
        writers.emplace_back([&, t] {
            for (int i = t; i < n; i += 8)
                write_file(root + "\\d" + std::to_string(i % 10) + "\\f" + std::to_string(i) + ".bmp", img);
        });
    for (auto& w : writers) w.join();
    CHECK(wait_for([&] { return r.snap().compressed >= uint64_t(n); }, 180));
    int ok = 0;
    for (int i = 0; i < n; i += 37)
        ok += compressed_ok(root + "\\d" + std::to_string(i % 10) + "\\f" + std::to_string(i) + ".bmp", img);
    CHECK_EQ(ok, (n + 36) / 37);
    CHECK_EQ(r.snap().errors, uint64_t(0));
    int overflows = 0;
    for (const auto& e : stdfs::directory_iterator(P(c.log_dir)))
        if (e.path().extension() == ".log") {
            const Bytes b = read_file(U(e.path()));
            const std::string t(b.begin(), b.end());
            for (size_t at = 0; (at = t.find("目录事件溢出", at)) != std::string::npos; ++at) ++overflows;
        }
    std::printf("  事件溢出 %d 次（均由全量扫描补上）\n", overflows);
    CHECK(overflows > 0);
}

TEST(engine_root_recreated) {
    const std::string root = fresh_dir("recreate");
    Config c = quick_config(root);
    Running r(c);
    const Bytes d1 = make_bmp(100, 80, 30), d2 = make_bmp(100, 80, 31);
    write_file(root + "\\one.bmp", d1);
    CHECK(wait_for([&] { return compressed_ok(root + "\\one.bmp", d1); }, 20));
    // 监控目录被整个删除，过一会儿重建（如人工清理）
    std::error_code ec;
    CHECK(wait_for([&] { return stdfs::remove_all(P(root), ec) != uintmax_t(-1) && !exists(root); }, 10));
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    CHECK(wait_for([&] { return r.snap().roots_online == 0; }, 10));
    stdfs::create_directories(P(root));
    write_file(root + "\\two.bmp", d2);
    CHECK(wait_for([&] { return compressed_ok(root + "\\two.bmp", d2); }, 30));
    CHECK_EQ(r.snap().roots_online, 1);
}

TEST(engine_startup_recovery) {
    const std::string root = fresh_dir("recovery");
    const Bytes d = make_bmp(150, 100, 40);
    // 上次运行留下：原文件 + 完整 .iis（释放前崩溃），另一个文件的残留临时文件
    write_file(root + "\\a.bmp", d);
    iis_file_options keep{};
    iis_file_result fr{};
    CHECK_EQ(iis_compress_file((root + "\\a.bmp").c_str(), &keep, &fr), IIS_OK);
    write_file(root + "\\b.bmp", d);
    write_file(root + "\\b.bmp.iis.0123abcd.iistmp", {1, 2, 3});
    Config c = quick_config(root);
    c.delay_s = 3600;  // 恢复不依赖写完判定
    Running r(c);
    CHECK(!exists(root + "\\a.bmp") && restore_equals(root + "\\a.bmp.iis", d));
    CHECK(!exists(root + "\\b.bmp.iis.0123abcd.iistmp"));
    CHECK(exists(root + "\\b.bmp"));  // 修改时间太新，还没到压缩时间
}

TEST(engine_pause_on_inspection_cpu) {
    // 把测试程序复制成另一个名字当作“检测软件”，让它占满几个核
    wchar_t self[32768];
    GetModuleFileNameW(nullptr, self, 32768);
    const stdfs::path burner = stdfs::path(self).parent_path() / "iis_fake_vision.exe";
    std::error_code ec;
    stdfs::copy_file(self, burner, stdfs::copy_options::overwrite_existing, ec);
    const std::string root = fresh_dir("pause");
    Config c = quick_config(root);
    const DWORD ncpu = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    const int burn_threads = 4;
    const double expect = 100.0 * burn_threads / ncpu;  // 占整机百分比
    c.pause.processes = {"IIS_FAKE_VISION.EXE"};       // 进程名不区分大小写
    c.pause.pause_percent = expect * 0.6;
    c.pause.resume_percent = expect * 0.3;
    c.pause.resume_after_s = 1;
    c.pause.sample_s = 0.3;
    Running r(c);
    std::wstring cmd = L"\"" + burner.wstring() + L"\" --burn " + std::to_wstring(burn_threads) + L" 6";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    CHECK(CreateProcessW(burner.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi));
    CHECK(wait_for([&] { return r.snap().paused; }, 5));
    std::printf("  检测进程 CPU %.1f%%（整机 %lu 核）\n", r.snap().inspect_cpu, ncpu);
    const Bytes d = make_bmp(200, 100, 50);
    write_file(root + "\\during.bmp", d);
    std::this_thread::sleep_for(std::chrono::seconds(2));
    CHECK(r.snap().paused);
    CHECK(exists(root + "\\during.bmp") && !exists(root + "\\during.bmp.iis"));  // 暂停中不压缩
    WaitForSingleObject(pi.hProcess, 20000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CHECK(wait_for([&] { return !r.snap().paused; }, 10));
    CHECK(wait_for([&] { return compressed_ok(root + "\\during.bmp", d); }, 20));
}

TEST(engine_low_space_guard) {
    const std::string root = fresh_dir("space");
    Config c = quick_config(root);
    c.min_free_mb = 1e9;  // 要求 1 PB 剩余：永远不满足
    Running r(c);
    const Bytes d = make_bmp(100, 100, 60);
    write_file(root + "\\x.bmp", d);
    CHECK(wait_for([&] { return r.snap().low_space; }, 10));
    std::this_thread::sleep_for(std::chrono::seconds(2));
    CHECK(read_file(root + "\\x.bmp") == d && !exists(root + "\\x.bmp.iis"));
}

TEST(engine_throughput_limit) {
    const std::string root = fresh_dir("rate");
    Config c = quick_config(root);
    c.max_mb_s = 1;
    c.delay_s = 0;
    c.stable_s = 0;
    std::vector<Bytes> data;
    for (int i = 0; i < 5; ++i) {
        data.push_back(make_bmp(800, 600, uint32_t(70 + i)));  // 约 0.48 MB
        write_file(root + "\\r" + std::to_string(i) + ".bmp", data.back());
    }
    const auto t0 = std::chrono::steady_clock::now();
    Running r(c);
    CHECK(wait_for([&] { return r.snap().compressed == 5; }, 30));
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  2.4 MB 在 1 MB/s 上限下用时 %.1f 秒\n", s);
    CHECK(s >= 1.8);
}

TEST(engine_keep_mode_no_rework) {
    const std::string root = fresh_dir("keep");
    Config c = quick_config(root);
    c.release = false;
    c.rescan_s = 1;  // 定期扫描也不能重复处理
    const Bytes d = make_bmp(160, 120, 80);
    write_file(root + "\\k.bmp", d);
    {
        Running r(c);
        CHECK(wait_for([&] { return r.snap().compressed == 1; }, 20));
        std::this_thread::sleep_for(std::chrono::seconds(3));
        CHECK_EQ(r.snap().compressed + r.snap().already, uint64_t(1));
    }
    CHECK(read_file(root + "\\k.bmp") == d && restore_equals(root + "\\k.bmp.iis", d));
    Running r2(c);  // 重启后也不重复
    std::this_thread::sleep_for(std::chrono::seconds(3));
    CHECK_EQ(r2.snap().compressed + r2.snap().already, uint64_t(0));
}

TEST(engine_future_mtime) {
    // 系统时间回拨：文件修改时间在“未来”一天。最多等 delay_s，不会卡一天
    const std::string root = fresh_dir("clock");
    Config c = quick_config(root);
    c.delay_s = 1;
    const std::string f = root + "\\future.bmp";
    const Bytes d = make_bmp(100, 100, 90);
    write_file(f, d);
    HANDLE h = CreateFileW(P(f).wstring().c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u{{ft.dwLowDateTime, ft.dwHighDateTime}};
    u.QuadPart += 864000000000ull;
    ft = {u.LowPart, u.HighPart};
    CHECK(SetFileTime(h, nullptr, nullptr, &ft));
    CloseHandle(h);
    Running r(c);
    CHECK(wait_for([&] { return compressed_ok(f, d); }, 15));
}

#include "test_storage.inc"
#include "test_ui.inc"
#include "test_license.inc"

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc >= 4 && std::string(argv[1]) == "--burn") {  // 模拟检测软件：n 个线程空转 s 秒
        const int n = std::atoi(argv[2]);
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(std::atoi(argv[3]));
        std::vector<std::thread> ts;
        for (int i = 0; i < n; ++i)
            ts.emplace_back([end] {
                volatile uint64_t x = 0;
                while (std::chrono::steady_clock::now() < end) x = x + 1;
            });
        for (auto& t : ts) t.join();
        return 0;
    }
    const std::string filter = argc > 1 ? argv[1] : "";
    std::printf("%s\n", iis_build_info());
    for (const auto& c : testing::registry()) {
        if (std::string(c.name).find(filter) == std::string::npos) continue;
        std::printf("运行 %s\n", c.name);
        const int before = testing::failures();
        c.fn();
        std::printf("%s %s\n", testing::failures() == before ? "通过" : "失败", c.name);
    }
    std::printf("共 %zu 项，失败断言 %d 个\n", testing::registry().size(), testing::failures());
    return testing::failures() ? 1 : 0;
}
