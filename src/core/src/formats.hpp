// 容器解析：BMP 头、TIFF 目录（IFD）、RAW 参数与打包格式。只读，不改动任何字节。
// 判定规则与阶段 0 参考实现（bench/formats）一致。
#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace iis::fmt {

using Samples = std::vector<uint16_t>;  // 工作数组统一用 16 位样本（8 位图同样存放），按行、按像素交织

// ---------------- BMP ----------------

struct BmpInfo {
    uint64_t file_len = 0;
    uint32_t pixel_offset = 0, dib = 0, compression = 0, clr_used = 0, pal_off = 0;
    uint32_t width = 0, height = 0, bits = 0, channels = 0;
    bool top_down = false;
    uint64_t stride = 0;
    std::vector<uint8_t> palette;  // 4 字节一项（B G R 0）

    uint64_t row_bytes() const { return uint64_t(width) * channels; }
    uint64_t pixel_len() const { return stride * height; }
    uint64_t pixel_end() const { return pixel_offset + pixel_len(); }
    bool damaged() const { return pixel_end() > file_len; }
    bool gray_palette() const;
};

enum class Parse { Ok, NotThis, Unsupported, Bad };

bool is_bmp(const uint8_t* d, size_t n);
Parse parse_bmp(const uint8_t* d, size_t n, BmpInfo& out, std::string& why);

// ---------------- TIFF ----------------

struct TiffPage {
    uint32_t index = 0;
    uint64_t width = 0, height = 0;
    int bits = 0;  // 各分量位深不同时为 -1
    int samples = 1, sampleformat = 1, compression = 1, predictor = 1, planar = 1, photometric = -1, orientation = 1;
    bool tiled = false;
    uint64_t tile_h = 0, tile_w = 0;
    uint64_t rowsperstrip = 0;  // 0 表示没有该标签
    std::vector<uint64_t> offsets, bytecounts;

    const char* compression_name() const;  // nullptr 表示未压缩；不认识的返回 "other"
};

struct TiffInfo {
    bool little = true, bigtiff = false;
    std::vector<TiffPage> pages;
};

bool is_tiff(const uint8_t* d, size_t n);
bool parse_tiff(const uint8_t* d, size_t n, TiffInfo& out, std::string& why);

// ---------------- RAW ----------------

struct PixelFormat {
    const char* name;
    int bpp;              // 每像素字节；0 表示打包格式
    int bits;             // 有效位数
    const char* bayer;    // nullptr 或 RGGB / GBRG / GRBG / BGGR
    const char* packed;   // nullptr 或 Mono10p / Mono12p / Mono12Packed
};

const PixelFormat* find_pixel_format(const std::string& name);

struct RawParams {
    uint64_t width = 0, height = 0, stride = 0 /* 0 表示紧密排列 */, offset = 0;
    std::string pixel_format;
    std::string origin;  // 命中的规则
    const PixelFormat* pf = nullptr;

    int bits_stored() const { return pf->packed ? pf->bits : pf->bpp * 8; }
    uint64_t row_stride() const { return pf->packed ? 0 : (stride ? stride : width * uint64_t(pf->bpp)); }
    uint64_t data_len() const;
};

// RAW 规则：JSON {"raw": [{"match": "station3/*.raw", "width": …, "height": …, "pixel_format": …,
//                           "stride": 可选, "offset": 可选}, …]}，也接受顶层直接是数组
struct RawRules {
    std::vector<std::pair<std::string, RawParams>> rules;
};

bool parse_raw_rules(const std::string& json_text, RawRules& out, std::string& why);
// 同名参数文件（JSON：width、height、pixel_format，可选 stride、offset）；不合法时返回空
std::optional<RawParams> parse_raw_sidecar(const std::string& json_text);
// 按相对路径（"/" 分隔）匹配第一条规则；通配语义同 Python fnmatch（Windows 下不区分大小写）
std::optional<RawParams> match_raw_rule(const RawRules& rules, const std::string& rel_path);
bool glob_match(const std::string& pattern, const std::string& name);

// GenICam 打包格式
uint64_t packed_size(const char* fmt, uint64_t n_pixels);
bool unpack(const char* fmt, const uint8_t* data, size_t len, uint64_t n_pixels, Samples& out);
std::vector<uint8_t> pack(const char* fmt, const uint16_t* v, uint64_t n_pixels);

}  // namespace iis::fmt
