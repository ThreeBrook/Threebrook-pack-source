// iis-loadsim：模拟视觉检测设备的负载，用于 T5 验收“对检测的影响”与“写图无失败”（开发工具，不随产品发布）。
//
//   iis-loadsim inspect --threads 8 --cores 2-9 --seconds 60 --period-ms 50 --work 4 [--size 512] [--trace 帧.csv]
//       每个线程按固定节拍（period）处理一帧：对 size×size（默认 512）灰度图做 work 次 3×3 均值滤波 + 直方图，
//       记录每帧处理耗时，输出 P50 / P95 / P99 / 最大值（毫秒，JSON 一行）
//   iis-loadsim write --dir 目录 --rate 10 --seconds 60 [--width 1024 --height 1200] --manifest 清单.txt
//       [--stations 3 --ng-ratio 0.1]：按 cam<k>/<批次>/<OK|NG>/img_N.bmp 分目录写（清单第 3 列为 OK / NG）
//       按速率写 8 位 BMP（每张内容不同），另有一个 latest.bmp 每次覆盖；记录写入失败与耗时；清单记下每个文件的 SHA-256
//   iis-loadsim verify --manifest 清单.txt
//       每个文件：原文件内容一致，或 .iis 还原后一致；输出完好 / 缺失 / 不一致数
#include "iis/iis.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#pragma comment(lib, "shell32.lib")

namespace fs = std::filesystem;
using clk = std::chrono::steady_clock;

namespace {

std::vector<std::wstring> wargs() {
    int n = 0;
    LPWSTR* v = CommandLineToArgvW(GetCommandLineW(), &n);
    std::vector<std::wstring> a(v, v + n);
    LocalFree(v);
    return a;
}

std::string u8(const std::wstring& w) {
    const auto s = fs::path(w).u8string();
    return std::string(s.begin(), s.end());
}

std::wstring opt(const std::vector<std::wstring>& a, const wchar_t* name, const wchar_t* def) {
    for (size_t i = 2; i + 1 < a.size(); ++i)
        if (a[i] == name) return a[i + 1];
    return def;
}

double pct(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(p * double(v.size() - 1) + 0.5))];
}

std::string hex(const uint8_t* d, size_t n) {
    static const char* x = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) s += x[d[i] >> 4], s += x[d[i] & 15];
    return s;
}

int inspect(const std::vector<std::wstring>& a) {
    const int threads = std::stoi(opt(a, L"--threads", L"8"));
    const double seconds = std::stod(opt(a, L"--seconds", L"30"));
    const double period = std::stod(opt(a, L"--period-ms", L"50"));
    const int work = std::stoi(opt(a, L"--work", L"6"));
    const std::wstring cores = opt(a, L"--cores", L"");
    std::vector<int> core_list;
    for (size_t i = 0; i < cores.size();) {  // 如 "2-9" 或 "2,4,6,8"
        size_t j = cores.find(L',', i);
        if (j == std::wstring::npos) j = cores.size();
        const std::wstring part = cores.substr(i, j - i);
        const size_t dash = part.find(L'-');
        const int lo = std::stoi(part.substr(0, dash)), hi = dash == std::wstring::npos ? lo : std::stoi(part.substr(dash + 1));
        for (int c = lo; c <= hi; ++c) core_list.push_back(c);
        i = j + 1;
    }
    const int W = std::stoi(opt(a, L"--size", L"512")), H = W;
    std::vector<std::vector<double>> frame_ms(static_cast<size_t>(threads));
    const fs::path trace = opt(a, L"--trace", L"");
    std::vector<std::vector<double>> starts(static_cast<size_t>(threads));
    const auto begin = clk::now();
    const double begin_epoch =
        std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    const auto end = begin + std::chrono::milliseconds(int64_t(seconds * 1000));
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t)
        ts.emplace_back([&, t] {
            if (!core_list.empty()) SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << core_list[size_t(t) % core_list.size()]);
            std::vector<uint8_t> img(size_t(W) * H), out(img.size());
            std::mt19937 rng{uint32_t(t)};
            for (auto& p : img) p = uint8_t(rng());
            uint64_t hist[256];
            auto next = clk::now() + std::chrono::microseconds(int64_t(period * 1000 * t / threads));  // 各线程错开
            while (clk::now() < end) {
                std::this_thread::sleep_until(next);
                next += std::chrono::microseconds(int64_t(period * 1000));
                const auto t0 = clk::now();
                for (int k = 0; k < work; ++k) {
                    for (int y = 1; y < H - 1; ++y)
                        for (int x = 1; x < W - 1; ++x) {
                            const uint8_t* p = &img[size_t(y) * W + x];
                            out[size_t(y) * W + x] = uint8_t((p[-W - 1] + p[-W] + p[-W + 1] + p[-1] + p[0] + p[1] +
                                                              p[W - 1] + p[W] + p[W + 1]) / 9);
                        }
                    std::fill(std::begin(hist), std::end(hist), 0);
                    for (uint8_t v : out) ++hist[v];
                    img[size_t(k)] = uint8_t(hist[k & 255]);
                }
                frame_ms[size_t(t)].push_back(std::chrono::duration<double, std::milli>(clk::now() - t0).count());
                starts[size_t(t)].push_back(std::chrono::duration<double>(t0 - begin).count());
                if (clk::now() > next) next = clk::now();  // 处理超时：不补帧
            }
        });
    for (auto& t : ts) t.join();
    std::vector<double> all;
    for (auto& v : frame_ms) all.insert(all.end(), v.begin(), v.end());
    if (!trace.empty()) {  // 每帧：开始时刻（Unix 秒）, 耗时（毫秒）
        std::ofstream f(trace, std::ios::binary);
        char line[64];
        for (size_t t = 0; t < frame_ms.size(); ++t)
            for (size_t k = 0; k < frame_ms[t].size(); ++k) {
                std::snprintf(line, sizeof line, "%.4f,%.4f\n", begin_epoch + starts[t][k], frame_ms[t][k]);
                f << line;
            }
    }
    std::printf("{\"frames\": %zu, \"p50\": %.3f, \"p95\": %.3f, \"p99\": %.3f, \"max\": %.3f}\n", all.size(),
                pct(all, 0.5), pct(all, 0.95), pct(all, 0.99), pct(all, 1.0));
    return 0;
}

std::vector<uint8_t> bmp(uint32_t w, uint32_t h) {
    const uint32_t stride = (w + 3) & ~3u, off = 14 + 40 + 1024;
    std::vector<uint8_t> b;
    auto le = [&](uint32_t v, int n) {
        for (int i = 0; i < n; ++i) b.push_back(uint8_t(v >> (8 * i)));
    };
    b.push_back('B');
    b.push_back('M');
    le(off + stride * h, 4), le(0, 4), le(off, 4), le(40, 4), le(w, 4), le(h, 4), le(1, 2), le(8, 2), le(0, 4);
    le(stride * h, 4), le(2835, 4), le(2835, 4), le(256, 4), le(0, 4);
    for (int i = 0; i < 256; ++i) le(uint32_t(i) * 0x010101u, 4);
    std::mt19937 rng(1);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < stride; ++x) b.push_back(x < w ? uint8_t((x / 3 + y / 5) + rng() % 12) : 0);
    return b;
}

// 视觉软件常见写法：CREATE_ALWAYS、只共享读
bool write_one(const std::wstring& path, const std::vector<uint8_t>& d, DWORD& err) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = GetLastError();
        return false;
    }
    bool ok = true;
    for (size_t off = 0; off < d.size() && ok; off += 256 * 1024) {
        DWORD n = 0;
        const DWORD want = DWORD(std::min<size_t>(256 * 1024, d.size() - off));
        ok = WriteFile(h, d.data() + off, want, &n, nullptr) && n == want;
    }
    if (!ok) err = GetLastError();
    CloseHandle(h);
    return ok;
}

int write(const std::vector<std::wstring>& a) {
    const fs::path dir = opt(a, L"--dir", L"");
    const double rate = std::stod(opt(a, L"--rate", L"10"));
    const double seconds = std::stod(opt(a, L"--seconds", L"30"));
    const uint32_t w = uint32_t(std::stoi(opt(a, L"--width", L"1024"))), h = uint32_t(std::stoi(opt(a, L"--height", L"1200")));
    const fs::path manifest = opt(a, L"--manifest", L"manifest.txt");
    const int stations = std::stoi(opt(a, L"--stations", L"0"));
    const double ng_ratio = std::stod(opt(a, L"--ng-ratio", L"0"));
    std::mt19937 pick(12345);
    std::vector<uint8_t> img = bmp(w, h);
    const size_t stamp = img.size() - w;  // 最后一行写入序号，让每张内容不同
    std::ofstream man(manifest, std::ios::binary | std::ios::app);
    std::vector<double> lat;
    int fails = 0, written = 0;
    std::string latest_sha;
    const auto start = clk::now();
    const auto end = start + std::chrono::milliseconds(int64_t(seconds * 1000));
    auto next = start;
    for (int i = 0; clk::now() < end; ++i) {
        std::this_thread::sleep_until(next);
        next += std::chrono::microseconds(int64_t(1e6 / rate));
        for (int k = 0; k < 8; ++k) img[stamp + size_t(k)] = uint8_t(uint32_t(i) >> (8 * (k & 3)));
        std::string verdict;
        fs::path sub = dir / ("batch" + std::to_string(i / 200));
        if (stations > 0) {
            verdict = std::uniform_real_distribution<double>(0, 1)(pick) < ng_ratio ? "NG" : "OK";
            sub = dir / ("cam" + std::to_string(i % stations)) / ("B" + std::to_string(i / 1000)) / verdict;
        }
        fs::create_directories(sub);
        char name[32];
        std::snprintf(name, sizeof name, "img_%06d.bmp", i);
        uint8_t sha[32];
        iis_sha256(img.data(), img.size(), sha);
        const auto t0 = clk::now();
        DWORD err = 0;
        const bool ok = write_one((sub / name).wstring(), img, err);
        lat.push_back(std::chrono::duration<double, std::milli>(clk::now() - t0).count());
        DWORD err2 = 0;
        const bool ok2 = write_one((dir / "latest.bmp").wstring(), img, err2);  // 同名覆盖
        if (!ok) std::fprintf(stderr, "写入失败 %s（错误 %lu）\n", name, err);
        if (!ok2) std::fprintf(stderr, "覆盖 latest.bmp 失败（错误 %lu）\n", err2);
        fails += !ok + !ok2;
        if (ok) {
            ++written;
            man << u8((sub / name).wstring()) << "\t" << hex(sha, 32) << (verdict.empty() ? "" : "\t" + verdict) << "\n";
        }
        if (ok2) latest_sha = hex(sha, 32);
    }
    if (!latest_sha.empty()) man << u8((dir / "latest.bmp").wstring()) << "\t" << latest_sha << "\n";
    std::printf("{\"written\": %d, \"failures\": %d, \"write_p50\": %.3f, \"write_p99\": %.3f, \"write_max\": %.3f}\n",
                written, fails, pct(lat, 0.5), pct(lat, 0.99), pct(lat, 1.0));
    return fails ? 1 : 0;
}

int verify(const std::vector<std::wstring>& a) {
    std::ifstream man(fs::path(opt(a, L"--manifest", L"manifest.txt")), std::ios::binary);
    std::string line;
    int ok = 0, as_iis = 0, missing = 0, bad = 0;
    while (std::getline(man, line)) {
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        const std::string path = line.substr(0, tab), sha = line.substr(tab + 1, 64);
        const fs::path p = fs::u8path(path);
        uint8_t d[32];
        if (fs::exists(p)) {
            std::ifstream f(p, std::ios::binary);
            const std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            iis_sha256(s.data(), s.size(), d);
            if (hex(d, 32) == sha) {
                ++ok;
                continue;
            }
        }
        iis_reader* r = nullptr;
        iis_buffer b{};
        if (iis_reader_open_file((path + ".iis").c_str(), &r) == IIS_OK && iis_reader_restore(r, &b) == IIS_OK) {
            iis_sha256(b.data, b.size, d);
            if (hex(d, 32) == sha) ++ok, ++as_iis;
            else ++bad, std::fprintf(stderr, "不一致：%s\n", path.c_str());
        } else if (fs::exists(p)) {
            ++bad;
            std::fprintf(stderr, "不一致：%s\n", path.c_str());
        } else {
            ++missing;
            std::fprintf(stderr, "缺失：%s\n", path.c_str());
        }
        iis_buffer_free(&b);
        iis_reader_close(r);
    }
    std::printf("{\"ok\": %d, \"as_iis\": %d, \"missing\": %d, \"mismatch\": %d}\n", ok, as_iis, missing, bad);
    return missing || bad ? 1 : 0;
}

}  // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    const auto a = wargs();
    const std::wstring mode = a.size() > 1 ? a[1] : L"";
    if (mode == L"inspect") return inspect(a);
    if (mode == L"write") return write(a);
    if (mode == L"verify") return verify(a);
    std::printf("用法：iis-loadsim inspect|write|verify …（见源码开头说明）\n");
    return 2;
}
