// 无文件头 RAW 的参数自动识别：按内容猜宽、高、像素格式与行跨距。
//
// 做法：对每种像素格式（Mono8、16 位小端、Mono10p、Mono12p、Mono12Packed）与每个能整除像素数的行宽，
// 抽样若干行，用 JPEG-LS 同款的 MED 预测估计每像素需要的比特数（Bayer 图再试同色相邻的预测），
// 取估计压缩后最小的那一种。行宽对了，上一行就是真正的上一行，预测最准。
//
// 猜错不会造成数据错误：压缩后照常自检、还原逐字节核对，只影响压缩比。所以调用方对猜出来的参数
// 应同时试 zstd，取小的。估计看不出任何二维结构（像随机数据或已压缩的数据）时返回空。
#pragma once

#include "formats.hpp"

#include <optional>

namespace iis::rawguess {

std::optional<fmt::RawParams> guess(const uint8_t* data, size_t size);

}  // namespace iis::rawguess
