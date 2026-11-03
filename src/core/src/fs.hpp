// 文件提交与恢复用到的文件系统操作（Windows / POSIX）。路径一律 UTF-8；Windows 下支持长路径。
#pragma once

#include "internal.hpp"

#include <string>
#include <vector>

namespace iis::fs {

#ifdef _WIN32
constexpr char kSep = '\\';
#else
constexpr char kSep = '/';
#endif

enum class Kind { Missing, File, Dir, Other };
// 不跟随符号链接；无法判断（权限等）时返回 Other
Kind kind(const std::string& path);

// 读整个文件。共享读、写、删除，不妨碍其他程序（并发修改由释放前的 SHA-256 复核发现）
int read_file(const std::string& path, std::vector<uint8_t>& out, uint64_t max_size = UINT64_MAX);

// 改名。replace 为 false 时目标已存在返回 IIS_E_CONFLICT（绝不覆盖）。
// 改名本身写穿到盘（Windows MOVEFILE_WRITE_THROUGH；POSIX 再同步所在目录）。同一目录内调用
int rename_file(const std::string& from, const std::string& to, bool replace);

int remove_file(const std::string& path);

struct DirEntry {
    std::string name;
    bool dir = false;
    bool link = false;  // 符号链接 / 重解析点：恢复扫描不进入、不处理
};
int list_dir(const std::string& dir, std::vector<DirEntry>& out);

std::string join(const std::string& dir, const std::string& name);
std::string parent(const std::string& path);  // 没有目录部分时返回 "."
std::string base_name(const std::string& path);

// 释放原文件：独占打开（拒绝他人写入和删除），长度与 SHA-256 都与 expect 一致才删除。
enum class Release { Released, Busy, Changed, Gone, Failed };
Release release_if_unchanged(const std::string& path, uint64_t size, const uint8_t sha[32], std::string& why);

}  // namespace iis::fs
