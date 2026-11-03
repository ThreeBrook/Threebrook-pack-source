// SHA-256（FIPS 180-4）纯整数实现。用于写入自检与容器的原文件哈希。
#include "internal.hpp"

namespace {

constexpr uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

inline uint32_t load_be32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}

void compress_blocks(uint32_t state[8], const uint8_t* p, size_t blocks) {
    uint32_t w[64];
    for (; blocks; --blocks, p += 64) {
        for (int i = 0; i < 16; ++i) w[i] = load_be32(p + 4 * i);
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }
}

}  // namespace

extern "C" {

void iis_sha256_init(iis_sha256_ctx* ctx) {
    static constexpr uint32_t H0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                       0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(ctx->state, H0, sizeof H0);
    ctx->total = 0;
    ctx->used = 0;
}

void iis_sha256_update(iis_sha256_ctx* ctx, const void* data, size_t size) {
    if (!size) return;
    auto p = static_cast<const uint8_t*>(data);
    ctx->total += size;
    if (ctx->used) {
        const size_t take = size < 64 - ctx->used ? size : 64 - ctx->used;
        std::memcpy(ctx->block + ctx->used, p, take);
        ctx->used += uint32_t(take);
        p += take;
        size -= take;
        if (ctx->used < 64) return;
        compress_blocks(ctx->state, ctx->block, 1);
        ctx->used = 0;
    }
    compress_blocks(ctx->state, p, size / 64);
    p += size / 64 * 64;
    size %= 64;
    std::memcpy(ctx->block, p, size);
    ctx->used = uint32_t(size);
}

void iis_sha256_final(iis_sha256_ctx* ctx, uint8_t digest[32]) {
    const uint64_t bits = ctx->total * 8;
    uint8_t pad[72] = {0x80};
    const size_t padlen = (ctx->used < 56 ? 56 : 120) - ctx->used;
    for (int i = 0; i < 8; ++i) pad[padlen + i] = uint8_t(bits >> (56 - 8 * i));
    const uint64_t total = ctx->total;
    iis_sha256_update(ctx, pad, padlen + 8);
    ctx->total = total;
    for (int i = 0; i < 8; ++i) {
        digest[4 * i] = uint8_t(ctx->state[i] >> 24);
        digest[4 * i + 1] = uint8_t(ctx->state[i] >> 16);
        digest[4 * i + 2] = uint8_t(ctx->state[i] >> 8);
        digest[4 * i + 3] = uint8_t(ctx->state[i]);
    }
}

void iis_sha256(const void* data, size_t size, uint8_t digest[32]) {
    iis_sha256_ctx ctx;
    iis_sha256_init(&ctx);
    iis_sha256_update(&ctx, data, size);
    iis_sha256_final(&ctx, digest);
}

}  // extern "C"
