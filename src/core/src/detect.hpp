// 格式识别（阶段 0 bench/detect 的移植）：按内容判定容器、位深、压缩、移位、Bayer，并缓存解码结果供版面拆分复用。
//
// Bayer 判定用到浮点统计——它只决定“是否按四相位拆平面”，不在可逆路径上（拆错只影响压缩率，
// 还原仍由 SHA-256 自检保证）；运算顺序与 Python 参考实现一致，保证两边判定相同。
#pragma once

#include "formats.hpp"

#include <optional>
#include <string>
#include <vector>

namespace iis::detect {

struct ValueStats {
    uint32_t value_max = 0;
    int shift = 0, bits_effective = 0;
};

// arrays：若干段样本；bits_stored > 8 时才判断高位对齐移位
ValueStats value_stats(const std::vector<std::pair<const uint16_t*, size_t>>& arrays, int bits_stored);

struct Bayer {
    bool is_bayer = false;
    double confidence = 0, score = 0, agree = 0;
    int tiles_valid = 0;
    std::string g_diagonal, guess;
    std::vector<std::string> candidates;
};

// img：h×w 单通道、自上而下，已右移去掉对齐位
Bayer bayer_detect(const uint16_t* img, uint64_t h, uint64_t w, uint32_t maxval);

// TIFF 一页的像素块几何与解码结果
struct TiffSeg {
    uint64_t rows, cols, y, x;
    int sample;  // 分平面存储时的分量号，否则 -1
};

struct TiffPageData {
    uint64_t canvas_h = 0, canvas_w = 0;  // 画布含图块边缘填充区
    int bps = 1, spp = 1;                  // 每样本字节、每个像素块内的分量数
    std::vector<TiffSeg> segs;
    fmt::Samples canvas;                   // canvas_h × canvas_w × samples
    std::string split_error;               // 能识别但不能安全拆分的原因（拆分时兜底）
    std::vector<std::vector<uint8_t>> raw;   // 已压缩页：各块解压后（差分前）的字节，供复现式还原比对
    std::vector<std::vector<uint8_t>> tails; // 未压缩页：块中超出几何推算的多余字节
};

struct Detection {
    std::string cls;        // bmp / tiff_uncompressed / tiff_compressed / raw / bayer / unsupported / unknown
    std::string container;  // bmp / tiff / raw / jpeg / png … / 空
    bool processable = false, damaged = false;
    uint64_t width = 0, height = 0, trailer_bytes = 0, row_padding = 0;
    int channels = 0, bits_stored = 0, bits_effective = 0, shift = 0, predictor = 0;
    uint32_t value_max = 0, pages = 1;
    std::string byteorder, compression, palette, row_order, pixel_format, bayer_config;
    std::vector<std::string> bayer_candidates;
    std::string bayer_guess;
    double bayer_confidence = 0;
    std::string error;

    // 缓存：拆分时直接复用，不重复解析与解码
    fmt::BmpInfo bmp;
    fmt::TiffInfo tiff;
    std::vector<TiffPageData> tiff_pages;
    std::optional<fmt::RawParams> raw;
    fmt::Samples raw_img;
};

// RAW 参数来源优先级与参考实现相同：规则 → 同名参数文件（sidecar_json，可为空）。
// guess_raw 为真时，两者都没有的无文件头数据再按内容自动识别（rawguess.hpp；参考实现没有这一步）
Detection detect(const uint8_t* d, size_t n, const std::string& rel_path, const fmt::RawRules* rules,
                 const std::string& sidecar_json = {}, bool guess_raw = false);

// 识别结果（不含缓存）转 JSON，字段名与阶段 0 记录一致
std::string to_json(const Detection& det);

// TIFF 页几何（与 bench/layout/tiff.py _geometry 相同），重组时同样调用
bool tiff_geometry(uint64_t width, uint64_t height, int samples, int bits, int planar, bool tiled, uint64_t tile_h,
                   uint64_t tile_w, uint64_t rowsperstrip, TiffPageData& out);

}  // namespace iis::detect
