// 审计日志（T6）：只追加的 JSON 行文件，按月分文件 audit-YYYYMM.log，哈希链防篡改。
//
// 每行：{"act":动作,"d":详情,"path":路径,"prev":上一条的 hash,"seq":序号,"t":本地时间,"hash":本条 hash}
// hash = SHA-256（去掉 hash 字段后的整行，即 "…,"t":"…"}"），prev 串起前一条（跨月份文件连续）。
// 改动、删除、插入、调换任何一条都会让校验失败并指出位置。
// 多进程安全（服务与命令行同时写）：用 audit.lock 文件锁串行化，每次追加前重读最后一条。
// 追加中途断电留下的半行，下次追加时截掉（那条本来就没写完），并记一条 audit_repair。
#pragma once

#include <nlohmann/json.hpp>

#include <mutex>
#include <string>
#include <vector>

namespace iis::svc {

class Audit {
public:
    struct Entry {
        std::string act, path;
        nlohmann::json detail = nlohmann::json::object();
    };
    struct Verify {
        bool ok = true;
        uint64_t entries = 0, files = 0, bad_seq = 0;
        std::string error, last_hash;
    };

    ~Audit();
    bool open(const std::string& dir, std::string& err);
    bool append(const std::vector<Entry>& entries);
    bool append(const std::string& act, const std::string& path, nlohmann::json detail = nlohmann::json::object());
    uint64_t last_seq() const { return seq_; }
    std::string last_hash() const;
    static Verify verify(const std::string& dir);

private:
    std::string dir_;
    void* lock_ = nullptr;  // HANDLE
    mutable std::mutex mu_;
    uint64_t seq_ = 0;
    std::string hash_;
};

}  // namespace iis::svc
