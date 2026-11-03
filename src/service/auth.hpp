// 管理界面的管理员密码（T7 补充）：PBKDF2-HMAC-SHA256 加随机盐，配置里只存盐与哈希。
#pragma once

#include <cstdint>
#include <string>

namespace iis::svc {

struct PasswordHash {
    std::string salt, hash;  // 十六进制
    int iterations = 0;
    bool empty() const { return hash.empty(); }
};

// 生成新的盐与哈希
PasswordHash make_password(const std::string& password, int iterations = 100000);
// 校验（比较不受内容影响耗时）
bool check_password(const PasswordHash& h, const std::string& password);

// 供测试：RFC 2104 HMAC-SHA256 与 RFC 8018 PBKDF2
void hmac_sha256(const void* key, size_t key_len, const void* msg, size_t msg_len, uint8_t out[32]);
std::string pbkdf2_sha256_hex(const std::string& password, const std::string& salt_bytes, int iterations, size_t len);

}  // namespace iis::svc
