// 后台压缩引擎：目录监控 + 写完判定 + 调度 + 资源控制，调用内核线程池做原子提交。
//
// 线程：每个监控根目录一个监控线程（ReadDirectoryChangesW）；一个扫描线程（启动、事件溢出、目录重建、定期补漏时全量扫描）；
// 一个调度线程（独占全部调度状态）；一个巡检线程（检测进程 CPU、剩余空间、定期扫描、统计）；压缩由内核线程池完成。
#pragma once

#include "config.hpp"
#include "log.hpp"

#include <atomic>
#include <memory>
#include <string>

namespace iis::svc {

struct Counters {
    std::atomic<uint64_t> compressed{0}, already{0}, stored{0}, skipped{0}, errors{0}, released{0};
    std::atomic<uint64_t> bytes_in{0}, bytes_out{0};  // 已提交 .iis 的原文件字节 / .iis 字节
};

struct Snapshot {
    uint64_t compressed, already, stored, skipped, errors, released, bytes_in, bytes_out;
    size_t waiting, ready, inflight, remembered;
    bool paused, low_space;
    int roots_online;
    double inspect_cpu;  // 检测进程合计 CPU（整机百分比），未配置为 -1
    bool accelerate;     // 空间告警：加速压缩中
    uint64_t deleted, deleted_ng;  // 存储管理本次运行删除的文件数（其中 NG）
    bool licensed;
};

class Engine {
public:
    Engine(const Config& cfg, Log& log);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // 加载规则、设置资源限制、启动恢复、启动各线程。失败返回 false（原因已写日志）
    bool start();
    // 停止：不再提交新文件，等正在压缩的文件做完
    void stop();
    Snapshot snapshot() const;
    // 状态（与 status.json 相同的 JSON 文本）：计数、空间、OK/NG、每日统计、审计末尾
    std::string status_json();
    // 授权状态：为假时不提交新文件压缩（已有 .iis 的读取、还原、导出与存储管理照常）
    void set_licensed(bool ok);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// 设置进程优先级、核亲和性与 Job Object CPU 上限（服务启动时调用一次）
void apply_resources(const Config& cfg, Log& log);

}  // namespace iis::svc
