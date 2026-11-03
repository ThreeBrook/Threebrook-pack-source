// JPEG-LS 与 zstd：各种位深 / 分量 / 尺寸往返一致；参数错误、损坏数据有明确状态码且不崩溃。
#include "testing.hpp"

#include "iis/iis.h"

#include <charls/charls.h>

#include <cstring>

namespace {

// 工业图像近似：平滑渐变 + 噪声 + 少量饱和点，样本值严格落在位深内
std::vector<uint8_t> make_image(const iis_image_info& info, uint32_t seed) {
    std::vector<uint8_t> px(iis_image_bytes(&info));
    std::mt19937 rng(seed);
    const uint32_t maxval = (1u << info.bits_per_sample) - 1;
    const size_t n = size_t(info.width) * info.height * info.components;
    for (size_t i = 0; i < n; ++i) {
        const size_t pix = i / info.components, x = pix % info.width, y = pix / info.width;
        uint32_t v = uint32_t((x * 3 + y * 2) * maxval / (3 * info.width + 2 * info.height + 1)) + rng() % 7;
        if (rng() % 997 == 0) v = maxval;
        if (v > maxval) v = maxval;
        if (info.bits_per_sample <= 8)
            px[i] = uint8_t(v);
        else
            std::memcpy(&px[2 * i], &v, 2);  // 本机字节序
    }
    return px;
}

bool roundtrip(const iis_image_info& info, uint32_t seed) {
    const auto px = make_image(info, seed);
    iis_buffer enc{}, dec{};
    if (iis_jpegls_encode(px.data(), px.size(), &info, &enc) != IIS_OK) return false;
    iis_image_info got{};
    CHECK_EQ(iis_jpegls_read_info(enc.data, enc.size, &got), IIS_OK);
    const int st = iis_jpegls_decode(enc.data, enc.size, &got, &dec);
    const bool ok = st == IIS_OK && got.width == info.width && got.height == info.height &&
                    got.components == info.components && got.bits_per_sample == info.bits_per_sample &&
                    dec.size == px.size() && std::memcmp(dec.data, px.data(), px.size()) == 0;
    iis_buffer_free(&enc);
    iis_buffer_free(&dec);
    return ok;
}

}  // namespace

TEST(jpegls_roundtrip_bit_depths) {
    for (uint32_t bits = 2; bits <= 16; ++bits) {
        CHECK(roundtrip({97, 61, 1, bits}, bits));
        CHECK(roundtrip({64, 33, 3, bits}, 100 + bits));
    }
}

TEST(jpegls_roundtrip_shapes) {
    CHECK(roundtrip({1, 1, 1, 8}, 1));
    CHECK(roundtrip({1, 500, 1, 12}, 2));
    CHECK(roundtrip({777, 1, 1, 16}, 3));
    CHECK(roundtrip({2448, 64, 1, 8}, 4));
    CHECK(roundtrip({31, 17, 4, 8}, 6));
}

TEST(jpegls_rejects_bad_input) {
    std::vector<uint8_t> px(100 * 10, 0);
    iis_buffer out{};
    iis_image_info info{100, 10, 1, 8};
    CHECK_EQ(iis_jpegls_encode(px.data(), px.size() - 1, &info, &out), IIS_E_SIZE_MISMATCH);
    info.bits_per_sample = 1;
    CHECK_EQ(iis_jpegls_encode(px.data(), px.size(), &info, &out), IIS_E_INVALID_ARG);
    info = {100, 10, 2, 8};  // 2 分量不支持（与 imagecodecs 一致）
    CHECK_EQ(iis_jpegls_encode(px.data(), px.size(), &info, &out), IIS_E_INVALID_ARG);
    info = {100, 10, 5, 8};
    CHECK_EQ(iis_jpegls_encode(px.data(), px.size(), &info, &out), IIS_E_INVALID_ARG);
    info = {0, 10, 1, 8};
    CHECK_EQ(iis_jpegls_encode(px.data(), px.size(), &info, &out), IIS_E_INVALID_ARG);
    CHECK(std::strlen(iis_last_error()) > 0);
    // 12 位图中混入 4096：必须拒绝，不能静默产出还原不一致的码流
    info = {50, 10, 1, 12};
    std::vector<uint8_t> px16(iis_image_bytes(&info), 0);
    const uint16_t bad = 4096;
    std::memcpy(&px16[2 * 123], &bad, 2);
    CHECK_EQ(iis_jpegls_encode(px16.data(), px16.size(), &info, &out), IIS_E_INVALID_ARG);
    // 7 位图中混入 128
    info = {50, 10, 1, 7};
    std::vector<uint8_t> px7(iis_image_bytes(&info), 1);
    px7[77] = 128;
    CHECK_EQ(iis_jpegls_encode(px7.data(), px7.size(), &info, &out), IIS_E_INVALID_ARG);
    CHECK(out.data == nullptr);
}

TEST(jpegls_rejects_near_lossless_stream) {
    const iis_image_info info{40, 30, 1, 8};
    const auto px = make_image(info, 9);
    charls::jpegls_encoder enc;
    enc.frame_info({40, 30, 8, 1}).near_lossless(2);
    std::vector<uint8_t> dst(enc.estimated_destination_size());
    enc.destination(dst);
    dst.resize(enc.encode(px));
    iis_buffer out{};
    CHECK_EQ(iis_jpegls_decode(dst.data(), dst.size(), nullptr, &out), IIS_E_UNSUPPORTED);
}

TEST(jpegls_planar_stream_is_interleaved_on_decode) {
    const iis_image_info info{23, 19, 3, 8};
    const auto px = make_image(info, 10);
    std::vector<uint8_t> planar(px.size());
    const size_t plane = 23 * 19;
    for (size_t i = 0; i < plane; ++i)
        for (size_t c = 0; c < 3; ++c) planar[c * plane + i] = px[i * 3 + c];
    charls::jpegls_encoder enc;
    enc.frame_info({23, 19, 8, 3}).interleave_mode(charls::interleave_mode::none);
    std::vector<uint8_t> dst(enc.estimated_destination_size());
    enc.destination(dst);
    dst.resize(enc.encode(planar));
    iis_buffer out{};
    CHECK_EQ(iis_jpegls_decode(dst.data(), dst.size(), nullptr, &out), IIS_OK);
    CHECK(out.size == px.size() && std::memcmp(out.data, px.data(), px.size()) == 0);
    iis_buffer_free(&out);
}

TEST(jpegls_corrupt_never_crashes) {
    const iis_image_info info{120, 80, 1, 12};
    const auto px = make_image(info, 11);
    iis_buffer enc{};
    CHECK_EQ(iis_jpegls_encode(px.data(), px.size(), &info, &enc), IIS_OK);
    const std::vector<uint8_t> good(enc.data, enc.data + enc.size);
    iis_buffer_free(&enc);
    std::mt19937 rng(12);
    int errors = 0;
    for (int round = 0; round < 2000; ++round) {
        auto bad = good;
        if (round % 4 == 0) {
            bad.resize(rng() % bad.size());  // 截断
        } else {
            for (int k = 0; k < 1 + int(rng() % 4); ++k) bad[rng() % bad.size()] ^= uint8_t(1 + rng() % 255);
        }
        iis_buffer out{};
        const int st = iis_jpegls_decode(bad.data(), bad.size(), nullptr, &out);
        // 损坏后仍可能“解出”一幅图（JPEG-LS 自身无校验）——这正是容器要有 CRC32C 与 SHA-256 的原因
        if (st != IIS_OK) {
            ++errors;
            CHECK(out.data == nullptr);
        }
        iis_buffer_free(&out);
    }
    CHECK(errors > 0);
}

TEST(zstd_roundtrip) {
    for (size_t n : {size_t(0), size_t(1), size_t(54), size_t(1078), size_t(1) << 20}) {
        auto data = testing::random_bytes(n, uint32_t(n));
        for (size_t i = 0; i < n / 2; ++i) data[i] = uint8_t(i / 64);  // 一半可压缩
        for (int level : {1, 3, 19}) {
            iis_buffer z{}, back{};
            CHECK_EQ(iis_zstd_compress(data.data(), data.size(), level, &z), IIS_OK);
            CHECK_EQ(iis_zstd_decompress(z.data, z.size, n, &back), IIS_OK);
            CHECK(back.size == n && (n == 0 || std::memcmp(back.data, data.data(), n) == 0));
            iis_buffer_free(&back);
            CHECK_EQ(iis_zstd_decompress(z.data, z.size, IIS_SIZE_UNKNOWN, &back), IIS_OK);
            CHECK_EQ(back.size, n);
            iis_buffer_free(&back);
            iis_buffer_free(&z);
        }
    }
}

TEST(zstd_rejects_bad_input) {
    const auto data = testing::random_bytes(5000, 20);
    iis_buffer z{}, out{};
    CHECK_EQ(iis_zstd_compress(data.data(), data.size(), 0, &z), IIS_E_INVALID_ARG);
    CHECK_EQ(iis_zstd_compress(data.data(), data.size(), 3, &z), IIS_OK);
    CHECK_EQ(iis_zstd_decompress(z.data, z.size, 4999, &out), IIS_E_SIZE_MISMATCH);
    std::vector<uint8_t> extra(z.data, z.data + z.size);
    extra.push_back(0);
    CHECK_EQ(iis_zstd_decompress(extra.data(), extra.size(), IIS_SIZE_UNKNOWN, &out), IIS_E_CORRUPT);
    CHECK_EQ(iis_zstd_decompress(z.data, z.size / 2, IIS_SIZE_UNKNOWN, &out), IIS_E_CORRUPT);
    std::mt19937 rng(21);
    for (int round = 0; round < 2000; ++round) {
        std::vector<uint8_t> bad(z.data, z.data + z.size);
        bad[rng() % bad.size()] ^= uint8_t(1 + rng() % 255);
        const int st = iis_zstd_decompress(bad.data(), bad.size(), IIS_SIZE_UNKNOWN, &out);
        CHECK(st == IIS_OK || out.data == nullptr);
        iis_buffer_free(&out);
    }
    iis_buffer_free(&z);
}

TEST(versions) {
    CHECK_EQ(iis_abi_version(), IIS_ABI_VERSION);
    const std::string info = iis_build_info();
    CHECK(info.find("charls 2.4.5") != std::string::npos);
    CHECK(info.find("zstd 1.5.7") != std::string::npos);
    CHECK(std::string(iis_status_str(IIS_E_CORRUPT)) == "数据损坏");
}
