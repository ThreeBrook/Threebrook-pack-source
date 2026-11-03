// 无文件头 RAW 自动识别：各像素格式、奇数宽高、行填充、Bayer 识别正确；非图像不猜；
// 用 raw_auto 压缩逐字节还原，且不比只用 zstd 大；不开 raw_auto 时行为不变。
#include "testing.hpp"

#include "iis/iis.h"
#include "rawguess.hpp"

#include <cmath>
#include <cstring>

using namespace iis;

namespace {

using Bytes = std::vector<uint8_t>;

// 平滑的“场景”（几个圆斑 + 渐变）加确定性的噪声，取值 0 ~ 2^bits-1
std::vector<uint16_t> scene(uint32_t w, uint32_t h, int bits, uint32_t seed, int noise, bool bayer = false) {
    std::mt19937 rng(seed);
    const uint32_t maxv = (1u << bits) - 1;
    std::vector<uint16_t> v(size_t(w) * h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            double t = 0.25 + 0.4 * x / w + 0.2 * y / h;
            for (int k = 0; k < 3; ++k) {
                const double cx = w * (0.2 + 0.3 * k), cy = h * (0.3 + 0.2 * k), r = h * (0.12 + 0.05 * k);
                if ((x - cx) * (x - cx) + (y - cy) * (y - cy) < r * r) t += 0.15 * (k % 2 ? -1 : 1);
            }
            if (bayer) t *= (y % 2 == 0 && x % 2 == 0) ? 0.55 : (y % 2 && x % 2) ? 0.8 : 1.0;  // R / B 增益不同
            const int n = noise ? int(rng() % uint32_t(2 * noise + 1)) - noise : 0;
            v[size_t(y) * w + x] = uint16_t(std::clamp(int(t * maxv) + n, 0, int(maxv)));
        }
    return v;
}

Bytes mono8(const std::vector<uint16_t>& v, uint32_t w, uint32_t h, uint32_t stride = 0) {
    if (!stride) stride = w;
    Bytes b(size_t(stride) * h, 0);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) b[size_t(y) * stride + x] = uint8_t(v[size_t(y) * w + x]);
    return b;
}

Bytes mono16(const std::vector<uint16_t>& v, int shift = 0) {
    Bytes b;
    for (const uint16_t s : v) b.push_back(uint8_t(s << shift)), b.push_back(uint8_t((s << shift) >> 8));
    return b;
}

void expect(const Bytes& b, uint64_t w, uint64_t h, const std::string& fmt, uint64_t stride, const char* what) {
    const auto g = rawguess::guess(b.data(), b.size());
    CHECK(g.has_value());
    if (!g) return std::printf("    %s：没有识别出来\n", what), void();
    const bool ok = g->width == w && g->height == h && g->pixel_format == fmt && g->stride == stride && g->origin == "auto";
    if (!ok)
        std::printf("    %s：识别为 %llu×%llu %s 跨距 %llu\n", what, (unsigned long long)g->width, (unsigned long long)g->height,
                    g->pixel_format.c_str(), (unsigned long long)g->stride);
    CHECK(ok);
}

}  // namespace

TEST(rawguess_formats) {
    expect(mono8(scene(1280, 1024, 8, 1, 2), 1280, 1024), 1280, 1024, "Mono8", 0, "Mono8");
    expect(mono8(scene(1279, 1023, 8, 2, 3), 1279, 1023), 1279, 1023, "Mono8", 0, "Mono8 奇数宽高");
    expect(mono8(scene(1277, 1024, 8, 3, 2), 1277, 1024, 1280), 1277, 1024, "Mono8", 1280, "Mono8 行填充");
    expect(mono16(scene(1280, 1024, 12, 4, 20)), 1280, 1024, "Mono12", 0, "16 位容器 12 位（低位对齐）");
    expect(mono16(scene(997, 768, 12, 5, 20), 4), 997, 768, "Mono16", 0, "16 位容器 12 位（高位对齐）");
    expect(mono16(scene(640, 480, 16, 6, 300)), 640, 480, "Mono16", 0, "Mono16");
    for (const char* f : {"Mono10p", "Mono12p", "Mono12Packed"}) {
        const int bits = std::strcmp(f, "Mono10p") == 0 ? 10 : 12;
        const auto v = scene(1279, 1023, bits, 7, 8);
        expect(fmt::pack(f, v.data(), v.size()), 1279, 1023, f, 0, f);
    }
    expect(mono8(scene(1280, 1024, 8, 8, 2, true), 1280, 1024), 1280, 1024, "Mono8", 0, "Bayer 8 位");
    expect(mono16(scene(1279, 1023, 12, 9, 10, true)), 1279, 1023, "Mono12", 0, "Bayer 12 位");
}

TEST(rawguess_not_images) {
    const Bytes r = testing::random_bytes(1 << 20, 11);
    CHECK(!rawguess::guess(r.data(), r.size()));
    Bytes png = mono8(scene(512, 512, 8, 12, 2), 512, 512);  // 像图像，但开头是 PNG 标识：不猜
    std::memcpy(png.data(), "\x89PNG\r\n\x1a\n", 8);
    CHECK(!rawguess::guess(png.data(), png.size()));
    const Bytes tiny(500, 7);  // 不到 64×8 像素：不猜
    CHECK(!rawguess::guess(tiny.data(), tiny.size()));
}

TEST(rawguess_compress) {
    const auto v = scene(1279, 1023, 12, 13, 12);
    const Bytes d = fmt::pack("Mono12p", v.data(), v.size());
    auto run = [&](int raw_auto, iis_compress_result& r) {
        iis_compress_options o{};
        o.raw_auto = raw_auto;
        iis_buffer c{};
        CHECK_EQ(iis_compress(d.data(), d.size(), &o, &c, &r), IIS_OK);
        // 逐字节还原
        iis_reader* rd = nullptr;
        iis_buffer out{};
        CHECK_EQ(iis_reader_open_memory(c.data, c.size, &rd), IIS_OK);
        CHECK_EQ(iis_reader_restore(rd, &out), IIS_OK);
        CHECK(out.size == d.size() && std::memcmp(out.data, d.data(), d.size()) == 0);
        iis_buffer_free(&out);
        iis_reader_close(rd);
        iis_buffer_free(&c);
    };
    iis_compress_result off{}, on{};
    run(0, off);
    run(1, on);
    CHECK_EQ(off.method, int(IIS_METHOD_ZSTD));  // 默认不猜：与以前相同
    CHECK_EQ(on.method, int(IIS_METHOD_IMAGE));
    CHECK(std::strstr(on.detail, "自动识别") != nullptr);
    CHECK(on.stored_size < off.stored_size);

    // C 接口的识别结果也带上自动识别的参数
    iis_compress_options o{};
    o.raw_auto = 1;
    iis_buffer j{};
    CHECK_EQ(iis_detect(d.data(), d.size(), &o, &j), IIS_OK);
    const std::string js(reinterpret_cast<char*>(j.data), j.size);
    CHECK(js.find("\"width\":1279") != std::string::npos && js.find("\"height\":1023") != std::string::npos);
    iis_buffer_free(&j);

    // 随机数据：开了 raw_auto 也照常兜底
    const Bytes r = testing::random_bytes(300000, 14);
    iis_buffer c{};
    iis_compress_result rr{};
    CHECK_EQ(iis_compress(r.data(), r.size(), &o, &c, &rr), IIS_OK);
    CHECK_EQ(rr.method, int(IIS_METHOD_STORE));
    iis_buffer_free(&c);
}
