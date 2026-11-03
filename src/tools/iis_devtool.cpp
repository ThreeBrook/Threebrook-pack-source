// iis-devtool：开发与对照测试用（不随产品发布）。直接调用内核内部函数，供 Python 对照脚本驱动。
//
//   iis-devtool codec <名称> <输入文件> <输出文件> [参数]
//     名称：lzw-libtiff lzw-imcd packbits packbits-rows(参数=行字节数) zlib(参数=级别) libdeflate(参数=级别)
//           lzw-decode zlib-decode packbits-decode
//   iis-devtool rawguess <文件> [宽 高 像素格式 行跨距]
//     无文件头 RAW 自动识别的结果；给了真值时再比较三种压缩后的大小：按真值参数、按自动识别、只用 zstd。
//     输出一行 JSON（tools/rawguess_eval.py 汇总）
#include "iis/iis.h"
#include "rawguess.hpp"
#include "tiffcodec.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

#include <nlohmann/json.hpp>

using namespace iis::tiffcodec;

namespace {

Bytes read_file(const char* p) {
    std::ifstream f(p, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(f), {});
}

bool write_file(const char* p, const Bytes& b) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
    return bool(f);
}

int codec(int argc, char** argv) {
    if (argc < 5) return 2;
    const std::string name = argv[2];
    const Bytes in = read_file(argv[3]);
    const long arg = argc > 5 ? std::atol(argv[5]) : 0;
    Bytes out;
    bool ok = true;
    if (name == "lzw-libtiff") out = lzw_encode_libtiff(in.data(), in.size());
    else if (name == "lzw-imcd") out = lzw_encode_imcd(in.data(), in.size());
    else if (name == "packbits") out = packbits_encode(in.data(), in.size());
    else if (name == "packbits-rows") out = packbits_encode_rows(in.data(), in.size(), size_t(arg));
    else if (name == "zlib") out = zlib_encode(in.data(), in.size(), int(arg));
    else if (name == "libdeflate") out = libdeflate_encode(in.data(), in.size(), int(arg));
    else if (name == "lzw-decode") ok = lzw_decode(in.data(), in.size(), size_t(1) << 31, out);
    else if (name == "zlib-decode") ok = zlib_decode(in.data(), in.size(), size_t(1) << 31, out);
    else if (name == "packbits-decode") ok = packbits_decode(in.data(), in.size(), size_t(1) << 31, out);
    else return 2;
    if (!ok) return 3;
    return write_file(argv[4], out) ? 0 : 4;
}

// 压缩后的大小（自检通过的容器；压不小时为原大小）
uint64_t compressed(const Bytes& d, const iis_compress_options& o, std::string& detail) {
    iis_buffer c{};
    iis_compress_result r{};
    if (iis_compress(d.data(), d.size(), &o, &c, &r) != IIS_OK) return 0;
    iis_buffer_free(&c);
    detail = r.detail;
    return r.stored_size;
}

int rawguess(int argc, char** argv) {
    if (argc < 3) return 2;
    const Bytes d = read_file(argv[2]);
    nlohmann::json j;
    const auto t0 = std::chrono::steady_clock::now();
    const auto g = iis::rawguess::guess(d.data(), d.size());
    j["ms"] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    j["size"] = d.size();
    if (g) j["guess"] = {{"width", g->width}, {"height", g->height}, {"pixel_format", g->pixel_format}, {"stride", g->stride}};
    if (argc >= 7) {
        std::string rules = "{\"raw\":[{\"match\":\"*\",\"width\":" + std::string(argv[3]) + ",\"height\":" + argv[4] +
                            ",\"pixel_format\":\"" + argv[5] + "\"";
        if (std::atol(argv[6])) rules += ",\"stride\":" + std::string(argv[6]);
        rules += "}]}";
        iis_raw_rules* rr = nullptr;
        if (iis_raw_rules_parse(rules.data(), rules.size(), &rr) != IIS_OK) return 3;
        std::string dt, da, dz;
        iis_compress_options truth{}, autod{}, zstd{};
        truth.raw_rules = rr;
        truth.rel_path_utf8 = "x.raw";
        autod.raw_auto = 1;
        j["truth"] = compressed(d, truth, dt);
        j["auto"] = compressed(d, autod, da);
        j["zstd"] = compressed(d, zstd, dz);  // 不识别：整文件 zstd
        j["auto_detail"] = da;
        iis_raw_rules_free(rr);
    }
    std::printf("%s\n", j.dump().c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "codec") return codec(argc, argv);
    if (argc >= 2 && std::string(argv[1]) == "rawguess") return rawguess(argc, argv);
    std::fprintf(stderr, "用法：iis-devtool codec <名称> <输入> <输出> [参数] | rawguess <文件> [宽 高 像素格式 行跨距]\n");
    return 2;
}
