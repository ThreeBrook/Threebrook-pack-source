// 版面整理：把识别过的文件拆成“原样字节区间 + 像素平面 + 附加字节 + 版面参数”，以及按配方逆向重组。
// 全部为整数、可逆运算；规则与阶段 0 参考实现（bench/layout）一致，配方格式见 spec/container-v1.md 第 13 节。
#pragma once

#include "container.hpp"
#include "detect.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <vector>

namespace iis::layout {

struct Plane {
    std::string role;        // gray / rgb / alpha / index / sample
    uint32_t image = 0;      // BMP / RAW 为 0，TIFF 为页号
    int bayer_dy = -1, bayer_dx = -1;
    int sample = -1;         // role=sample 时的分量号
    uint64_t width = 0, height = 0;
    uint32_t components = 1;
    int bits = 1;            // 有效位数（≥1）
    int shift = 0;
    fmt::Samples px;         // height × width × components
};

struct Extra {
    std::string role;        // row_padding / packed_tail / block_tail
    uint32_t image = 0;
    int64_t block = -1;      // block_tail：像素块序号
    std::vector<uint8_t> bytes;
};

struct Split {
    std::string format;      // bmp / tiff / raw
    std::vector<cont::Range> skeleton;
    nlohmann::json layout;
    std::vector<Plane> planes;
    std::vector<Extra> extras;
};

// 失败返回 false（why 说明原因），上层整文件兜底
bool split(const uint8_t* d, size_t n, const detect::Detection& det, Split& out, std::string& why);

// 重组：fetch(type, id, bytes) 取 PLANE（解码后的像素字节，布局见规格 6.2）或 EXTRA 段内容；
// 把像素区写入 out（长度 size），并把写入的区间追加到 covered。配方不合法或数据不符返回 IIS_E_CORRUPT。
using Fetch = std::function<int(uint16_t type, uint16_t id, std::vector<uint8_t>& bytes)>;
int rebuild(const nlohmann::json& recipe, const Fetch& fetch, uint8_t* out, uint64_t size,
            std::vector<cont::Range>& covered);

}  // namespace iis::layout
