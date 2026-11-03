// 后台服务配置（JSON 文件，UTF-8）。字段与默认值见 src/service/README.md。
#pragma once

#include "auth.hpp"
#include "classify.hpp"

#include <string>
#include <vector>

namespace iis::svc {

struct WatchDir {
    std::string path;
    bool recursive = true;
    std::vector<std::string> include;  // 相对监控根目录、"/" 分隔的通配符；空 = 默认图像扩展名
    std::vector<std::string> exclude;
    double quota_mb = 0;  // 本目录占用上限（受管文件合计），0 = 只看所在盘的剩余空间
};

// 保留策略（T6）：按顺序取第一条匹配的规则；days 为 0 表示不按时间删除
struct RetentionRule {
    std::string station = "*", product = "*";
    std::string verdict = "*";  // OK / NG / ? / *（* 不含 NG：NG 只能由明确写 "NG" 的规则删除）
    double days = 0;
};

// 防爆盘（T6）：阈值取 MB 与百分比（占容量）中较大者；容量为盘总容量，或设了 quota_mb 时为配额
struct DiskGuard {
    double warn_free_mb = 0, warn_free_percent = 15;      // 低于此值：告警 + 加速压缩
    double delete_free_mb = 0, delete_free_percent = 8;   // 低于此值：按时间从旧到新删除可删除的 OK 图
    double target_free_mb = 0, target_free_percent = 10;  // 删除到剩余空间回到此值
    int accelerate_threads = 0;                           // 加速时的压缩线程数，0 = threads × 2
    double accelerate_delay_s = 300;                      // 加速时的写完判定延迟（不超过 delay_s）
    double check_s = 10;
};

struct PauseRule {
    std::vector<std::string> processes;  // 检测进程名（如 Vision.exe），不区分大小写
    double pause_percent = 60;           // 这些进程合计 CPU（占整机百分比）达到此值时暂停
    double resume_percent = 40;          // 低于此值持续 resume_after_s 秒后恢复
    double resume_after_s = 10;
    double sample_s = 1;
};

struct Config {
    std::vector<WatchDir> watch;
    std::string raw_rules_file;  // RAW 参数规则（iis_raw_rules_parse 的 JSON）；空 = 无
    double delay_s = 3600;       // 写完判定：最后修改后至少等这么久
    double stable_s = 5;         // 再确认大小与修改时间在这段时间内不变
    double retry_s = 60;         // 被占用等情况的重试间隔
    double rescan_s = 3600;      // 定期全量扫描（补漏），0 = 只在启动和事件溢出时扫描
    bool release = true;         // 提交后释放原文件
    double min_saving = 0;
    int zstd_level = 19;
    int threads = 2;             // 压缩线程数
    int max_cores = 0;           // 限制使用的逻辑核数（亲和性，取编号最大的若干个核），0 = 不限
    std::vector<int> cores;      // 指定逻辑核编号（优先于 max_cores）
    int cpu_rate_percent = 0;    // Job Object CPU 硬上限（占整机百分比），0 = 不限
    double max_mb_s = 0;         // 吞吐上限（按原文件字节），0 = 不限
    std::string priority = "background";  // background（CPU 与 IO 都降到后台）/ idle / below_normal / normal
    double min_free_mb = 2048;   // 监控目录所在盘剩余空间低于此值时不写入 .iis，避免与视觉软件抢最后的空间
    PauseRule pause;
    ClassifyConfig classify;
    std::vector<RetentionRule> retention;
    double retention_interval_s = 600;
    DiskGuard disk;
    std::string data_dir;        // 索引与审计日志；空 = 配置文件所在目录下的 data
    std::string license_file;    // 授权文件；空 = 配置文件所在目录下的 license.lic
    std::string config_sha256;   // 配置文件内容的 SHA-256（启动时与上次比较，变化记审计）
    std::string change_note;     // 本次变更说明（JSON 文本，由管理界面填写，写入配置变更审计）
    bool ui_enabled = true;      // 本机管理界面（只监听 127.0.0.1）
    int ui_port = 8686;
    PasswordHash ui_password;    // 管理员密码（只存盐与哈希）；未设置时界面要求先设置才能改配置
    std::string log_dir;         // 空 = 配置文件所在目录下的 logs
    int log_keep_days = 30;
    std::string status_file;     // 空 = log_dir/status.json
    double status_s = 60;        // 统计日志与状态文件的更新间隔
    size_t watch_buffer = 64 * 1024;  // 目录事件缓冲区字节数（网络共享上限 64 KB）。不从配置文件读取，仅供测试调小以制造溢出
    int watch_stall_ms = 0;           // 每批事件处理后停顿（毫秒）。不从配置文件读取，仅供测试制造溢出
};

// 解析并检查；未知字段报错（防止拼错的配置被静默忽略）。base_dir 用于补全相对路径
bool parse_config(const std::string& json_text, const std::string& base_dir, Config& out, std::string& err);
bool load_config(const std::string& path, Config& out, std::string& err);

// 默认只处理常见图像扩展名；视觉软件自己的日志、数据库、参数文件不在默认范围内
const std::vector<std::string>& default_include();

// 全部可配置项的默认值（取自 Config{}，与解析同源），JSON 文本：
//   {"config": 完整配置（watch 为空数组）, "watch": 新增监控目录的默认值（不含 path）}
// init-config 用它生成写全默认值的配置；管理界面用它把没写的项显示为默认值
std::string defaults_json();

}  // namespace iis::svc
