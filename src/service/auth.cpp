#include "auth.hpp"

#include "iis/iis.h"

#include <cstring>
#include <random>
#include <vector>

namespace iis::svc {

namespace {

std::string hex(const uint8_t* d, size_t n) {
    static const char* x = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) s += x[d[i] >> 4], s += x[d[i] & 15];
    return s;
}

std::string unhex(const std::string& h) {
    std::string o;
    for (size_t i = 0; i + 1 < h.size(); i += 2) o += char(std::stoi(h.substr(i, 2), nullptr, 16));
    return o;
}

}  // namespace

void hmac_sha256(const void* key, size_t key_len, const void* msg, size_t msg_len, uint8_t out[32]) {
    uint8_t k[64] = {};
    if (key_len > 64) iis_sha256(key, key_len, k);
    else std::memcpy(k, key, key_len);
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) ipad[i] = k[i] ^ 0x36, opad[i] = k[i] ^ 0x5c;
    iis_sha256_ctx c;
    uint8_t inner[32];
    iis_sha256_init(&c);
    iis_sha256_update(&c, ipad, 64);
    iis_sha256_update(&c, msg, msg_len);
    iis_sha256_final(&c, inner);
    iis_sha256_init(&c);
    iis_sha256_update(&c, opad, 64);
    iis_sha256_update(&c, inner, 32);
    iis_sha256_final(&c, out);
}

std::string pbkdf2_sha256_hex(const std::string& password, const std::string& salt, int iterations, size_t len) {
    std::vector<uint8_t> dk;
    for (uint32_t block = 1; dk.size() < len; ++block) {
        std::string s = salt;
        s += char(block >> 24), s += char(block >> 16), s += char(block >> 8), s += char(block);
        uint8_t u[32], t[32];
        hmac_sha256(password.data(), password.size(), s.data(), s.size(), u);
        std::memcpy(t, u, 32);
        for (int i = 1; i < iterations; ++i) {
            hmac_sha256(password.data(), password.size(), u, 32, u);
            for (int j = 0; j < 32; ++j) t[j] ^= u[j];
        }
        dk.insert(dk.end(), t, t + 32);
    }
    dk.resize(len);
    return hex(dk.data(), dk.size());
}

PasswordHash make_password(const std::string& password, int iterations) {
    std::random_device rd;
    uint8_t salt[16];
    for (auto& b : salt) b = uint8_t(rd());
    PasswordHash h;
    h.salt = hex(salt, 16);
    h.iterations = iterations;
    h.hash = pbkdf2_sha256_hex(password, std::string(reinterpret_cast<char*>(salt), 16), iterations, 32);
    return h;
}

bool check_password(const PasswordHash& h, const std::string& password) {
    if (h.empty() || h.iterations <= 0) return false;
    const std::string got = pbkdf2_sha256_hex(password, unhex(h.salt), h.iterations, 32);
    if (got.size() != h.hash.size()) return false;
    unsigned diff = 0;
    for (size_t i = 0; i < got.size(); ++i) diff |= unsigned(got[i] ^ h.hash[i]);
    return diff == 0;
}

}  // namespace iis::svc
