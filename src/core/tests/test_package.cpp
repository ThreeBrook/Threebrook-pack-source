// 包（.iisz）：打包 → 校验 → 解包逐字节一致、修改时间保留、不覆盖、ZIP64、损坏检出、删除原文件、取消、不安全的名称。
#include "testing.hpp"

#include "iis/iis.h"
#include "formats.hpp"
#include "package.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

namespace stdfs = std::filesystem;
using namespace iis;

namespace {

using Bytes = std::vector<uint8_t>;

stdfs::path P(const std::string& utf8) { return stdfs::u8path(utf8); }
std::string U(const stdfs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string fresh_dir(const std::string& name) {
    const stdfs::path d = stdfs::current_path() / "pkg_work" / P(name);
    stdfs::remove_all(d);
    stdfs::create_directories(d);
    return U(d);
}

void write_file(const std::string& path, const Bytes& b) {
    stdfs::create_directories(P(path).parent_path());
    std::ofstream f(P(path), std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

Bytes read_file(const std::string& path) {
    std::ifstream f(P(path), std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool has_temp(const std::string& dir) {
    for (const auto& e : stdfs::recursive_directory_iterator(P(dir)))
        if (U(e.path().filename()).find(".iistmp") != std::string::npos) return true;
    return false;
}

// 8 位灰度 BMP（渐变 + 噪声）
Bytes make_bmp(uint32_t w, uint32_t h, uint32_t seed) {
    std::mt19937 rng(seed);
    const uint32_t stride = (w + 3) & ~3u, off = 14 + 40 + 1024;
    Bytes b;
    auto le = [&](uint32_t v, int n) {
        for (int i = 0; i < n; ++i) b.push_back(uint8_t(v >> (8 * i)));
    };
    b.push_back('B');
    b.push_back('M');
    le(off + stride * h, 4), le(0, 4), le(off, 4), le(40, 4), le(w, 4), le(h, 4), le(1, 2), le(8, 2), le(0, 4);
    le(stride * h, 4), le(2835, 4), le(2835, 4), le(256, 4), le(0, 4);
    for (int i = 0; i < 256; ++i) le(uint32_t(i) * 0x010101u, 4);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) b.push_back(uint8_t((x + y) / 8 + rng() % 24));
        for (uint32_t x = w; x < stride; ++x) b.push_back(0);
    }
    return b;
}

Bytes make_text(size_t n) {
    std::string s;
    while (s.size() < n) s += "station=3 result=OK frame=" + std::to_string(s.size()) + "\n";
    return Bytes(s.begin(), s.begin() + long(n));
}

// 测试用文件夹：图像、子目录、文本、随机数据（压不小）、空文件、空目录、已有的 .iis、中文与空格
std::map<std::string, Bytes> make_tree(const std::string& root) {
    std::map<std::string, Bytes> m;
    m["图像/a.bmp"] = make_bmp(320, 240, 1);
    m["图像/子目录 1/b.bmp"] = make_bmp(200, 150, 2);
    m["日志.txt"] = make_text(200000);
    m["random.bin"] = testing::random_bytes(100000, 3);
    m["empty.dat"] = {};
    {
        const Bytes src = make_bmp(64, 64, 4);
        iis_buffer c{};
        iis_compress_result cr{};
        iis_compress(src.data(), src.size(), nullptr, &c, &cr);
        m["old.bmp.iis"] = Bytes(c.data, c.data + c.size);
        iis_buffer_free(&c);
    }
    for (const auto& [rel, b] : m) write_file(U(P(root) / P(rel)), b);
    stdfs::create_directories(P(root) / P("空目录"));
    return m;
}

void check_tree(const std::string& root, const std::map<std::string, Bytes>& m) {
    for (const auto& [rel, b] : m) {
        const std::string p = U(P(root) / P(rel));
        CHECK(stdfs::exists(P(p)));
        CHECK(read_file(p) == b);
    }
    CHECK(stdfs::is_directory(P(root) / P("空目录")));
}

std::string sibling(const std::string& dir, const std::string& name) { return U(P(dir).parent_path() / P(name)); }

}  // namespace

TEST(package_crc32) {
    const char* s = "123456789";
    CHECK_EQ(pkg::crc32(0, s, 9), 0xCBF43926u);
    const Bytes r = testing::random_bytes(1000, 9);
    CHECK_EQ(pkg::crc32(pkg::crc32(0, r.data(), 333), r.data() + 333, 667), pkg::crc32(0, r.data(), r.size()));
}

TEST(package_roundtrip) {
    const std::string base = fresh_dir("roundtrip");
    const std::string root = U(P(base) / P("包 测试"));
    const auto tree = make_tree(root);
    const auto stamp = stdfs::file_time_type::clock::now() - std::chrono::hours(24 * 400);
    stdfs::last_write_time(P(root) / P("图像/a.bmp"), stamp);

    uint64_t last_done = 0;
    const pkg::PackResult r = pkg::pack_folder(root, "", {}, [&](const pkg::Progress& p) { last_done = p.files_done; });
    CHECK_EQ(r.status, 0);
    CHECK(r.error.empty());
    CHECK(r.package == sibling(root, "包 测试.iisz"));
    CHECK(stdfs::exists(P(r.package)));
    CHECK_EQ(r.files, uint64_t(6));
    CHECK_EQ(r.dirs, uint64_t(4));  // 包 测试 / 图像 / 子目录 1 / 空目录
    CHECK_EQ(last_done, uint64_t(6));
    CHECK(r.as_image >= 2);
    CHECK(r.as_stored >= 2);  // random.bin、old.bmp.iis（已是容器，不再套一层）
    CHECK(r.package_bytes < r.orig_bytes);
    CHECK(!has_temp(base));
    check_tree(root, tree);  // 默认保留原文件

    pkg::Reader rd;
    CHECK_EQ(rd.open(r.package), 0);
    std::map<std::string, pkg::Entry> by;
    for (const auto& e : rd.entries()) by[e.name] = e;
    CHECK(by.count("包 测试/图像/a.bmp.iis") && by["包 测试/图像/a.bmp.iis"].iis);
    CHECK(by["包 测试/图像/a.bmp.iis"].path == "包 测试/图像/a.bmp");
    CHECK(by.count("包 测试/random.bin") && !by["包 测试/random.bin"].iis && by["包 测试/random.bin"].has_meta);
    CHECK(by.count("包 测试/old.bmp.iis") && !by["包 测试/old.bmp.iis"].iis);
    CHECK(by["包 测试/old.bmp.iis"].path == "包 测试/old.bmp.iis");
    CHECK(by.count("包 测试/空目录/") && by["包 测试/空目录/"].dir);

    const pkg::ExtractResult v = pkg::verify(r.package, true);
    CHECK_EQ(v.status, 0);
    CHECK_EQ(v.ok, uint64_t(6));
    CHECK_EQ(v.failed, uint64_t(0));

    // 解压到此处：原文件夹还在，所以解到“包 测试 (2)”，不与原文件混在一起
    const pkg::ExtractResult x = pkg::extract(r.package, {});
    CHECK_EQ(x.status, 0);
    CHECK(x.out_dir == sibling(root, "包 测试 (2)"));
    CHECK_EQ(x.ok, uint64_t(6));
    CHECK_EQ(x.failed, uint64_t(0));
    check_tree(x.out_dir, tree);
    const auto got = stdfs::last_write_time(P(x.out_dir) / P("图像/a.bmp"));
    // 修改时间保留（设的是 400 天前）；容差 2 秒：Docker 挂载的 Windows 目录上时间精度不到微秒
    CHECK(std::chrono::abs(std::chrono::duration_cast<std::chrono::milliseconds>(got - stamp)).count() < 2000);
    CHECK(!has_temp(base));

    // 再解一次到指定目录：已存在的跳过，不覆盖
    pkg::ExtractOptions eo;
    eo.here = false;
    eo.dest = base;
    const pkg::ExtractResult y = pkg::extract(r.package, eo);  // 解到 base/包 测试/…：全部已存在
    CHECK_EQ(y.skipped, uint64_t(6));
    CHECK_EQ(y.ok, uint64_t(0));
    check_tree(root, tree);

    // 再打一次：包名不覆盖
    const pkg::PackResult r2 = pkg::pack_folder(root, "", {});
    CHECK(r2.package == sibling(root, "包 测试 (2).iisz"));
}

TEST(package_zip64) {
    const std::string base = fresh_dir("zip64");
    const std::string root = U(P(base) / P("z"));
    const auto tree = make_tree(root);
    pkg::PackOptions o;
    o.force_zip64 = true;
    o.threads = 1;
    const pkg::PackResult r = pkg::pack_folder(root, "", o);
    CHECK_EQ(r.status, 0);
    const pkg::ExtractResult v = pkg::verify(r.package, true);
    CHECK_EQ(v.ok, uint64_t(6));
    CHECK_EQ(v.failed, uint64_t(0));
    pkg::ExtractOptions eo;
    eo.here = false;
    eo.dest = U(P(base) / P("out"));
    const pkg::ExtractResult x = pkg::extract(r.package, eo);
    CHECK_EQ(x.ok, uint64_t(6));
    check_tree(U(P(eo.dest) / P("z")), tree);
}

TEST(package_corrupt) {
    const std::string base = fresh_dir("corrupt");
    const std::string root = U(P(base) / P("c"));
    make_tree(root);
    const pkg::PackResult r = pkg::pack_folder(root, "", {});
    CHECK_EQ(r.status, 0);
    Bytes b = read_file(r.package);
    // 第一次出现的名称在本地头里，数据紧随其后（本地头没有附加字段）
    const std::string s = "c/图像/a.bmp.iis";
    const Bytes name(s.begin(), s.end());
    const auto it = std::search(b.begin(), b.end(), name.begin(), name.end());
    CHECK(it != b.end());
    if (it == b.end()) return;
    const size_t data = size_t(it - b.begin()) + name.size();
    b[data + 200] ^= 0x5A;
    const std::string bad = U(P(base) / P("bad.iisz"));
    write_file(bad, b);

    const pkg::ExtractResult v = pkg::verify(bad, false);
    CHECK_EQ(v.failed, uint64_t(1));
    CHECK_EQ(v.ok, uint64_t(5));
    pkg::ExtractOptions eo;
    eo.here = false;
    eo.dest = U(P(base) / P("out"));
    const pkg::ExtractResult x = pkg::extract(bad, eo);
    CHECK_EQ(x.failed, uint64_t(1));
    CHECK_EQ(x.ok, uint64_t(5));
    CHECK(!stdfs::exists(P(eo.dest) / P("c/图像/a.bmp")));  // 损坏的不留下文件
    CHECK(stdfs::exists(P(eo.dest) / P("c/图像/子目录 1/b.bmp")));
    CHECK(!has_temp(eo.dest));

    // 截断：打不开
    Bytes t = read_file(r.package);
    t.resize(t.size() - 30);
    const std::string cut = U(P(base) / P("cut.iisz"));
    write_file(cut, t);
    pkg::Reader rd;
    CHECK(rd.open(cut) != 0);
    CHECK(!rd.error().empty());
    // 不是包
    const std::string txt = U(P(base) / P("x.iisz"));
    write_file(txt, make_text(1000));
    CHECK(rd.open(txt) != 0);
}

TEST(package_delete_originals) {
    const std::string base = fresh_dir("delete");
    const std::string root = U(P(base) / P("d"));
    const auto tree = make_tree(root);
#ifdef _WIN32
    // 只读文件不删除，所以它所在的文件夹也保留
    const stdfs::path ro = P(root) / P("图像/子目录 1/b.bmp");
    stdfs::permissions(ro, stdfs::perms::owner_write | stdfs::perms::group_write | stdfs::perms::others_write, stdfs::perm_options::remove);
#endif
    pkg::PackOptions o;
    o.delete_originals = true;
    const pkg::PackResult r = pkg::pack_folder(root, "", o);
    CHECK_EQ(r.status, 0);
#ifdef _WIN32
    CHECK_EQ(r.released, uint64_t(5));
    CHECK_EQ(r.not_released, uint64_t(1));
    CHECK(stdfs::exists(ro));
    CHECK(!stdfs::exists(P(root) / P("日志.txt")));
    CHECK(!stdfs::exists(P(root) / P("空目录")));
    stdfs::permissions(ro, stdfs::perms::owner_write | stdfs::perms::group_write | stdfs::perms::others_write, stdfs::perm_options::add);
#else
    CHECK_EQ(r.released, uint64_t(6));
    CHECK(!stdfs::exists(P(root)));  // 全部删除后文件夹也删除
#endif
    const pkg::ExtractResult x = pkg::extract(r.package, {});
    CHECK_EQ(x.failed, uint64_t(0));
    check_tree(x.out_dir, tree);
}

TEST(package_cancel) {
    const std::string base = fresh_dir("cancel");
    const std::string root = U(P(base) / P("k"));
    make_tree(root);
    std::atomic<bool> stop{true};
    pkg::PackOptions o;
    o.cancel = &stop;
    o.delete_originals = true;
    const pkg::PackResult r = pkg::pack_folder(root, "", o);
    CHECK_EQ(r.status, pkg::kCancelled);
    CHECK(r.package.empty());
    CHECK(!stdfs::exists(P(base) / P("k.iisz")));
    CHECK(!has_temp(base));
    CHECK(stdfs::exists(P(root) / P("日志.txt")));
}

namespace {

// 手工拼一个只存不压的 ZIP（不经过 IIS 的写入），用于检查不安全的名称与别家的包
Bytes raw_zip(const std::vector<std::pair<std::string, Bytes>>& files, uint16_t method = 0) {
    Bytes z, cd;
    auto le = [](Bytes& b, uint64_t v, int n) {
        for (int i = 0; i < n; ++i) b.push_back(uint8_t(v >> (8 * i)));
    };
    for (const auto& [name, data] : files) {
        const uint32_t crc = pkg::crc32(0, data.data(), data.size());
        const size_t off = z.size();
        le(z, 0x04034b50, 4), le(z, 20, 2), le(z, 0x0800, 2), le(z, method, 2), le(z, 0, 4), le(z, crc, 4);
        le(z, data.size(), 4), le(z, data.size(), 4), le(z, name.size(), 2), le(z, 0, 2);
        z.insert(z.end(), name.begin(), name.end());
        z.insert(z.end(), data.begin(), data.end());
        le(cd, 0x02014b50, 4), le(cd, 63, 2), le(cd, 20, 2), le(cd, 0x0800, 2), le(cd, method, 2), le(cd, 0, 4);
        le(cd, crc, 4), le(cd, data.size(), 4), le(cd, data.size(), 4), le(cd, name.size(), 2), le(cd, 0, 2);
        le(cd, 0, 2), le(cd, 0, 2), le(cd, 0, 2), le(cd, 0x20, 4), le(cd, off, 4);
        cd.insert(cd.end(), name.begin(), name.end());
    }
    const size_t cd_off = z.size();
    z.insert(z.end(), cd.begin(), cd.end());
    le(z, 0x06054b50, 4), le(z, 0, 2), le(z, 0, 2), le(z, files.size(), 2), le(z, files.size(), 2);
    le(z, cd.size(), 4), le(z, cd_off, 4), le(z, 0, 2);
    return z;
}

}  // namespace

TEST(package_unsafe_names) {
    const std::string base = fresh_dir("unsafe");
    const std::string zip = U(P(base) / P("evil.iisz"));
    write_file(zip, raw_zip({{"../evil.txt", {'e'}},
                             {"a/../../b.txt", {'b'}},
                             {"C:/x.txt", {'c'}},
                             {"/abs.txt", {'d'}},
                             {"good/ok.txt", {'o', 'k'}}}));
    pkg::Reader rd;
    CHECK_EQ(rd.open(zip), 0);
    int bad = 0;
    for (const auto& e : rd.entries()) bad += !e.bad.empty();
    CHECK_EQ(bad, 4);
    pkg::ExtractOptions eo;
    eo.here = false;
    eo.dest = U(P(base) / P("out"));
    const pkg::ExtractResult x = pkg::extract(zip, eo);
    CHECK_EQ(x.ok, uint64_t(1));
    CHECK_EQ(x.failed, uint64_t(4));
    CHECK(read_file(U(P(eo.dest) / P("good/ok.txt"))) == (Bytes{'o', 'k'}));
    CHECK(!stdfs::exists(P(base) / P("evil.txt")));
    CHECK(!stdfs::exists(P(base) / P("b.txt")));

    // 没有 IIS 附加字段的别家 ZIP（只存不压）也能解；包内只有散文件时放进“包名\”
    const std::string plain = U(P(base) / P("plain.iisz"));
    write_file(plain, raw_zip({{"x.txt", {'1'}}, {"y.txt", {'2'}}}));
    const pkg::ExtractResult p = pkg::extract(plain, {});
    CHECK_EQ(p.ok, uint64_t(2));
    CHECK(p.out_dir == U(P(base) / P("plain")));
    CHECK(read_file(U(P(p.out_dir) / P("y.txt"))) == (Bytes{'2'}));

    // 经过压缩的 ZIP 条目：不支持，给出说明
    const std::string deflated = U(P(base) / P("deflated.iisz"));
    write_file(deflated, raw_zip({{"x.txt", {'1'}}}, 8));
    CHECK(rd.open(deflated) != 0);
    CHECK(rd.error().find("普通解压软件") != std::string::npos);
}

TEST(package_raw) {
    // 文件夹里的无文件头 RAW：没有参数文件时自动识别，有同名 .json 时按参数文件，都按图像压缩
    const std::string base = fresh_dir("raw");
    const std::string root = U(P(base) / P("raw"));
    auto smooth = [](uint32_t w, uint32_t h, uint32_t seed) {
        std::mt19937 rng(seed);
        std::vector<uint16_t> v(size_t(w) * h);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
                v[size_t(y) * w + x] = uint16_t(800 + x + 2 * y + ((x / 50 + y / 40) % 2) * 500 + rng() % 9);  // ≤ 4095
        return v;
    };
    const auto a = smooth(1279, 700, 1), b = smooth(1000, 600, 2);
    const Bytes ra = fmt::pack("Mono12p", a.data(), a.size()), rb = fmt::pack("Mono12p", b.data(), b.size());
    write_file(U(P(root) / P("a.raw")), ra);
    write_file(U(P(root) / P("b.raw")), rb);
    const std::string side = "{\"width\":1000,\"height\":600,\"pixel_format\":\"Mono12p\"}";
    write_file(U(P(root) / P("b.json")), Bytes(side.begin(), side.end()));
    const pkg::PackResult r = pkg::pack_folder(root, "", {});
    CHECK_EQ(r.status, 0);
    CHECK_EQ(r.as_image, uint64_t(2));
    CHECK(r.package_bytes < (ra.size() + rb.size()) * 7 / 10);
    pkg::ExtractOptions eo;
    eo.here = false;
    eo.dest = U(P(base) / P("out"));
    const pkg::ExtractResult x = pkg::extract(r.package, eo);
    CHECK_EQ(x.failed, uint64_t(0));
    CHECK(read_file(U(P(eo.dest) / P("raw/a.raw"))) == ra);
    CHECK(read_file(U(P(eo.dest) / P("raw/b.raw"))) == rb);
}

TEST(package_unique_path) {
    const std::string base = fresh_dir("unique");
    const std::string f = U(P(base) / P("a.bmp"));
    CHECK(pkg::unique_path(f, false) == f);
    write_file(f, {'1'});
    CHECK(pkg::unique_path(f, false) == U(P(base) / P("a (2).bmp")));
    write_file(U(P(base) / P("a (2).bmp")), {'1'});
    CHECK(pkg::unique_path(f, false) == U(P(base) / P("a (3).bmp")));
    const std::string d = U(P(base) / P("dir.x"));
    stdfs::create_directories(P(d));
    CHECK(pkg::unique_path(d, true) == U(P(base) / P("dir.x (2)")));
    CHECK(pkg::is_package_name("A.IISZ"));
    CHECK(!pkg::is_package_name("a.iis"));
}
