// 文件压缩的原子提交与启动恢复（commit.cpp）。
#pragma once

#include "internal.hpp"

#include <string>

namespace iis::commit {

int compress_file(const std::string& path, const iis_file_options& opt, iis_file_result& result);
int recover_dir(const std::string& dir, bool recursive, const iis_file_options& opt, iis_event_callback cb, void* user,
                iis_recover_report& report);

}  // namespace iis::commit
