// 容器 v1：往返、确定性、逐字节损坏检测、截断、版本兼容规则、写入参数校验、文件读写。
// RawBuilder 按 spec/container-v1.md 独立拼装容器，用来对照库的输出与构造库自己写不出的文件（未来版本、未知段）。
#include "testing.hpp"

#include "iis/iis.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

void le(std::vector<uint8_t>& v, uint64_t x, int n) {
    for (int i = 0; i < n; ++i) v.push_back(uint8_t(x >> (8 * i)));
}
void set32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
    for (int i = 0; i < 4; ++i) v[at + i] = uint8_t(x >> (8 * i));
}
uint32_t crc(const uint8_t* p, size_t n) { return iis_crc32c(0, p, n); }

struct RawSeg {
    uint16_t type, codec, flags, id;
    std::vector<uint8_t> meta, data;
    uint64_t raw_len;
};

struct RawBuilder {
    uint16_t major = 1, minor = 0;
    uint32_t header_size = 72, incompat = 0, compat = 0, entry_size = 40;
    uint64_t orig_size = 0;
    uint8_t sha[32] = {};
    std::vector<RawSeg> segs;

    std::vector<uint8_t> build() const {
        std::vector<uint8_t> f;
        const uint8_t magic[8] = {'I', 'I', 'S', 'C', 'O', 'N', 'T', 0};
        f.insert(f.end(), magic, magic + 8);
        le(f, major, 2);
        le(f, minor, 2);
        le(f, header_size, 4);
        le(f, incompat, 4);
        le(f, compat, 4);
        le(f, orig_size, 8);
        f.insert(f.end(), sha, sha + 32);
        f.resize(header_size - 4, 0);
        le(f, crc(f.data(), f.size()), 4);
        std::vector<uint8_t> idx = {'I', 'I', 'D', 'X'};
        le(idx, segs.size(), 4);
        le(idx, entry_size, 4);
        le(idx, 0, 4);
        for (const auto& s : segs) {
            const uint64_t off = f.size();
            const uint32_t dcrc = iis_crc32c(crc(s.meta.data(), s.meta.size()), s.data.data(), s.data.size());
            const size_t h = f.size();
            const uint8_t sm[4] = {'I', 'S', 'E', 'G'};
            f.insert(f.end(), sm, sm + 4);
            le(f, s.type, 2);
            le(f, s.codec, 2);
            le(f, s.flags, 2);
            le(f, s.id, 2);
            le(f, s.meta.size(), 4);
            le(f, s.raw_len, 8);
            le(f, s.data.size(), 8);
            le(f, dcrc, 4);
            le(f, crc(&f[h], 36), 4);
            f.insert(f.end(), s.meta.begin(), s.meta.end());
            f.insert(f.end(), s.data.begin(), s.data.end());
            const size_t e = idx.size();
            le(idx, off, 8);
            le(idx, s.type, 2);
            le(idx, s.codec, 2);
            le(idx, s.flags, 2);
            le(idx, s.id, 2);
            le(idx, s.meta.size(), 4);
            le(idx, dcrc, 4);
            le(idx, s.raw_len, 8);
            le(idx, s.data.size(), 8);
            idx.resize(e + entry_size, 0);
        }
        le(idx, crc(idx.data(), idx.size()), 4);
        const uint64_t index_offset = f.size();
        f.insert(f.end(), idx.begin(), idx.end());
        std::vector<uint8_t> t;
        le(t, index_offset, 8);
        le(t, idx.size(), 8);
        le(t, f.size() + 32, 8);
        le(t, crc(t.data(), 24), 4);
        const uint8_t em[4] = {'I', 'E', 'N', 'D'};
        t.insert(t.end(), em, em + 4);
        f.insert(f.end(), t.begin(), t.end());
        return f;
    }
};

std::vector<uint8_t> bytes_of(const std::string& s) { return {s.begin(), s.end()}; }

std::string recipe_text() {
    iis_reader* r = nullptr;
    iis_buffer c{}, j{};
    iis_pack_bytes("x", 1, 0, &c);
    iis_reader_open_memory(c.data, c.size, &r);
    iis_reader_recipe(r, &j);
    std::string s(reinterpret_cast<char*>(j.data), j.size);
    iis_buffer_free(&j);
    iis_reader_close(r);
    iis_buffer_free(&c);
    return s;
}

// 兜底容器的 RawBuilder 版本：原文件整体作为一个不压缩的原样字节段
RawBuilder raw_bytes_container(const std::vector<uint8_t>& data) {
    RawBuilder b;
    b.orig_size = data.size();
    iis_sha256(data.data(), data.size(), b.sha);
    const auto recipe = bytes_of(recipe_text());
    b.segs.push_back({1, 0, 1, 0, {}, recipe, recipe.size()});
    std::vector<uint8_t> meta = {1, 0};  // count=1, gap=0
    for (uint64_t v = data.size();;) {
        meta.push_back(uint8_t((v & 0x7F) | (v >= 0x80 ? 0x80 : 0)));
        v >>= 7;
        if (!v) break;
    }
    b.segs.push_back({2, 0, 1, 0, meta, data, data.size()});
    return b;
}

bool open_ok(const std::vector<uint8_t>& f, iis_reader** r) {
    return iis_reader_open_memory(f.data(), f.size(), r) == IIS_OK;
}

// 一个“完整”容器：配方、两段原样字节、JPEG-LS 平面、zstd 附加字节、描述信息
std::vector<uint8_t> rich_container(std::vector<uint8_t>* plane_px = nullptr) {
    const uint64_t orig_size = 5000;
    uint8_t sha[32];
    std::vector<uint8_t> orig = testing::random_bytes(orig_size, 7);
    iis_sha256(orig.data(), orig.size(), sha);
    iis_writer* w = nullptr;
    CHECK_EQ(iis_writer_create_memory(orig_size, sha, &w), IIS_OK);
    const std::string recipe =
        R"({"extras":[{"id":0,"image":0,"role":"row_padding"}],"format":"bmp","iis_recipe":1,"layout":{},)"
        R"("planes":[{"codec":"jpegls","id":3,"image":0}],"predictor":null,"writer":{"libs":{},"name":"t","version":"0"}})";
    CHECK_EQ(iis_writer_add(w, IIS_SEG_RECIPE, IIS_CODEC_NONE, 0, recipe.size(), recipe.data(), recipe.size()), IIS_OK);
    const iis_range r1[] = {{0, 54}, {100, 20}};
    std::vector<uint8_t> s1(orig.begin(), orig.begin() + 54);
    s1.insert(s1.end(), orig.begin() + 100, orig.begin() + 120);
    CHECK_EQ(iis_writer_add_skeleton(w, 0, IIS_CODEC_NONE, r1, 2, s1.data(), s1.size()), IIS_OK);
    const iis_range r2[] = {{4000, 1000}};
    iis_buffer z{};
    iis_zstd_compress(orig.data() + 4000, 1000, 3, &z);
    CHECK_EQ(iis_writer_add_skeleton(w, 1, IIS_CODEC_ZSTD, r2, 1, z.data, z.size), IIS_OK);
    iis_buffer_free(&z);
    const iis_image_info info{32, 16, 1, 8};
    std::vector<uint8_t> px(32 * 16);
    for (size_t i = 0; i < px.size(); ++i) px[i] = uint8_t(i / 3);
    iis_buffer ls{};
    CHECK_EQ(iis_jpegls_encode(px.data(), px.size(), &info, &ls), IIS_OK);
    CHECK_EQ(iis_writer_add(w, IIS_SEG_PLANE, IIS_CODEC_JPEGLS, 3, px.size(), ls.data, ls.size), IIS_OK);
    iis_buffer_free(&ls);
    const std::string pad(300, 'p');
    iis_zstd_compress(pad.data(), pad.size(), 3, &z);
    CHECK_EQ(iis_writer_add(w, IIS_SEG_EXTRA, IIS_CODEC_ZSTD, 0, pad.size(), z.data, z.size), IIS_OK);
    iis_buffer_free(&z);
    const std::string meta = R"({"source_name":"相机1_0001.bmp","verdict":"NG"})";
    CHECK_EQ(iis_writer_add(w, IIS_SEG_META, IIS_CODEC_NONE, 0, meta.size(), meta.data(), meta.size()), IIS_OK);
    iis_buffer out{};
    CHECK_EQ(iis_writer_finish(w, 0, &out), IIS_OK);
    iis_writer_free(w);
    std::vector<uint8_t> f(out.data, out.data + out.size);
    iis_buffer_free(&out);
    if (plane_px) *plane_px = px;
    return f;
}

std::string tmp_path(const char* name) {
    std::string p = std::string("iis_test_") + name;
    std::remove(p.c_str());
    return p;
}

}  // namespace

TEST(container_pack_bytes_roundtrip) {
    for (size_t n : {size_t(0), size_t(1), size_t(1000), size_t(3) << 20}) {
        auto data = testing::random_bytes(n, uint32_t(n) + 1);
        for (size_t i = 0; i < n / 3; ++i) data[i] = uint8_t(i >> 10);
        for (int level : {0, 3, 19}) {
            if (level == 19 && n > 100000) continue;
            iis_buffer c{}, out{};
            CHECK_EQ(iis_pack_bytes(data.data(), data.size(), level, &c), IIS_OK);
            iis_reader* r = nullptr;
            CHECK_EQ(iis_reader_open_memory(c.data, c.size, &r), IIS_OK);
            iis_container_info info{};
            CHECK_EQ(iis_reader_info(r, &info), IIS_OK);
            CHECK_EQ(info.orig_size, uint64_t(n));
            CHECK_EQ(info.container_size, uint64_t(c.size));
            CHECK_EQ(info.restorable, 1);
            CHECK_EQ(info.segment_count, n ? 2u : 1u);
            CHECK_EQ(iis_reader_verify(r, 1), IIS_OK);
            CHECK_EQ(iis_reader_restore(r, &out), IIS_OK);
            CHECK(out.size == n && (n == 0 || std::memcmp(out.data, data.data(), n) == 0));
            iis_buffer_free(&out);
            iis_reader_close(r);
            iis_buffer_free(&c);
        }
    }
}

TEST(container_matches_independent_builder_and_is_deterministic) {
    const auto data = testing::random_bytes(777, 3);
    iis_buffer a{}, b{};
    CHECK_EQ(iis_pack_bytes(data.data(), data.size(), 0, &a), IIS_OK);
    CHECK_EQ(iis_pack_bytes(data.data(), data.size(), 0, &b), IIS_OK);
    CHECK(a.size == b.size && std::memcmp(a.data, b.data, a.size) == 0);
    const auto ref = raw_bytes_container(data).build();
    CHECK(ref.size() == a.size && std::memcmp(ref.data(), a.data, a.size) == 0);
    iis_buffer_free(&a);
    iis_buffer_free(&b);
}

TEST(container_rich_segments) {
    std::vector<uint8_t> px;
    const auto f = rich_container(&px);
    iis_reader* r = nullptr;
    if (!open_ok(f, &r)) {
        std::printf("  打开失败：%s\n", iis_last_error());
        CHECK(false);
        return;
    }
    iis_container_info info{};
    iis_reader_info(r, &info);
    CHECK_EQ(info.segment_count, 6u);
    CHECK_EQ(iis_reader_verify(r, 0), IIS_OK);
    uint32_t i = 0;
    CHECK_EQ(iis_reader_find(r, IIS_SEG_PLANE, 3, &i), IIS_OK);
    iis_buffer out{};
    CHECK_EQ(iis_reader_decode(r, i, &out), IIS_OK);
    CHECK(out.size == px.size() && std::memcmp(out.data, px.data(), px.size()) == 0);
    iis_buffer_free(&out);
    CHECK_EQ(iis_reader_find(r, IIS_SEG_PLANE, 4, &i), IIS_E_NOT_FOUND);
    CHECK_EQ(iis_reader_find(r, IIS_SEG_SKELETON, 0, &i), IIS_OK);
    CHECK_EQ(iis_reader_ranges(r, i, &out), IIS_OK);
    CHECK_EQ(out.size, 2 * sizeof(iis_range));
    const auto* rg = reinterpret_cast<const iis_range*>(out.data);
    CHECK(rg[0].offset == 0 && rg[0].length == 54 && rg[1].offset == 100 && rg[1].length == 20);
    iis_buffer_free(&out);
    CHECK_EQ(iis_reader_find(r, IIS_SEG_EXTRA, 0, &i), IIS_OK);
    CHECK_EQ(iis_reader_decode(r, i, &out), IIS_OK);
    CHECK(out.size == 300 && out.data[299] == 'p');
    iis_buffer_free(&out);
    // 配方的 layout 是空对象：格式处理器拒绝重组并报告配方与数据不符，不假装还原
    CHECK_EQ(iis_reader_restore(r, &out), IIS_E_CORRUPT);
    CHECK(std::string(iis_last_error()).find("配方") != std::string::npos);
    iis_reader_close(r);
}

TEST(container_every_byte_corruption_detected) {
    // 兜底容器（深度校验）与完整容器（结构校验）：逐字节改动，必须每一处都报错
    const auto small = testing::random_bytes(600, 11);
    iis_buffer c{};
    iis_pack_bytes(small.data(), small.size(), 3, &c);
    const std::vector<uint8_t> fallback(c.data, c.data + c.size);
    iis_buffer_free(&c);
    const auto rich = rich_container();
    std::mt19937 rng(12);
    for (const auto* f : {&fallback, &rich}) {
        int missed = 0;
        for (size_t pos = 0; pos < f->size(); ++pos) {
            auto bad = *f;
            bad[pos] ^= uint8_t(1 + rng() % 255);
            iis_reader* r = nullptr;
            bool detected = !open_ok(bad, &r);
            if (!detected) detected = iis_reader_verify(r, f == &fallback ? 1 : 0) != IIS_OK;
            iis_reader_close(r);
            if (!detected) ++missed;
        }
        CHECK_EQ(missed, 0);
        // 每一种截断长度、尾部追加都要报错
        int trunc_missed = 0;
        for (size_t len = 0; len < f->size(); ++len) {
            iis_reader* r = nullptr;
            if (iis_reader_open_memory(f->data(), len, &r) == IIS_OK) ++trunc_missed;
            iis_reader_close(r);
        }
        CHECK_EQ(trunc_missed, 0);
        auto longer = *f;
        longer.push_back(0);
        iis_reader* r = nullptr;
        CHECK(!open_ok(longer, &r));
    }
}

TEST(container_random_damage_and_garbage_never_crash) {
    const auto rich = rich_container();
    std::mt19937 rng(13);
    for (int round = 0; round < 3000; ++round) {
        auto bad = rich;
        if (round % 3 == 0) {
            bad = testing::random_bytes(rng() % 400, uint32_t(round));  // 纯垃圾
        } else {
            for (int k = 0; k < 1 + int(rng() % 8); ++k) bad[rng() % bad.size()] = uint8_t(rng());
        }
        iis_reader* r = nullptr;
        if (open_ok(bad, &r)) {
            iis_buffer out{};
            CHECK(iis_reader_verify(r, 1) != IIS_OK);
            iis_container_info info{};
            iis_reader_info(r, &info);
            for (uint32_t i = 0; i < info.segment_count; ++i) {
                iis_reader_decode(r, i, &out);
                iis_buffer_free(&out);
            }
            iis_reader_close(r);
        }
    }
}

TEST(container_version_rules) {
    const auto data = testing::random_bytes(100, 5);
    iis_reader* r = nullptr;
    iis_buffer out{};
    auto expect_open = [&](RawBuilder b, int want) {
        const auto f = b.build();
        const int st = iis_reader_open_memory(f.data(), f.size(), &r);
        CHECK_EQ(st, want);
        if (st == IIS_OK) {
            CHECK_EQ(iis_reader_restore(r, &out), IIS_OK);
            CHECK_EQ(out.size, data.size());
            iis_buffer_free(&out);
            iis_reader_close(r);
        }
        r = nullptr;
    };
    RawBuilder b = raw_bytes_container(data);
    expect_open(b, IIS_OK);
    { auto x = b; x.major = 2; expect_open(x, IIS_E_UNSUPPORTED); }
    { auto x = b; x.major = 0; expect_open(x, IIS_E_UNSUPPORTED); }
    { auto x = b; x.minor = 9; expect_open(x, IIS_OK); }          // 更高次版本照常读
    { auto x = b; x.header_size = 96; expect_open(x, IIS_OK); }   // 加长的文件头
    { auto x = b; x.entry_size = 56; expect_open(x, IIS_OK); }    // 加长的段表项
    { auto x = b; x.compat = 0x80; expect_open(x, IIS_OK); }      // 不认识的兼容特性位：忽略
    { auto x = b; x.incompat = 1; expect_open(x, IIS_E_UNSUPPORTED); }  // 预测层：拒绝
    { auto x = b; x.incompat = 0x40; expect_open(x, IIS_E_UNSUPPORTED); }
    { auto x = b; x.header_size = 60; expect_open(x, IIS_E_CORRUPT); }
    { auto x = b; x.entry_size = 32; expect_open(x, IIS_E_CORRUPT); }
    // 不认识的非关键段：跳过，照常还原
    { auto x = b; x.segs.push_back({200, 0, 0, 0, {9, 9}, {1, 2, 3}, 3}); expect_open(x, IIS_OK); }
    // 不认识的关键段：能打开，不能还原
    {
        auto x = b;
        x.segs.push_back({201, 0, 1, 0, {}, {1}, 1});
        const auto f = x.build();
        CHECK_EQ(iis_reader_open_memory(f.data(), f.size(), &r), IIS_OK);
        iis_container_info info{};
        iis_reader_info(r, &info);
        CHECK_EQ(info.restorable, 0);
        CHECK_EQ(iis_reader_restore(r, &out), IIS_E_UNSUPPORTED);
        iis_reader_close(r);
    }
    // 结构性违规
    { auto x = b; x.segs[0].flags = 0; expect_open(x, IIS_E_CORRUPT); }                // 配方必须是关键段
    { auto x = b; x.segs.erase(x.segs.begin()); expect_open(x, IIS_E_CORRUPT); }       // 缺配方
    { auto x = b; x.segs.push_back(x.segs[1]); expect_open(x, IIS_E_CORRUPT); }        // (type,id) 重复
    { auto x = b; x.segs[0].meta = {1}; expect_open(x, IIS_E_CORRUPT); }               // 配方不应带 meta
    { auto x = b; x.segs[1].raw_len += 1; expect_open(x, IIS_E_CORRUPT); }             // none 但长度不一致
}

TEST(container_skeleton_range_rules) {
    const auto data = testing::random_bytes(300, 6);
    RawBuilder b = raw_bytes_container(data);
    iis_reader* r = nullptr;
    iis_buffer out{};
    auto restore_status = [&](RawBuilder x) {
        const auto f = x.build();
        int st = iis_reader_open_memory(f.data(), f.size(), &r);
        if (st == IIS_OK) st = iis_reader_restore(r, &out);
        iis_buffer_free(&out);
        iis_reader_close(r);
        r = nullptr;
        return st;
    };
    CHECK_EQ(restore_status(b), IIS_OK);
    { auto x = b; x.segs[1].meta = {1, 0, 0xAC, 0x82, 0x00}; CHECK_EQ(restore_status(x), IIS_E_CORRUPT); }  // 非最短编码
    { auto x = b; x.segs[1].meta = {1, 1, 0xAC, 0x02}; CHECK_EQ(restore_status(x), IIS_E_CORRUPT); }        // 越过原文件末尾
    { auto x = b; x.segs[1].meta = {2, 0, 0x96, 0x01, 0, 0x96, 0x01}; CHECK_EQ(restore_status(x), IIS_OK); }  // 相邻两区间 150+150：合法
    { auto x = b; x.segs[1].meta = {2, 0, 0x96, 0x01, 0, 0x64}; CHECK_EQ(restore_status(x), IIS_E_CORRUPT); }      // 总长 250 ≠ 300
    { auto x = b; x.segs[1].meta = {1, 0, 0xAC, 0x02, 7}; CHECK_EQ(restore_status(x), IIS_E_CORRUPT); }     // 多余字节
    { auto x = b; x.segs[1].meta = {1, 0, 0}; CHECK_EQ(restore_status(x), IIS_E_CORRUPT); }                 // 长度 0
    // 没覆盖全文件：只存前 200 字节
    {
        auto x = b;
        x.segs[1].meta = {1, 0, 0xC8, 0x01};
        x.segs[1].data.resize(200);
        x.segs[1].raw_len = 200;
        CHECK_EQ(restore_status(x), IIS_E_CORRUPT);
    }
    // 内容对但 SHA-256 不对
    { auto x = b; x.sha[0] ^= 1; CHECK_EQ(restore_status(x), IIS_E_CORRUPT); }
}

TEST(container_writer_rejects_bad_input) {
    uint8_t sha[32] = {};
    iis_writer* w = nullptr;
    CHECK_EQ(iis_writer_create_memory(1000, sha, &w), IIS_OK);
    const char* j = "{}";
    CHECK_EQ(iis_writer_add(w, IIS_SEG_RECIPE, IIS_CODEC_NONE, 0, 2, j, 2), IIS_OK);
    CHECK_EQ(iis_writer_add(w, IIS_SEG_RECIPE, IIS_CODEC_NONE, 0, 2, j, 2), IIS_E_INVALID_ARG);   // 重复
    CHECK_EQ(iis_writer_add(w, IIS_SEG_META, IIS_CODEC_NONE, 1, 2, j, 2), IIS_E_INVALID_ARG);     // id 须为 0
    CHECK_EQ(iis_writer_add(w, IIS_SEG_EXTRA, IIS_CODEC_JPEGLS, 0, 2, j, 2), IIS_E_INVALID_ARG);  // 编码不符
    CHECK_EQ(iis_writer_add(w, IIS_SEG_PLANE, IIS_CODEC_HTJ2K, 0, 2, j, 2), IIS_E_INVALID_ARG);   // 预留编码不写
    CHECK_EQ(iis_writer_add(w, IIS_SEG_EXTRA, IIS_CODEC_NONE, 0, 3, j, 2), IIS_E_INVALID_ARG);    // 长度不符
    CHECK_EQ(iis_writer_add(w, 99, IIS_CODEC_NONE, 0, 2, j, 2), IIS_E_INVALID_ARG);
    std::vector<uint8_t> d(100);
    const iis_range bad1[] = {{10, 50}, {40, 50}};
    CHECK_EQ(iis_writer_add_skeleton(w, 0, IIS_CODEC_NONE, bad1, 2, d.data(), 100), IIS_E_INVALID_ARG);  // 重叠
    const iis_range bad2[] = {{950, 100}};
    CHECK_EQ(iis_writer_add_skeleton(w, 0, IIS_CODEC_NONE, bad2, 1, d.data(), 100), IIS_E_INVALID_ARG);  // 越界
    const iis_range ok1[] = {{0, 100}};
    CHECK_EQ(iis_writer_add_skeleton(w, 0, IIS_CODEC_NONE, ok1, 1, d.data(), 100), IIS_OK);
    const iis_range overlap[] = {{50, 100}};
    CHECK_EQ(iis_writer_add_skeleton(w, 1, IIS_CODEC_NONE, overlap, 1, d.data(), 100), IIS_E_INVALID_ARG);  // 跨段重叠
    iis_writer_free(w);

    CHECK_EQ(iis_writer_create_memory(0, sha, &w), IIS_OK);
    iis_buffer out{};
    CHECK_EQ(iis_writer_finish(w, 0, &out), IIS_E_INVALID_ARG);  // 没有配方
    iis_writer_free(w);
}

TEST(container_file_io) {
    const auto data = testing::random_bytes(200000, 21);
    uint8_t sha[32];
    iis_sha256(data.data(), data.size(), sha);
    const std::string path = tmp_path("a.iis"), out_path = tmp_path("a.restored"), partial = tmp_path("p.iis");

    // 流式写文件
    iis_writer* w = nullptr;
    CHECK_EQ(iis_writer_create_file(path.c_str(), data.size(), sha, &w), IIS_OK);
    const std::string recipe = recipe_text();
    CHECK_EQ(iis_writer_add(w, IIS_SEG_RECIPE, IIS_CODEC_NONE, 0, recipe.size(), recipe.data(), recipe.size()), IIS_OK);
    const iis_range halves[2][1] = {{{0, 100000}}, {{100000, 100000}}};
    for (uint16_t k = 0; k < 2; ++k)
        CHECK_EQ(iis_writer_add_skeleton(w, k, IIS_CODEC_NONE, halves[k], 1, data.data() + 100000 * k, 100000), IIS_OK);
    CHECK_EQ(iis_writer_finish(w, 1, nullptr), IIS_OK);
    iis_writer_free(w);

    // 不覆盖已有文件
    CHECK_EQ(iis_writer_create_file(path.c_str(), 1, sha, &w), IIS_E_CONFLICT);

    iis_reader* r = nullptr;
    CHECK_EQ(iis_reader_open_file(path.c_str(), &r), IIS_OK);
    CHECK_EQ(iis_reader_verify(r, 1), IIS_OK);
    CHECK_EQ(iis_reader_restore_file(r, out_path.c_str(), 1), IIS_OK);
    CHECK_EQ(iis_reader_restore_file(r, out_path.c_str(), 1), IIS_E_CONFLICT);  // 已存在，不覆盖
    iis_reader_close(r);
    FILE* fp = std::fopen(out_path.c_str(), "rb");
    std::vector<uint8_t> back(data.size() + 1);
    const size_t got = fp ? std::fread(back.data(), 1, back.size(), fp) : 0;
    if (fp) std::fclose(fp);
    CHECK(got == data.size() && std::memcmp(back.data(), data.data(), got) == 0);

    // 没有 finish 就释放：不完整文件被删除
    CHECK_EQ(iis_writer_create_file(partial.c_str(), data.size(), sha, &w), IIS_OK);
    CHECK_EQ(iis_writer_add(w, IIS_SEG_RECIPE, IIS_CODEC_NONE, 0, recipe.size(), recipe.data(), recipe.size()), IIS_OK);
    iis_writer_free(w);
    fp = std::fopen(partial.c_str(), "rb");
    CHECK(fp == nullptr);
    if (fp) std::fclose(fp);

    // 中文路径
    const std::string cn = tmp_path("中文路径.iis");
    iis_buffer c{};
    iis_pack_bytes(data.data(), 1000, 3, &c);
    CHECK_EQ(iis_writer_create_file(cn.c_str(), 1000, sha, &w), IIS_OK);
    iis_writer_free(w);
    CHECK_EQ(iis_reader_open_file("不存在的文件.iis", &r), IIS_E_NOT_FOUND);
    iis_buffer_free(&c);

    std::remove(path.c_str());
    std::remove(out_path.c_str());
}
