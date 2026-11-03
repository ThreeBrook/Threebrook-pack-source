// SHA-256 与 CRC32C：标准测试向量、分段计算与一次计算一致、硬件与软件实现一致。
#include "testing.hpp"

#include "../src/internal.hpp"

#include <cstring>

namespace {

std::string hex(const uint8_t* d, size_t n) {
    static const char* x = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) s += {x[d[i] >> 4], x[d[i] & 15]};
    return s;
}

std::string sha(const std::string& m) {
    uint8_t d[32];
    iis_sha256(m.data(), m.size(), d);
    return hex(d, 32);
}

}  // namespace

TEST(sha256_vectors) {
    CHECK(sha("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(sha(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    // 长度落在填充边界附近（55/56/63/64/65 字节）
    CHECK(sha(std::string(55, 'x')) != sha(std::string(56, 'x')));
    CHECK(sha(std::string(64, 'a')) == "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
}

TEST(sha256_incremental) {
    const auto data = testing::random_bytes(100003, 1);
    uint8_t whole[32];
    iis_sha256(data.data(), data.size(), whole);
    std::mt19937 rng(2);
    for (int round = 0; round < 50; ++round) {
        iis_sha256_ctx ctx;
        iis_sha256_init(&ctx);
        size_t pos = 0;
        while (pos < data.size()) {
            const size_t n = std::min<size_t>(rng() % 200, data.size() - pos);
            iis_sha256_update(&ctx, data.data() + pos, n);
            pos += n;
        }
        uint8_t d[32];
        iis_sha256_final(&ctx, d);
        CHECK(std::memcmp(d, whole, 32) == 0);
    }
}

TEST(crc32c_vectors) {
    // RFC 3720 附录 B.4 与通用 CRC32C 测试向量
    const char* s = "123456789";
    CHECK_EQ(iis_crc32c(0, s, 9), 0xE3069283u);
    uint8_t buf[32];
    std::memset(buf, 0, 32);
    CHECK_EQ(iis_crc32c(0, buf, 32), 0x8A9136AAu);
    std::memset(buf, 0xFF, 32);
    CHECK_EQ(iis_crc32c(0, buf, 32), 0x62A8AB43u);
    for (int i = 0; i < 32; ++i) buf[i] = uint8_t(i);
    CHECK_EQ(iis_crc32c(0, buf, 32), 0x46DD794Eu);
    for (int i = 0; i < 32; ++i) buf[i] = uint8_t(31 - i);
    CHECK_EQ(iis_crc32c(0, buf, 32), 0x113FDB5Cu);
    CHECK_EQ(iis_crc32c(0, nullptr, 0), 0u);
}

TEST(crc32c_hw_matches_sw_and_chains) {
    const auto data = testing::random_bytes(70001, 3);
    std::mt19937 rng(4);
    for (int round = 0; round < 300; ++round) {
        const size_t off = rng() % 64, n = rng() % (data.size() - off);
        CHECK_EQ(iis_crc32c(0, data.data() + off, n), iis::crc32c_software(0, data.data() + off, n));
        const size_t cut = n ? rng() % n : 0;
        const uint32_t part = iis_crc32c(0, data.data() + off, cut);
        CHECK_EQ(iis_crc32c(part, data.data() + off + cut, n - cut), iis_crc32c(0, data.data() + off, n));
    }
}
