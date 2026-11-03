// 右键“IIS 压缩”“IIS 解压”的任务（与界面无关，可单独测试）。
//
// 压缩：文件夹 → 旁边的“文件夹名.iisz”（一个包）；单个文件 → 旁边的“文件名.iis”。默认保留原文件。
// 解压：.iisz → 解到包所在文件夹（同名文件夹已存在时加 (2)）；.iis → 还原到旁边（已存在不覆盖）；
//       文件夹 → 解开其中（含子目录）所有的 .iis 与 .iisz。
// 每一步都核对 SHA-256：解出的文件与原文件逐字节相同，不一致时报错且不留下文件。
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace iis::view {

struct TaskState {  // 工作线程写、界面线程读，都要持有 mu
    std::mutex mu;
    size_t item = 0, items = 0;  // 正在处理第几项（从 0 起）/ 共几项
    double fraction = 0;         // 当前项的进度 0~1
    std::string current;         // 当前项正在处理的文件
    std::vector<std::string> lines;     // 每项的结果
    std::vector<std::string> problems;  // 失败、跳过、未删除等需要注意的事（最多 500 条）
    uint64_t in_bytes = 0, out_bytes = 0;  // 压缩：压缩前后的总字节
    int failed = 0;                        // 失败的文件或项
    bool cancelled = false;
    std::string open_dir;  // 完成后“打开文件夹”打开的位置
};

void run_compress(const std::vector<std::string>& paths, bool delete_originals, const std::atomic<bool>& cancel,
                  TaskState& st);
void run_extract(const std::vector<std::string>& paths, const std::atomic<bool>& cancel, TaskState& st);

// 压缩任务里每项会生成什么（界面在开始前列出）：“[文件夹] 名 → 名.iisz”等
std::string compress_preview(const std::string& path);

}  // namespace iis::view
