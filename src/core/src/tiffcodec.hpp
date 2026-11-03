// TIFF 像素块的解码与复现式重编码（docs/05 第 5 节）。
//
// 复现式还原要求“按记录的编码器重新编码，与原字节逐字节一致”，所以每个编码器都必须与写图程序所用实现完全相同：
//   libtiff-lzw   libtiff tif_lzw.c 的行为（OpenCV 与多数视觉软件），移植自 bench/layout/lzw_libtiff.py
//   imcd-lzw      imagecodecs imcd.c 的 imcd_lzw_encode（tifffile 写的 LZW）
//   zlib          经典 zlib compress2（libtiff / OpenCV 的 ZIP）
//   libdeflate    libdeflate zlib 格式（tifffile / imagecodecs 的 ZIP）
//   packbits      imagecodecs imcd.c 的 imcd_packbits_encode；按行编码即 libtiff / tifffile 的做法
// 全部为确定性整数运算。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace iis::tiffcodec {

using Bytes = std::vector<uint8_t>;

// 解码：失败返回 false。max_out 为输出上限（防止损坏数据引发超大输出），超出也返回 false
bool lzw_decode(const uint8_t* src, size_t n, size_t max_out, Bytes& out);
bool zlib_decode(const uint8_t* src, size_t n, size_t max_out, Bytes& out);
bool packbits_decode(const uint8_t* src, size_t n, size_t max_out, Bytes& out);

Bytes lzw_encode_libtiff(const uint8_t* src, size_t n);
Bytes lzw_encode_imcd(const uint8_t* src, size_t n);
Bytes zlib_encode(const uint8_t* src, size_t n, int level);
Bytes libdeflate_encode(const uint8_t* src, size_t n, int level);
Bytes packbits_encode(const uint8_t* src, size_t n);
Bytes packbits_encode_rows(const uint8_t* src, size_t n, size_t row_bytes);

const char* zlib_lib_version();
const char* libdeflate_version();

}  // namespace iis::tiffcodec
