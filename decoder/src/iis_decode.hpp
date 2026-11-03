// iis-decode：IIS 容器 v1 的独立开源解码器（BSD-3-Clause）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace iis_decode {

// 把一个完整的 .iis 容器还原为原文件。成功时 out 与原文件逐字节相同（已按容器记录的 SHA-256 核对）；
// 失败返回 false，error 为中文原因，out 为空。不会读写任何文件。
bool restore(const uint8_t* data, size_t size, std::vector<uint8_t>& out, std::string& error);

std::string sha256_hex(const uint8_t* data, size_t size);

}  // namespace iis_decode
