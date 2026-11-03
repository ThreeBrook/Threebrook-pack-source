// 查看器图像读取与显示准备的测试：BMP（灰度 / 彩色调色板 / 24 位 / 自下而上）、TIFF（16 位、多页、LZW）、
// RAW（需要参数、打包格式、Bayer）、.iis（还原并核对）、窗宽窗位、去马赛克、缩略图。
#include "testing.hpp"

#include "i18n.hpp"
#include "image.hpp"
#include "iis/iis.h"
#include "package.hpp"
#include "tasks.hpp"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using namespace iis::view;
using Bytes = std::vector<uint8_t>;

namespace {

void le(Bytes& b, uint32_t v, int n) {
    for (int i = 0; i < n; ++i) b.push_back(uint8_t(v >> (8 * i)));
}

// 8 位（调色板）或 24 位 BMP；像素值 = f(x, y)
Bytes bmp(uint32_t w, uint32_t h, int ch, bool top_down, bool color_palette = false) {
    const uint32_t stride = (w * ch + 3) & ~3u, pal = ch == 1 ? 1024 : 0, off = 54 + pal;
    Bytes b;
    b.push_back('B');
    b.push_back('M');
    le(b, off + stride * h, 4), le(b, 0, 4), le(b, off, 4), le(b, 40, 4), le(b, w, 4);
    le(b, top_down ? uint32_t(-int32_t(h)) : h, 4), le(b, 1, 2), le(b, uint32_t(ch * 8), 2), le(b, 0, 4);
    le(b, stride * h, 4), le(b, 2835, 4), le(b, 2835, 4), le(b, ch == 1 ? 256 : 0, 4), le(b, 0, 4);
    for (uint32_t i = 0; ch == 1 && i < 256; ++i) le(b, color_palette ? (i << 16) : i * 0x010101u, 4);  // 彩色：只有红
    for (uint32_t r = 0; r < h; ++r) {
        const uint32_t y = top_down ? r : h - 1 - r;
        for (uint32_t x = 0; x < w; ++x)
            for (int c = 0; c < ch; ++c) b.push_back(uint8_t(x * 3 + y * 5 + c * 50));
        for (uint32_t x = w * ch; x < stride; ++x) b.push_back(0);
    }
    return b;
}

// 未压缩 16 位单通道 TIFF（II），pages 页，像素 = x*100 + y*7 + 页号*1000
Bytes tiff16(uint32_t w, uint32_t h, int pages) {
    Bytes b = {'I', 'I', 42, 0, 0, 0, 0, 0};
    std::vector<uint32_t> ifd_pos;
    uint32_t prev_next = 4;
    for (int p = 0; p < pages; ++p) {
        const uint32_t data = uint32_t(b.size());
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) le(b, x * 100 + y * 7 + uint32_t(p) * 1000, 2);
        if (b.size() % 2) b.push_back(0);
        const uint32_t ifd = uint32_t(b.size());
        for (int k = 0; k < 4; ++k) b[prev_next + k] = uint8_t(ifd >> (8 * k));
        const uint32_t tags[][4] = {{256, 4, 1, w}, {257, 4, 1, h}, {258, 3, 1, 16}, {259, 3, 1, 1}, {262, 3, 1, 1},
                                    {273, 4, 1, data}, {277, 3, 1, 1}, {278, 4, 1, h}, {279, 4, 1, w * h * 2}};
        le(b, 9, 2);
        for (const auto& t : tags) {
            le(b, t[0], 2), le(b, t[1], 2), le(b, t[2], 4);
            if (t[1] == 3) le(b, t[3], 2), le(b, 0, 2);
            else le(b, t[3], 4);
        }
        prev_next = uint32_t(b.size());
        le(b, 0, 4);
    }
    return b;
}

Bytes compress(const Bytes& d) {
    iis_buffer c{};
    iis_compress_result r{};
    CHECK_EQ(iis_compress(d.data(), d.size(), nullptr, &c, &r), IIS_OK);
    Bytes out(c.data, c.data + c.size);
    iis_buffer_free(&c);
    return out;
}

}  // namespace

TEST(viewer_bmp) {
    const Bytes g = bmp(31, 17, 1, false);
    Loaded l = load_bytes(g, "a.bmp");
    CHECK(l.ok && l.pages.size() == 1);
    const Image& im = l.pages[0];
    CHECK(im.width == 31 && im.height == 17 && im.channels == 1);
    CHECK_EQ(int(im.px[size_t(5) * 31 + 7]), (7 * 3 + 5 * 5) & 0xFF);  // 自下而上存储也按自上而下显示
    CHECK(!l.from_iis && l.sha256.size() == 64);
    // 24 位：文件里 B G R，显示为 R G B
    const Loaded c = load_bytes(bmp(9, 4, 3, true), "c.bmp");
    CHECK(c.ok && c.pages[0].channels == 3);
    const uint16_t* p = &c.pages[0].px[(size_t(2) * 9 + 3) * 3];
    CHECK(p[0] == ((3 * 3 + 2 * 5 + 100) & 0xFF) && p[2] == ((3 * 3 + 2 * 5) & 0xFF));
    // 彩色调色板：转 RGB
    const Loaded cp = load_bytes(bmp(8, 8, 1, true, true), "p.bmp");
    CHECK(cp.ok && cp.pages[0].channels == 3);
}

TEST(viewer_tiff_and_iis) {
    const Bytes t = tiff16(40, 30, 3);
    Loaded l = load_bytes(t, "x.tif");
    CHECK(l.ok);
    CHECK_EQ(l.pages.size(), size_t(3));
    CHECK(l.pages[2].bits == 16 && l.pages[2].px[size_t(4) * 40 + 9] == 9 * 100 + 4 * 7 + 2000);
    // .iis：还原后显示，SHA-256 为原文件的
    const Bytes c = compress(t);
    const Loaded li = load_bytes(c, "x.tif.iis");
    CHECK(li.ok && li.from_iis && li.pages.size() == 3);
    CHECK(li.original == t);
    CHECK(li.sha256 == l.sha256);
    CHECK(li.kind.find("TIFF") != std::string::npos);
    // 损坏的 .iis：给出原因，不崩溃
    Bytes bad = c;
    bad[bad.size() / 2] ^= 0xFF;
    const Loaded lb = load_bytes(bad, "x.tif.iis");
    CHECK(!lb.ok && !lb.error.empty());
}

TEST(viewer_raw_and_bayer) {
    // Mono12p 打包、无文件头：没有参数时提示输入，给了参数后显示
    const uint32_t w = 32, h = 20;
    std::vector<uint16_t> v(size_t(w) * h);
    for (size_t i = 0; i < v.size(); ++i) v[i] = uint16_t((i * 37) & 0xFFF);
    Bytes raw((v.size() * 12 + 7) / 8, 0);
    for (size_t i = 0; i < v.size(); ++i) {
        const uint64_t bit = i * 12;
        const uint32_t x = uint32_t(v[i]) << (bit % 8);
        for (int k = 0; k < 3; ++k)
            if (bit / 8 + k < raw.size()) raw[size_t(bit / 8 + k)] |= uint8_t(x >> (8 * k));
    }
    // 看不出图像结构的数据：识别不出来，仍请用户给参数
    Loaded l = load_bytes(testing::random_bytes(raw.size(), 5), "a.raw");
    CHECK(!l.ok && l.need_raw_params);
    // 平滑的无文件头 Mono12p：自动识别宽高与格式，直接显示，并标明是自动识别的
    {
        const uint32_t aw = 256, ah = 96;
        std::vector<uint16_t> s(size_t(aw) * ah);
        for (uint32_t y = 0; y < ah; ++y)
            for (uint32_t x = 0; x < aw; ++x) s[size_t(y) * aw + x] = uint16_t(1000 + 6 * x + 5 * y + ((x / 40 + y / 30) % 2) * 600);  // ≤ 4095
        Bytes packed((s.size() * 12 + 7) / 8, 0);
        for (size_t i = 0; i < s.size(); ++i) {
            const uint64_t bit = i * 12;
            const uint32_t x = uint32_t(s[i]) << (bit % 8);
            for (int k = 0; k < 3; ++k)
                if (bit / 8 + k < packed.size()) packed[size_t(bit / 8 + k)] |= uint8_t(x >> (8 * k));
        }
        const Loaded a = load_bytes(packed, "auto.raw");
        CHECK(a.ok && a.raw_auto);
        CHECK(a.ok && a.pages[0].width == aw && a.pages[0].height == ah && a.pages[0].px == s);
    }
    RawParams rp{w, h, "Mono12p", 0, 0};
    l = load_bytes(raw, "a.raw", &rp);
    CHECK(l.ok && l.pages[0].bits == 12);
    CHECK(l.pages[0].px == v);
    // Bayer RGGB：红像素亮、其他暗 → 去马赛克后整体偏红
    Bytes b8(size_t(w) * h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) b8[size_t(y) * w + x] = (y % 2 == 0 && x % 2 == 0) ? 200 : 10;
    RawParams rb{w, h, "BayerRG8", 0, 0};
    l = load_bytes(b8, "b.raw", &rb);
    CHECK(l.ok && l.pages[0].bayer == "RGGB");
    std::vector<uint8_t> rgba;
    to_rgba(l.pages[0], {0, 255}, true, l.pages[0].bayer, rgba);
    const uint8_t* px = &rgba[(size_t(5) * w + 6) * 4];  // 内部像素
    CHECK(px[0] > 180 && px[1] < 30 && px[2] < 30);
    // 白平衡：R 偏高的图平衡后三色接近
    Bytes flat(size_t(w) * h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) flat[size_t(y) * w + x] = (y % 2 == 0 && x % 2 == 0) ? 160 : 80;
    const Loaded lf = load_bytes(flat, "f.raw", &rb);
    to_rgba(lf.pages[0], {0, 255}, true, "RGGB", rgba, true);
    px = &rgba[(size_t(5) * w + 6) * 4];
    CHECK(std::abs(int(px[0]) - int(px[1])) < 6 && std::abs(int(px[2]) - int(px[1])) < 6);
    to_rgba(l.pages[0], {0, 255}, false, "", rgba);  // 不去马赛克：灰度
    px = &rgba[(size_t(5) * w + 6) * 4];
    CHECK(px[0] == px[1] && px[1] == px[2]);
    // 参数不对：报错
    RawParams wrong{w, h, "Mono99", 0, 0};
    CHECK(!load_bytes(raw, "a.raw", &wrong).ok);
}

TEST(viewer_window_and_thumbnail) {
    Image im;
    im.width = 100;
    im.height = 100;
    im.bits = 16;
    im.px.resize(10000);
    for (size_t i = 0; i < im.px.size(); ++i) im.px[i] = uint16_t(1000 + i % 1000);
    im.px[0] = 65535;  // 个别极值不影响自动窗
    const Window w = auto_window(im);
    CHECK(w.lo >= 1000 && w.lo < 1010 && w.hi > 1990 && w.hi <= 1999);
    std::vector<uint8_t> rgba;
    to_rgba(im, w, false, "", rgba);
    CHECK_EQ(rgba.size(), size_t(40000));
    CHECK(rgba[0] == 255 && rgba[3] == 255);
    int tw = 0, th = 0;
    im.width = 400;
    im.height = 100;
    im.px.assign(40000, 500);
    thumbnail(im, 128, rgba, tw, th);
    CHECK(tw == 128 && th == 32 && rgba.size() == size_t(128 * 32 * 4));
    CHECK(listable("a.BMP") && listable("b.tif.iis") && listable("c.raw") && !listable("d.txt"));
}

// 批量还原（资源管理器右键 / 开始菜单“批量还原”的核心）：目录（含子目录）与单个文件；中文与空格；已存在不覆盖；
// 损坏的 .iis 报错且不留文件；.iis 保留；判断输出目录是否在服务的监控目录里
TEST(viewer_restore_tree) {
    namespace fs = std::filesystem;
    const fs::path d = fs::current_path() / L"viewer_restore_test";
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d / L"源 目录" / L"工位1");
    auto put = [](const fs::path& p, const Bytes& b) {
        std::ofstream f(p, std::ios::binary);
        f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
    };
    auto get = [](const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    };
    const Bytes a = bmp(64, 48, 1, false), b = tiff16(32, 20, 2);
    const fs::path src = d / L"源 目录";
    put(src / L"a.bmp.iis", compress(a));
    put(src / L"工位1" / L"b 图.tif.iis", compress(b));
    Bytes bad = compress(a);
    bad[bad.size() / 2] ^= 0xFF;
    put(src / L"工位1" / L"坏.bmp.iis", bad);
    put(src / L"其他.txt", Bytes{1, 2, 3});
    const auto u8 = [](const fs::path& p) { const auto s = p.u8string(); return std::string(s.begin(), s.end()); };
    int calls = 0;
    RestoreResult r = restore_tree(u8(src), u8(d / L"输出"), true, [&](const RestoreResult&) { ++calls; });
    CHECK_EQ(r.total, 3);
    CHECK_EQ(r.ok, 2);
    CHECK_EQ(r.failed, 1);
    CHECK_EQ(r.errors.size(), size_t(1));
    CHECK_EQ(calls, 4);
    CHECK(get(d / L"输出" / L"a.bmp") == a);
    CHECK(get(d / L"输出" / L"工位1" / L"b 图.tif") == b);
    CHECK(!fs::exists(d / L"输出" / L"工位1" / L"坏.bmp"));
    CHECK(fs::exists(src / L"a.bmp.iis"));  // .iis 保留
    // 再来一次：已存在的不覆盖
    r = restore_tree(u8(src), u8(d / L"输出"), true);
    CHECK(r.skipped == 2 && r.ok == 0);
    // 不含子目录
    r = restore_tree(u8(src), u8(d / L"输出2"), false);
    CHECK(r.total == 1 && r.ok == 1);
    // 单个文件
    r = restore_tree(u8(src / L"工位1" / L"b 图.tif.iis"), u8(d / L"单个"), true);
    CHECK(r.total == 1 && r.ok == 1 && get(d / L"单个" / L"b 图.tif") == b);
    // 监控目录判断（用指定的配置文件）
    const fs::path cfg = d / L"config.json";
    const std::string text = "{\"watch\": [{\"path\": \"" + std::string("D:\\\\图像") + "\"}, {\"path\": \"相对\"}]}";
    put(cfg, Bytes(text.begin(), text.end()));
    CHECK(watched_root("D:\\图像\\工位1", u8(cfg)) == "D:\\图像");
    CHECK(watched_root("d:\\图像", u8(cfg)) == "D:\\图像");
    CHECK(watched_root("D:\\图像2", u8(cfg)).empty());
    CHECK(!watched_root(u8(d / L"相对" / L"x"), u8(cfg)).empty());
    CHECK(watched_root("E:\\x", u8(d / L"没有.json")).empty());
    fs::remove_all(d, ec);
}

namespace {

// 最小的 PNG（8 位 RGB，zlib 用不压缩的块），用来检查 WIC 预览
Bytes png_rgb(uint32_t w, uint32_t h) {
    Bytes raw;
    for (uint32_t y = 0; y < h; ++y) {
        raw.push_back(0);  // 行过滤：无
        for (uint32_t x = 0; x < w; ++x) raw.push_back(uint8_t(x * 40)), raw.push_back(uint8_t(y * 60)), raw.push_back(200);
    }
    Bytes z = {0x78, 0x01, 1, uint8_t(raw.size()), uint8_t(raw.size() >> 8), uint8_t(~raw.size()), uint8_t(~raw.size() >> 8)};
    z.insert(z.end(), raw.begin(), raw.end());
    uint32_t a = 1, b = 0;
    for (const uint8_t c : raw) a = (a + c) % 65521, b = (b + a) % 65521;
    const uint32_t adler = b << 16 | a;
    for (int i = 3; i >= 0; --i) z.push_back(uint8_t(adler >> (8 * i)));
    Bytes out = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    auto chunk = [&](const char* type, const Bytes& d) {
        for (int i = 3; i >= 0; --i) out.push_back(uint8_t(d.size() >> (8 * i)));
        Bytes td(type, type + 4);
        td.insert(td.end(), d.begin(), d.end());
        out.insert(out.end(), td.begin(), td.end());
        const uint32_t crc = iis::pkg::crc32(0, td.data(), td.size());
        for (int i = 3; i >= 0; --i) out.push_back(uint8_t(crc >> (8 * i)));
    };
    Bytes ihdr;
    for (const uint32_t v : {w, h})
        for (int i = 3; i >= 0; --i) ihdr.push_back(uint8_t(v >> (8 * i)));
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    return out;
}

}  // namespace

TEST(viewer_tasks) {
    namespace fs = std::filesystem;
    const fs::path d = fs::current_path() / L"viewer_task_test";
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d / L"图 A" / L"子 目录");
    auto put = [](const fs::path& p, const Bytes& b) {
        std::ofstream f(p, std::ios::binary);
        f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
    };
    auto get = [](const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    };
    const auto u8 = [](const fs::path& p) { const auto s = p.u8string(); return std::string(s.begin(), s.end()); };
    const Bytes a = bmp(64, 48, 1, false), b = tiff16(32, 20, 2), c = png_rgb(5, 3), single = bmp(40, 30, 3, true);
    put(d / L"图 A" / L"a.bmp", a);
    put(d / L"图 A" / L"子 目录" / L"b.tif", b);
    put(d / L"图 A" / L"c.png", c);
    put(d / L"单张.bmp", single);

    CHECK(compress_preview(u8(d / L"图 A")) == "[文件夹] 图 A → 图 A.iisz");
    CHECK(compress_preview(u8(d / L"单张.bmp")) == "[文件] 单张.bmp → 单张.bmp.iis");
    CHECK(compress_preview(u8(d / L"x.iis")).rfind("[跳过]", 0) == 0);
    // 英文界面
    iis::i18n::set_lang(iis::i18n::Lang::En);
    CHECK(compress_preview(u8(d / L"图 A")) == "[Folder] 图 A → 图 A.iisz");
    CHECK(compress_preview(u8(d / L"x.iis")) == "[Skip] x.iis (already compressed)");
    CHECK(iis::i18n::trf("共 %d 个", "%d in total", 3) == "3 in total");
    iis::i18n::set_lang(iis::i18n::Lang::Zh);
    CHECK(iis::i18n::trf("共 %d 个", "%d in total", 3) == "共 3 个");

    // 压缩：文件夹 → 一个包，单个文件 → .iis；原文件保留
    std::atomic<bool> cancel{false};
    TaskState st;
    run_compress({u8(d / L"图 A") + "\\", u8(d / L"单张.bmp")}, false, cancel, st);
    CHECK_EQ(st.failed, 0);
    CHECK_EQ(st.lines.size(), size_t(2));
    CHECK(st.problems.empty());
    CHECK(fs::exists(d / L"图 A.iisz"));
    CHECK(fs::exists(d / L"单张.bmp.iis"));
    CHECK(get(d / L"图 A" / L"a.bmp") == a);
    CHECK(st.out_bytes > 0 && st.out_bytes < st.in_bytes);
    CHECK(st.open_dir == u8(d));

    // 包内文件：取出并预览（PNG 走 Windows 自带解码器）
    const auto pk = open_package(u8(d / L"图 A.iisz"));
    CHECK(pk != nullptr);
    if (pk) {
        size_t png_index = SIZE_MAX;
        for (size_t i = 0; i < pk->entries().size(); ++i)
            if (pk->entries()[i].path == "图 A/c.png") png_index = i;
        CHECK(png_index != SIZE_MAX);
        const Loaded l = load_any(package_item(u8(d / L"图 A.iisz"), png_index));
        CHECK(l.ok);
        CHECK(l.in_package);
        CHECK(l.name == "c.png");
        CHECK(!l.pages.empty() && l.pages[0].width == 5 && l.pages[0].height == 3);
        CHECK(l.original == c);
    }

    // 解压：包 → “图 A (2)”（原文件夹还在）；单个 .iis 的原文件还在 → 不覆盖
    TaskState x;
    run_extract({u8(d / L"图 A.iisz"), u8(d / L"单张.bmp.iis")}, cancel, x);
    CHECK_EQ(x.failed, 0);
    CHECK(get(d / L"图 A (2)" / L"a.bmp") == a);
    CHECK(get(d / L"图 A (2)" / L"子 目录" / L"b.tif") == b);
    CHECK(get(d / L"图 A (2)" / L"c.png") == c);
    CHECK(x.open_dir == u8(d));  // 最后一项（单个 .iis）所在的文件夹
    CHECK(x.lines.size() == 2 && x.lines[1].find("已存在") != std::string::npos);

    // 解压文件夹：还原其中的 .iis、解开其中的包
    fs::remove(d / L"单张.bmp");
    fs::create_directories(d / L"再来");
    fs::copy_file(d / L"单张.bmp.iis", d / L"再来" / L"单张.bmp.iis");
    fs::copy_file(d / L"图 A.iisz", d / L"再来" / L"图 A.iisz");
    TaskState y;
    run_extract({u8(d / L"再来")}, cancel, y);
    CHECK_EQ(y.failed, 0);
    CHECK(get(d / L"再来" / L"单张.bmp") == single);
    CHECK(get(d / L"再来" / L"图 A" / L"a.bmp") == a);

    // 删除原文件
    TaskState z;
    run_compress({u8(d / L"图 A")}, true, cancel, z);
    CHECK_EQ(z.failed, 0);
    CHECK(!fs::exists(d / L"图 A"));
    CHECK(fs::exists(d / L"图 A (2).iisz"));

    // 取消：不生成包
    cancel = true;
    TaskState k;
    run_compress({u8(d / L"图 A (2)")}, false, cancel, k);
    CHECK(k.cancelled);
    CHECK(!fs::exists(d / L"图 A (2) (2).iisz"));
    fs::remove_all(d, ec);
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    iis::i18n::set_lang(iis::i18n::Lang::Zh);  // 断言按中文写；英文另有专门的检查
    const std::string filter = argc > 1 ? argv[1] : "";
    std::printf("%s\n", iis_build_info());
    for (const auto& c : testing::registry()) {
        if (std::string(c.name).find(filter) == std::string::npos) continue;
        std::printf("运行 %s\n", c.name);
        const int before = testing::failures();
        c.fn();
        std::printf("%s %s\n", testing::failures() == before ? "通过" : "失败", c.name);
    }
    std::printf("共 %zu 项，失败断言 %d 个\n", testing::registry().size(), testing::failures());
    return testing::failures() ? 1 : 0;
}
