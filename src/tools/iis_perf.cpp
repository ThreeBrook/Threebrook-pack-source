// iis-perf：单线程吞吐测量（开发工具，不随产品发布）。
//
// 用法：iis-perf [--repeat N] [--zstd-level L] [--cpu K] 图1.pgm 图2.pgm ...
// 输入为 PGM（P5，maxval ≤ 65535；16 位为大端，读入后转为本机字节序），
// 位深取 maxval 的有效位数。可用 tools/planes_to_pgm.py 从阶段 0 合成样本导出像素平面。
//
// 计时：预热 1 次 + N 次取中位数；MB/s 按 10^6 字节、以原始像素字节数计。
// “管线”= 编码 + 解码 + 两次 SHA-256（原数据与还原数据），近似单文件写入自检的纯计算开销。
#include "iis/iis.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

#ifdef _WIN32
#  define NOMINMAX
#  include <windows.h>
#endif

namespace {

struct Image {
    std::string path;
    iis_image_info info{};
    std::vector<uint8_t> px;
};

bool read_pgm(const std::string& path, Image& img) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string magic;
    uint32_t w = 0, h = 0, maxval = 0;
    f >> magic >> w >> h >> maxval;
    f.get();
    if (magic != "P5" || !w || !h || !maxval || maxval > 65535) return false;
    uint32_t bits = 1;
    while ((1u << bits) - 1 < maxval) ++bits;
    img.path = path;
    img.info = {w, h, 1, std::max(bits, 2u)};
    img.px.resize(iis_image_bytes(&img.info));
    f.read(reinterpret_cast<char*>(img.px.data()), std::streamsize(img.px.size()));
    if (size_t(f.gcount()) != img.px.size()) return false;
    if (maxval > 255)
        for (size_t i = 0; i < img.px.size(); i += 2) std::swap(img.px[i], img.px[i + 1]);
    return true;
}

double median_seconds(int repeat, const std::function<void()>& fn) {
    fn();  // 预热
    std::vector<double> t;
    for (int i = 0; i < repeat; ++i) {
        const auto a = std::chrono::steady_clock::now();
        fn();
        t.push_back(std::chrono::duration<double>(std::chrono::steady_clock::now() - a).count());
    }
    std::sort(t.begin(), t.end());
    return std::max(t[t.size() / 2], 1e-9);
}

void must(int st, const char* what) {
    if (st != IIS_OK) {
        std::fprintf(stderr, "%s 失败：%s %s\n", what, iis_status_str(st), iis_last_error());
        std::exit(2);
    }
}

std::string read_text(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return f ? std::string(std::istreambuf_iterator<char>(f), {}) : std::string();
}

// 整文件模式：iis-perf files <根目录> [--rules 规则.json] [--repeat N] [--cpu K] 相对路径…
// 每个文件：压缩（识别+拆分 / 编码+写容器 / 自检 分段计时）与还原，预热 1 次 + N 次取中位数，输出 CSV。
int files_mode(int argc, char** argv) {
    const std::string root = argv[2];
    int repeat = 3, cpu = -1;
    std::string rules_path;
    std::vector<std::string> rels;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--repeat" && i + 1 < argc) repeat = std::max(1, std::atoi(argv[++i]));
        else if (a == "--cpu" && i + 1 < argc) cpu = std::atoi(argv[++i]);
        else if (a == "--rules" && i + 1 < argc) rules_path = argv[++i];
        else rels.push_back(a);
    }
#ifdef _WIN32
    if (cpu >= 0) SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << cpu);
#endif
    iis_raw_rules* rules = nullptr;
    if (!rules_path.empty()) {
        const std::string t = read_text(rules_path);
        must(iis_raw_rules_parse(t.data(), t.size(), &rules), "读取 RAW 规则");
    }
    std::printf("path,orig,method,stored,analyze_s,encode_s,verify_s,restore_s\n");
    for (const auto& rel : rels) {
        const std::string path = root + "/" + rel;
        const std::string data = read_text(path);
        std::string side;
        const auto dot = path.find_last_of('.');
        if (dot != std::string::npos) side = read_text(path.substr(0, dot) + ".json");
        iis_compress_options opt{19, rules, rel.c_str(), side.empty() ? nullptr : side.c_str()};
        std::vector<double> ta, te, tv, tr;
        iis_compress_result res{};
        iis_buffer c{};
        for (int k = 0; k <= repeat; ++k) {
            iis_buffer_free(&c);
            must(iis_compress(data.data(), data.size(), &opt, &c, &res), "压缩");
            if (k) ta.push_back(res.seconds_analyze), te.push_back(res.seconds_encode), tv.push_back(res.seconds_verify);
        }
        if (res.method != IIS_METHOD_STORE) {
            for (int k = 0; k <= repeat; ++k) {
                const auto a = std::chrono::steady_clock::now();
                iis_reader* r = nullptr;
                iis_buffer out{};
                must(iis_reader_open_memory(c.data, c.size, &r), "打开");
                must(iis_reader_restore(r, &out), "还原");
                iis_buffer_free(&out);
                iis_reader_close(r);
                if (k) tr.push_back(std::chrono::duration<double>(std::chrono::steady_clock::now() - a).count());
            }
        }
        auto med = [](std::vector<double> v) {
            if (v.empty()) return 0.0;
            std::sort(v.begin(), v.end());
            return v[v.size() / 2];
        };
        std::printf("%s,%zu,%d,%llu,%.6f,%.6f,%.6f,%.6f\n", rel.c_str(), data.size(), res.method,
                    (unsigned long long)res.stored_size, med(ta), med(te), med(tv), med(tr));
        iis_buffer_free(&c);
    }
    iis_raw_rules_free(rules);
    return 0;
}

// 线程池模式：iis-perf pool <样本根目录> <工作目录> [--threads 1,2,4] [--rules 规则.json] [相对路径…]
// 样本只读：每轮先把文件复制到工作目录，再用线程池原地压缩并释放原文件（含落盘、读回、改名、复核删除），
// 计时从提交到全部完成；最后逐个只凭 .iis 还原并与样本比对 SHA-256。不给相对路径时取根目录下全部文件。
// 同名参数文件（*.json）最后再提交，避免 RAW 还没处理时参数文件先被压缩释放。
int pool_mode(int argc, char** argv) {
    namespace fs = std::filesystem;
    const fs::path root = argv[2], work = argv[3];  // 命令行参数为本地代码页
    auto slurp = [](const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    };
    auto u8 = [](const fs::path& p) {
        const auto s = p.u8string();
        return std::string(s.begin(), s.end());
    };
    std::vector<int> threads = {1, 2, 4};
    std::string rules_path;
    std::vector<std::string> rels;
    for (int i = 4; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--threads" && i + 1 < argc) {
            threads.clear();
            for (const char* p = argv[++i]; *p;) {
                threads.push_back(std::max(1, std::atoi(p)));
                while (*p && *p != ',') ++p;
                if (*p) ++p;
            }
        } else if (a == "--rules" && i + 1 < argc) {
            rules_path = argv[++i];
        } else {
            rels.push_back(a);
        }
    }
    if (rels.empty())
        for (const auto& e : fs::recursive_directory_iterator(root))
            if (e.is_regular_file()) {
                const auto s = fs::relative(e.path(), root).generic_u8string();
                rels.emplace_back(s.begin(), s.end());
            }
    std::stable_partition(rels.begin(), rels.end(),
                          [](const std::string& r) { return r.size() < 5 || r.compare(r.size() - 5, 5, ".json") != 0; });
    iis_raw_rules* rules = nullptr;
    if (!rules_path.empty()) {
        const std::string t = read_text(rules_path);
        must(iis_raw_rules_parse(t.data(), t.size(), &rules), "读取 RAW 规则");
    }
    uint64_t total = 0;
    for (const auto& r : rels) total += fs::file_size(root / fs::u8path(r));

    struct Stats {
        std::mutex mu;
        uint64_t stored = 0;
        int methods[4] = {}, errors = 0;
    };
    std::printf("threads,files,orig_mb,stored_mb,seconds,mb_per_s,image,zstd,store,errors,verified\n");
    for (const int n : threads) {
        fs::remove_all(work);
        for (const auto& r : rels) {
            const fs::path dst = work / fs::u8path(r);
            fs::create_directories(dst.parent_path());
            fs::copy_file(root / fs::u8path(r), dst);
        }
        Stats st;
        auto cb = [](void* u, const char* path, int status, const iis_file_result* res, const char* err) {
            auto* s = static_cast<Stats*>(u);
            std::lock_guard<std::mutex> lk(s->mu);
            if (status != IIS_OK) {
                ++s->errors;
                std::fprintf(stderr, "%s：%d %s\n", path, status, err);
                return;
            }
            s->stored += res->outcome == IIS_FILE_STORED ? res->compress.orig_size : res->container_size;
            ++s->methods[res->outcome == IIS_FILE_COMPRESSED || res->outcome == IIS_FILE_STORED ? res->compress.method & 3 : 0];
        };
        iis_file_options opt{};
        opt.compress.raw_rules = rules;
        opt.release = IIS_RELEASE_DELETE;
        iis_pool* pool = nullptr;
        must(iis_pool_create(n, &opt, cb, &st, &pool), "创建线程池");
        const auto t0 = std::chrono::steady_clock::now();
        bool sidecars = false;
        for (const auto& r : rels) {
            const bool js = r.size() >= 5 && r.compare(r.size() - 5, 5, ".json") == 0;
            if (js && !sidecars) {
                iis_pool_wait(pool);
                sidecars = true;
            }
            must(iis_pool_submit(pool, u8(work / fs::u8path(r)).c_str(), r.c_str()), "提交");
        }
        iis_pool_wait(pool);
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        iis_pool_destroy(pool);

        int verified = 0;
        for (const auto& r : rels) {
            const std::string src = slurp(root / fs::u8path(r));
            const fs::path wp = work / fs::u8path(r);
            const std::string out = u8(wp);
            if (fs::exists(wp)) {
                verified += slurp(wp) == src && !fs::exists(fs::u8path(out + ".iis"));
                continue;
            }
            iis_reader* rd = nullptr;
            iis_buffer b{};
            if (iis_reader_open_file((out + ".iis").c_str(), &rd) == IIS_OK && iis_reader_restore(rd, &b) == IIS_OK)
                verified += b.size == src.size() && std::memcmp(b.data, src.data(), b.size) == 0;
            iis_buffer_free(&b);
            iis_reader_close(rd);
        }
        std::printf("%d,%zu,%.1f,%.1f,%.2f,%.1f,%d,%d,%d,%d,%d\n", n, rels.size(), total / 1e6, st.stored / 1e6, sec,
                    total / 1e6 / sec, st.methods[IIS_METHOD_IMAGE], st.methods[IIS_METHOD_ZSTD],
                    st.methods[IIS_METHOD_STORE], st.errors, verified);
        std::fflush(stdout);
    }
    fs::remove_all(work);
    iis_raw_rules_free(rules);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    int repeat = 5, zlevel = 3, cpu = -1;
    std::vector<std::string> files;
    if (argc >= 3 && std::string(argv[1]) == "files") return files_mode(argc, argv);
    if (argc >= 4 && std::string(argv[1]) == "pool") return pool_mode(argc, argv);
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--repeat" && i + 1 < argc) repeat = std::max(1, std::atoi(argv[++i]));
        else if (a == "--zstd-level" && i + 1 < argc) zlevel = std::atoi(argv[++i]);
        else if (a == "--cpu" && i + 1 < argc) cpu = std::atoi(argv[++i]);
        else files.push_back(a);
    }
    if (files.empty()) {
        std::fprintf(stderr, "用法：iis-perf [--repeat N] [--zstd-level L] [--cpu K] 图.pgm ...\n");
        return 1;
    }
#ifdef _WIN32
    if (cpu >= 0) SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << cpu);
#endif
    std::printf("%s\n", iis_build_info());
    std::printf("%-28s %5s %4s %6s %9s %9s %9s %9s %9s %9s\n", "文件", "位深", "", "压缩比", "LS编码", "LS解码",
                "管线", "zstd", "SHA-256", "CRC32C");

    double sum_bytes = 0, sum_enc = 0, sum_dec = 0, sum_pipe = 0, sum_z = 0, sum_sha = 0, sum_crc = 0, sum_out = 0;
    for (const auto& path : files) {
        Image img;
        if (!read_pgm(path, img)) {
            std::fprintf(stderr, "无法读取 PGM：%s\n", path.c_str());
            return 1;
        }
        const double mb = double(img.px.size()) / 1e6;
        iis_buffer enc{}, dec{}, z{};
        uint8_t d[32];
        must(iis_jpegls_encode(img.px.data(), img.px.size(), &img.info, &enc), "JPEG-LS 编码");
        must(iis_jpegls_decode(enc.data, enc.size, nullptr, &dec), "JPEG-LS 解码");
        if (dec.size != img.px.size() || std::memcmp(dec.data, img.px.data(), dec.size) != 0) {
            std::fprintf(stderr, "往返不一致：%s\n", path.c_str());
            return 3;
        }
        const size_t enc_size = enc.size;
        iis_buffer_free(&dec);

        const double te = median_seconds(repeat, [&] {
            iis_buffer b{};
            must(iis_jpegls_encode(img.px.data(), img.px.size(), &img.info, &b), "编码");
            iis_buffer_free(&b);
        });
        const double td = median_seconds(repeat, [&] {
            iis_buffer b{};
            must(iis_jpegls_decode(enc.data, enc.size, nullptr, &b), "解码");
            iis_buffer_free(&b);
        });
        const double ts = median_seconds(repeat, [&] { iis_sha256(img.px.data(), img.px.size(), d); });
        const double tz = median_seconds(repeat, [&] {
            must(iis_zstd_compress(img.px.data(), img.px.size(), zlevel, &z), "zstd");
            iis_buffer_free(&z);
        });
        volatile uint32_t sink = 0;
        const double tc = median_seconds(repeat, [&] { sink = sink + iis_crc32c(0, img.px.data(), img.px.size()); });
        const double tp = te + td + 2 * ts;
        iis_buffer_free(&enc);

        std::string name = path.substr(path.find_last_of("/\\") + 1);
        if (name.size() > 28) name = name.substr(name.size() - 28);
        std::printf("%-28s %5u %4s %6.2f %9.1f %9.1f %9.1f %9.1f %9.1f %9.0f\n", name.c_str(), img.info.bits_per_sample,
                    "", double(img.px.size()) / double(enc_size), mb / te, mb / td, mb / tp, mb / tz, mb / ts, mb / tc);
        sum_bytes += mb;
        sum_out += double(enc_size) / 1e6;
        sum_enc += te;
        sum_dec += td;
        sum_pipe += tp;
        sum_z += tz;
        sum_sha += ts;
        sum_crc += tc;
    }
    std::printf("%-28s %5s %4s %6.2f %9.1f %9.1f %9.1f %9.1f %9.1f %9.0f\n", "合计（按总字节/总时间）", "", "",
                sum_bytes / sum_out, sum_bytes / sum_enc, sum_bytes / sum_dec, sum_bytes / sum_pipe, sum_bytes / sum_z,
                sum_bytes / sum_sha, sum_bytes / sum_crc);
    std::printf("单位 MB/s（10^6 字节，按原始像素字节计）；单线程；zstd 级别 %d；重复 %d 次取中位数\n", zlevel, repeat);
    return 0;
}
