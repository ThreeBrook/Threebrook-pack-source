// 存储管理（T6）：保留策略、防爆盘、导出。
//
// 删除规则：
//   - 防爆盘只删“可删除”的图（OK；未知且配置 unknown_as=ok 时也算），按原文件修改时间从旧到新，删到剩余空间回到目标值；
//     **NG 永远不会被防爆盘删除**；没有可删的图时只告警（不会去删 NG）。
//   - 保留策略按规则删除过期文件；NG 只能由 verdict 明确写 "NG" 的规则删除（"*" 不含 NG）。
//   - 正在压缩的文件跳过；删除的是磁盘上的 .iis 与（或）原文件；每删一个记一条审计。
#pragma once

#include "audit.hpp"
#include "classify.hpp"
#include "config.hpp"
#include "index.hpp"
#include "log.hpp"

#include <atomic>
#include <functional>
#include <mutex>

namespace iis::svc {

struct RootSpace {
    std::string root;
    uint64_t capacity = 0, free = 0;          // 容量（盘或配额）与剩余
    uint64_t warn = 0, del = 0, target = 0;   // 生效的阈值（字节）
    int level = 0;                            // 0 正常 / 1 告警 / 2 需删除
    bool stuck = false;                       // 需删除但没有可删除的图
    bool online = false;
};

class Storage {
public:
    // claim：调度线程没有在压缩该文件时“占住”它并返回 true（之后调度线程不会提交它），删完调用 unclaim
    using Claim = std::function<bool(const std::string&)>;
    using Unclaim = std::function<void(const std::string&)>;
    Storage(const Config& cfg, Log& log, Index& index, Audit& audit, const Classifier& cls, Claim claim, Unclaim unclaim);

    void tick();  // 由巡检线程定期调用：测量空间、防爆盘、（到期时）保留策略
    std::vector<RootSpace> spaces();
    bool accelerate() const { return accelerate_; }
    uint64_t deleted() const { return deleted_; }
    uint64_t deleted_ng() const { return deleted_ng_; }

private:
    RootSpace measure(size_t i);
    bool remove(const Row& r, const std::string& reason, std::vector<Audit::Entry>& audit);
    void flush(std::vector<Audit::Entry>& audit);

    const Config& cfg_;
    Log& log_;
    Index& index_;
    Audit& audit_;
    const Classifier& cls_;
    Claim claim_;
    Unclaim unclaim_;
    std::mutex mu_;
    std::vector<RootSpace> spaces_;
    std::atomic<bool> accelerate_{false};
    std::atomic<uint64_t> deleted_{0}, deleted_ng_{0};
    int64_t next_retention_ms_ = 0;
};

struct ExportResult {
    uint64_t files = 0, bytes = 0, failed = 0;
};

// 按条件导出到 out_dir/<根目录名>/<相对路径>：restore 为真时还原成原始格式（SHA-256 校验后才写出），否则复制 .iis。
// 目标已存在不覆盖（计入失败）。每个文件与汇总都记审计
ExportResult export_files(Index& index, Audit& audit, const Filter& f, const std::string& out_dir, bool restore,
                          const std::function<void(const std::string&)>& on_error,
                          const std::function<void(const ExportResult&)>& progress = nullptr);

}  // namespace iis::svc
