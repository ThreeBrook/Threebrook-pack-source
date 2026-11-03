// 格式处理器（T3）：在 C++ 里生成 BMP / RAW / TIFF 变体 → 压缩 → 只凭容器还原 → 逐字节一致；坏文件头模糊测试。
// 与 Python 参考实现的逐项对照见 bench/tests/test_native_formats.py；这里保证没有 Python 的平台（Linux CI）同样覆盖。
#include "testing.hpp"

#include "iis_decode.hpp"

#include "../src/formats.hpp"
#include "../src/tiffcodec.hpp"
#include "iis/iis.h"

#include <cstring>
#include <array>
#include <functional>
#include <string>

namespace {

using Bytes = std::vector<uint8_t>;

void le(Bytes& v, uint64_t x, int n) {
    for (int i = 0; i < n; ++i) v.push_back(uint8_t(x >> (8 * i)));
}

void put_be(Bytes& v, size_t at, uint64_t x, int n) {
    for (int i = 0; i < n; ++i) v[at + i] = uint8_t(x >> (8 * (n - 1 - i)));
}

void put_le(Bytes& v, size_t at, uint64_t x, int n) {
    for (int i = 0; i < n; ++i) v[at + i] = uint8_t(x >> (8 * i));
}

// 平滑渐变 + 小噪声，可压缩
uint32_t pixel(uint32_t x, uint32_t y, uint32_t c, uint32_t bits, std::mt19937& rng) {
    const uint32_t maxv = (1u << bits) - 1;
    return std::min<uint32_t>(maxv, ((x * 5 + y * 3 + c * 40) % (maxv + 1)) + rng() % 3);
}

Bytes make_bmp(uint32_t w, uint32_t h, int ch, bool top_down, bool pad_junk, size_t trailer, uint32_t seed) {
    std::mt19937 rng(seed);
    const uint32_t bits = 8u * uint32_t(ch);
    const uint32_t stride = (w * bits + 31) / 32 * 4;
    const uint32_t pal = ch == 1 ? 1024 : 0, off = 54 + pal, size = off + stride * h + uint32_t(trailer);
    Bytes b = {'B', 'M'};
    le(b, size, 4);
    le(b, 0, 4);
    le(b, off, 4);
    le(b, 40, 4);
    le(b, w, 4);
    le(b, top_down ? uint32_t(-int32_t(h)) : h, 4);
    le(b, 1, 2);
    le(b, bits, 2);
    le(b, 0, 4);
    le(b, stride * h, 4);
    le(b, 2835, 4);
    le(b, 2835, 4);
    le(b, ch == 1 ? 256 : 0, 4);
    le(b, 0, 4);
    for (uint32_t i = 0; i < pal / 4; ++i) b.insert(b.end(), {uint8_t(i), uint8_t(i), uint8_t(i), 0});
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x)
            for (int c = 0; c < ch; ++c) b.push_back(uint8_t(c == 3 ? 255 : pixel(x, y, uint32_t(c), 8, rng)));
        for (uint32_t k = w * uint32_t(ch); k < stride; ++k) b.push_back(pad_junk ? uint8_t(rng()) : 0);
    }
    for (size_t k = 0; k < trailer; ++k) b.push_back(uint8_t('T'));
    return b;
}

// 经典 TIFF：单页、条带；compression 1 / 5（libtiff LZW）/ 8（zlib）/ 32773（PackBits 按行）
Bytes make_tiff(uint32_t w, uint32_t h, uint32_t spp, uint32_t bits, uint32_t rps, int compression, int predictor,
                bool big_endian, uint32_t seed) {
    std::mt19937 rng(seed);
    const uint32_t bps = bits / 8, rb = w * spp * bps, ns = (h + rps - 1) / rps;
    std::vector<Bytes> strips;
    for (uint32_t s = 0; s < ns; ++s) {
        const uint32_t rows = std::min(rps, h - s * rps);
        Bytes raw(size_t(rows) * rb);
        for (uint32_t r = 0; r < rows; ++r) {
            std::vector<uint32_t> prev(spp, 0);
            for (uint32_t x = 0; x < w; ++x)
                for (uint32_t c = 0; c < spp; ++c) {
                    uint32_t v = pixel(x, s * rps + r, c, bits == 16 ? 12 : 8, rng);
                    if (bits == 16) v <<= 4;  // MSB 对齐的 12 位
                    const uint32_t stored = predictor == 2 ? (v - prev[c]) & ((1u << bits) - 1) : v;
                    prev[c] = v;
                    const size_t at = size_t(r) * rb + (size_t(x) * spp + c) * bps;
                    if (bps == 1) raw[at] = uint8_t(stored);
                    else if (big_endian) put_be(raw, at, stored, 2);
                    else put_le(raw, at, stored, 2);
                }
        }
        using namespace iis::tiffcodec;
        if (compression == 5) raw = lzw_encode_libtiff(raw.data(), raw.size());
        else if (compression == 8) raw = zlib_encode(raw.data(), raw.size(), 6);
        else if (compression == 32773) raw = packbits_encode_rows(raw.data(), raw.size(), rb);
        strips.push_back(std::move(raw));
    }
    Bytes f(8, 0);
    auto put = [&](size_t at, uint64_t x, int n) { big_endian ? put_be(f, at, x, n) : put_le(f, at, x, n); };
    f[0] = f[1] = big_endian ? 'M' : 'I';
    put(2, 42, 2);
    std::vector<uint32_t> offs, cnts;
    for (const auto& s : strips) {
        offs.push_back(uint32_t(f.size()));
        cnts.push_back(uint32_t(s.size()));
        f.insert(f.end(), s.begin(), s.end());
    }
    auto array = [&](const std::vector<uint32_t>& v) {
        const uint32_t at = uint32_t(f.size());
        for (auto x : v) {
            f.resize(f.size() + 4);
            put(f.size() - 4, x, 4);
        }
        return at;
    };
    const uint32_t off_at = ns > 1 ? array(offs) : offs[0], cnt_at = ns > 1 ? array(cnts) : cnts[0];
    std::vector<std::array<uint32_t, 4>> tags = {
        {256, 4, 1, w}, {257, 4, 1, h}, {258, 3, 1, bits}, {259, 3, 1, uint32_t(compression)},
        {262, 3, 1, spp == 3 ? 2u : 1u}, {273, 4, ns, off_at}, {277, 3, 1, spp}, {278, 4, 1, rps},
        {279, 4, ns, cnt_at}, {284, 3, 1, 1}};
    if (predictor == 2) tags.push_back({317, 3, 1, 2});
    if (f.size() % 2) f.push_back(0);
    const uint32_t ifd = uint32_t(f.size());
    put(4, ifd, 4);
    f.resize(f.size() + 2 + 12 * tags.size() + 4, 0);
    put(ifd, tags.size(), 2);
    for (size_t i = 0; i < tags.size(); ++i) {
        const size_t e = ifd + 2 + 12 * i;
        put(e, tags[i][0], 2);
        put(e + 2, tags[i][1], 2);
        put(e + 4, tags[i][2], 4);
        if (tags[i][1] == 3 && tags[i][2] == 1) put(e + 8, tags[i][3], 2);  // SHORT 左对齐存放
        else put(e + 8, tags[i][3], 4);
    }
    return f;
}

Bytes make_raw(const std::string& fmt, uint32_t w, uint32_t h, size_t header, uint32_t seed) {
    std::mt19937 rng(seed);
    Bytes b(header, 0xAB);
    const iis::fmt::PixelFormat* pf = iis::fmt::find_pixel_format(fmt);
    std::vector<uint16_t> v(size_t(w) * h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) v[size_t(y) * w + x] = uint16_t(pixel(x, y, 0, uint32_t(pf->bits), rng));
    if (pf->packed) {
        const auto p = iis::fmt::pack(pf->packed, v.data(), v.size());
        b.insert(b.end(), p.begin(), p.end());
    } else {
        for (auto x : v) {
            b.push_back(uint8_t(x));
            if (pf->bpp == 2) b.push_back(uint8_t(x >> 8));
        }
    }
    return b;
}

// 压缩 → 只凭容器还原 → 与输入相同；返回 method。split_ok：按图像拆分成功（含“压不小、保留原文件”）
int g_decoder_checked = 0;  // 经开源解码器核对过的容器数

int roundtrip(const Bytes& data, const iis_compress_options* opt = nullptr, bool* split_ok = nullptr) {
    iis_buffer c{}, out{};
    iis_compress_result res{};
    if (iis_compress(data.data(), data.size(), opt, &c, &res) != IIS_OK) {
        testing::report(__FILE__, __LINE__, std::string("压缩失败：") + iis_last_error());
        return 0;
    }
    if (split_ok)
        *split_ok = res.method == IIS_METHOD_IMAGE || std::string(res.detail).find("不小于原文件") != std::string::npos;
    if (res.method != IIS_METHOD_STORE) {
        iis_reader* r = nullptr;
        const bool ok = iis_reader_open_memory(c.data, c.size, &r) == IIS_OK && iis_reader_restore(r, &out) == IIS_OK &&
                        out.size == data.size() && (data.empty() || std::memcmp(out.data, data.data(), data.size()) == 0);
        if (!ok) testing::report(__FILE__, __LINE__, std::string("还原不一致：") + res.detail + " / " + iis_last_error());
        iis_reader_close(r);
        // 独立开源解码器（decoder/）也必须还原出同样的字节
        std::vector<uint8_t> dec;
        std::string err;
        if (!iis_decode::restore(c.data, c.size, dec, err) || dec != data)
            testing::report(__FILE__, __LINE__, std::string("开源解码器还原不一致：") + res.detail + " / " + err);
        else
            ++g_decoder_checked;
    }
    iis_buffer_free(&out);
    iis_buffer_free(&c);
    return res.method;
}

}  // namespace

TEST(formats_bmp_variants) {
    int seed = 0;
    for (int ch : {1, 3, 4})
        for (bool td : {false, true})
            for (bool junk : {false, true})
                for (uint32_t w : {33u, 64u, 101u}) CHECK_EQ(roundtrip(make_bmp(w, 47, ch, td, junk, junk ? 5 : 0, ++seed)), IIS_METHOD_IMAGE);
}

TEST(formats_tiff_variants) {
    int seed = 100;
    for (int comp : {1, 5, 8, 32773})
        for (uint32_t bits : {8u, 16u})
            for (uint32_t spp : {1u, 3u})
                for (bool be : {false, true}) {
                    const int pred = (comp == 5 || comp == 8) && (seed % 2) ? 2 : 1;
                    bool split_ok = false;  // 已压缩的小 TIFF 拆得开但可能压不小，此时保留原文件
                    roundtrip(make_tiff(77, 41, spp, bits, 7, comp, pred, be, uint32_t(++seed)), nullptr, &split_ok);
                    CHECK(split_ok);
                }
}

TEST(formats_raw_variants_with_rules) {
    const std::string rules_json =
        R"({"raw": [{"match": "cam1/*.raw", "width": 61, "height": 33, "pixel_format": "Mono12p"},)"
        R"( {"match": "cam2/*.raw", "width": 61, "height": 33, "pixel_format": "Mono12Packed", "offset": 16},)"
        R"( {"match": "cam3/*.raw", "width": 61, "height": 33, "pixel_format": "Mono10p"},)"
        R"( {"match": "cam4/*.raw", "width": 64, "height": 40, "pixel_format": "Mono16"},)"
        R"( {"match": "cam5/*.raw", "width": 64, "height": 40, "pixel_format": "BayerRG8"}]})";
    iis_raw_rules* rules = nullptr;
    CHECK_EQ(iis_raw_rules_parse(rules_json.data(), rules_json.size(), &rules), IIS_OK);
    struct Case {
        const char* rel;
        const char* fmt;
        uint32_t w, h;
        size_t header;
    } cases[] = {{"cam1/a.raw", "Mono12p", 61, 33, 0},  {"cam2/a.raw", "Mono12Packed", 61, 33, 16},
                 {"cam3/a.raw", "Mono10p", 61, 33, 0},  {"cam4/a.raw", "Mono16", 64, 40, 0},
                 {"cam5/a.raw", "BayerRG8", 64, 40, 0}};
    for (const auto& c : cases) {
        iis_compress_options opt{19, rules, c.rel, nullptr};
        CHECK_EQ(roundtrip(make_raw(c.fmt, c.w, c.h, c.header, 7), &opt), IIS_METHOD_IMAGE);
    }
    // 没有规则命中：不猜参数，整文件兜底
    iis_compress_options none{19, rules, "other/a.raw", nullptr};
    CHECK(roundtrip(make_raw("Mono8", 64, 40, 0, 8), &none) != IIS_METHOD_IMAGE);
    // 同名参数文件
    const char* side = R"({"width": 61, "height": 33, "pixel_format": "Mono12p"})";
    iis_compress_options sc{19, rules, "other/b.raw", side};
    CHECK_EQ(roundtrip(make_raw("Mono12p", 61, 33, 0, 9), &sc), IIS_METHOD_IMAGE);
    iis_raw_rules_free(rules);
    CHECK(iis::fmt::glob_match("a/*.raw", "a/b/c.raw"));  // fnmatch：* 跨越 /
    CHECK(iis::fmt::glob_match("st[0-9]/x?.raw", "st3/x1.raw"));
    CHECK(!iis::fmt::glob_match("st[!0-9]/x.raw", "st3/x.raw"));
#ifdef _WIN32
    CHECK(iis::fmt::glob_match("CAM4/*.RAW", "cam4/a.raw"));  // 与 Python fnmatch 相同：Windows 不区分大小写
#else
    CHECK(!iis::fmt::glob_match("CAM4/*.RAW", "cam4/a.raw"));
#endif
}

TEST(formats_fuzz_headers) {
    const std::vector<Bytes> bases = {make_bmp(40, 30, 1, false, false, 0, 1), make_bmp(17, 9, 3, true, true, 3, 2),
                                      make_tiff(40, 30, 1, 16, 6, 5, 2, false, 3), make_tiff(33, 20, 3, 8, 5, 8, 1, true, 4),
                                      make_tiff(30, 25, 1, 8, 4, 32773, 1, false, 5)};
    std::mt19937 rng(77);
    int image = 0, other = 0;
    for (int i = 0; i < 3000; ++i) {
        Bytes d = bases[rng() % bases.size()];
        const int k = 1 + int(rng() % 6);
        for (int j = 0; j < k; ++j) {
            const size_t pos = rng() % 5 ? rng() % std::min<size_t>(d.size(), 300) : rng() % d.size();
            d[pos] = uint8_t(rng());
        }
        if (rng() % 10 == 0) d.resize(1 + rng() % d.size());
        const int m = roundtrip(d);  // 不崩溃；任何路径的还原都必须与输入相同
        (m == IIS_METHOD_IMAGE ? image : other)++;
    }
    CHECK(image > 300 && other > 300);
}

// 开源解码器：每个单字节改动都必须被发现（报错，不崩溃，不给出错误结果）
TEST(decoder_rejects_corruption) {
    for (const Bytes& data : {make_bmp(101, 47, 1, false, true, 5, 11), make_tiff(160, 120, 1, 16, 16, 5, 2, false, 12),
                              make_bmp(101, 47, 3, true, false, 0, 14)}) {
        iis_buffer c{};
        iis_compress_result res{};
        CHECK_EQ(iis_compress(data.data(), data.size(), nullptr, &c, &res), IIS_OK);
        if (res.method != IIS_METHOD_IMAGE) std::printf("  样本未按图像压缩：%s\n", res.detail);
        CHECK(res.method == IIS_METHOD_IMAGE);
        std::vector<uint8_t> cont(c.data, c.data + c.size), out;
        iis_buffer_free(&c);
        std::string err;
        CHECK(iis_decode::restore(cont.data(), cont.size(), out, err) && out == data);
        int caught = 0;
        for (size_t i = 0; i < cont.size(); ++i) {
            cont[i] ^= 0x5A;
            caught += !iis_decode::restore(cont.data(), cont.size(), out, err);
            cont[i] ^= 0x5A;
        }
        CHECK_EQ(caught, int(cont.size()));
        for (size_t len = 0; len < cont.size(); len += 7)  // 截断
            CHECK(!iis_decode::restore(cont.data(), len, out, err));
    }
    std::printf("  经开源解码器逐字节核对的容器：%d 个\n", g_decoder_checked);
}
