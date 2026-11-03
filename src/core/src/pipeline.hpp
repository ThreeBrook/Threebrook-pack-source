// 单文件内存内压缩管线（pipeline.cpp），供 iis_compress 与文件提交（commit.cpp）共用。
#pragma once

#include "internal.hpp"

#include <vector>

namespace iis::pipeline {

// 选项已检查并补齐默认值；sha 为 data 的 SHA-256。method 为 STORE 时 out 为空
int compress(const uint8_t* data, size_t size, const uint8_t sha[32], const iis_compress_options& opt,
             std::vector<uint8_t>& out, iis_compress_result& result);

// 补齐默认值并检查范围；opt 为 NULL 时用默认值
int normalize(const iis_compress_options* opt, iis_compress_options& out);

void set_detail(char (&dst)[512], const std::string& s);

}  // namespace iis::pipeline
