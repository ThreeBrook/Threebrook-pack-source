// 本地索引（SQLite，T6）：每个受管文件一行，按原文件路径为键（压缩后磁盘上是 路径 + ".iis"）。
// 索引只是加速与统计：真实状态以磁盘为准，丢了可由全量扫描重建（压缩文件的大小与 SHA-256 取自容器头）。
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace iis::svc {

enum FileState { ST_PENDING = 0, ST_COMPRESSED = 1, ST_STORED = 2, ST_DELETED = 3 };
const char* state_name(int s);

struct Row {
    std::string path, root, rel, station, product, batch, verdict = "?";
    int64_t mtime_ms = 0;  // 原文件修改时间（Unix 毫秒）
    uint64_t orig_size = 0, disk_size = 0;  // 原大小 / 现在占用的磁盘字节（.iis，或原文件，或两者之和）
    std::string sha256;                     // 原文件 SHA-256（十六进制），未知为空
    int state = ST_PENDING;
    int64_t updated_ms = 0;
};

struct Totals {
    uint64_t files = 0, used = 0, equiv = 0;  // 未删除文件数 / 占用字节 / 原始字节（等效容量）
};

struct Daily {
    std::string day;
    uint64_t files = 0, bytes_in = 0, bytes_out = 0, deleted = 0, deleted_bytes = 0;
};

struct Filter {
    int64_t from_ms = INT64_MIN, to_ms = INT64_MAX;  // 修改时间 [from, to)
    std::string root, station, product, batch, verdict;  // 空 = 不限；工位 / 产品 / 批次为通配符
};

class Index {
public:
    ~Index();
    bool open(const std::string& file, std::string& err);
    bool get(const std::string& path, Row& out);
    void upsert(Row r);
    // 按修改时间从旧到新遍历未删除的行；fn 返回 false 停止。用于保留策略与防爆盘
    void scan_oldest(const std::string& root, int64_t before_ms, const std::function<bool(const Row&)>& fn);
    void query(const Filter& f, const std::function<bool(const Row&)>& fn);
    Totals totals(const std::string& root);
    std::map<std::string, Totals> all_totals();
    std::map<std::string, uint64_t> verdict_counts();  // 未删除文件按 OK / NG / ? 计数
    void add_daily(const Daily& d);  // d.day 为空时取今天；各字段累加
    std::vector<Daily> daily(int days);
    std::string meta(const std::string& key);
    void set_meta(const std::string& key, const std::string& value);

    // 批量写入放在一个事务里（同一线程可嵌套调用其他方法）
    class Tx {
    public:
        explicit Tx(Index& ix);
        ~Tx();

    private:
        Index& ix_;
    };

private:
    void exec(const char* sql);
    void contribute(const Row& r, int sign);
    sqlite3* db_ = nullptr;
    std::recursive_mutex mu_;
    std::map<std::string, Totals> totals_;
    int tx_depth_ = 0;
};

int64_t now_ms();
std::string local_day(int64_t ms);  // YYYY-MM-DD（本地时间）

}  // namespace iis::svc
