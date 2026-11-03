// 缩略图处理程序自检：像资源管理器那样调用 iis_thumb.dll（不需要注册、不需要管理员）。
//
//   iis_thumb_check <iis_thumb.dll> [输出.bmp]
//
// 生成一张 16 位图压成 .iis → DllGetClassObject → IInitializeWithStream（文件流）→ IThumbnailProvider::GetThumbnail(256)，
// 检查得到 256 宽的位图、中间像素不是全黑；损坏的 .iis 返回失败；
// .iisz 包：1 张图按原比例，2 张左右并排，4 张及以上拼 2×2（缝透明），第一层的图优先、不取非图像文件，
// 没有图像的包返回失败；全部释放后 DllCanUnloadNow 为 S_OK。
#include "iis/iis.h"
#include "package.hpp"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlwapi.h>
#include <thumbcache.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")

namespace {

const CLSID kClsid = {0x6C71C38C, 0x08F5, 0x40DC, {0xAD, 0x84, 0xB8, 0x64, 0x4B, 0x00, 0x8F, 0x7A}};
int fails = 0;

void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "通过" : "失败", what);
    fails += !ok;
}

// 16 位、12 位有效的 TIFF（单条带，未压缩）：渐变 + 一个亮方块
std::vector<uint8_t> tiff16(uint32_t w, uint32_t h) {
    std::vector<uint8_t> b = {'I', 'I', 42, 0, 0, 0, 0, 0};
    auto le = [&](uint32_t v, int n) {
        for (int i = 0; i < n; ++i) b.push_back(uint8_t(v >> (8 * i)));
    };
    const uint32_t data = 8;
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) le((x > w / 3 && x < w / 2 && y > h / 3 && y < h / 2) ? 4000 : 500 + x * 3 + y, 2);
    const uint32_t ifd = uint32_t(b.size());
    for (int k = 0; k < 4; ++k) b[4 + k] = uint8_t(ifd >> (8 * k));
    const uint32_t tags[][4] = {{256, 4, 1, w}, {257, 4, 1, h}, {258, 3, 1, 16}, {259, 3, 1, 1}, {262, 3, 1, 1},
                                {273, 4, 1, data}, {277, 3, 1, 1}, {278, 4, 1, h}, {279, 4, 1, w * h * 2}};
    le(9, 2);
    for (const auto& t : tags) {
        le(t[0], 2), le(t[1], 2), le(t[2], 4);
        if (t[1] == 3) le(t[3], 2), le(0, 2);
        else le(t[3], 4);
    }
    le(0, 4);
    return b;
}

void write(const std::filesystem::path& p, const std::vector<uint8_t>& b) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

// 像资源管理器那样取缩略图；返回 HRESULT，成功时给出位图
HRESULT thumb(IClassFactory* cf, const std::filesystem::path& file, UINT cx, HBITMAP& bmp) {
    bmp = nullptr;
    IInitializeWithStream* init = nullptr;
    HRESULT hr = cf->CreateInstance(nullptr, IID_PPV_ARGS(&init));
    if (FAILED(hr)) return hr;
    IStream* s = nullptr;
    hr = SHCreateStreamOnFileEx(file.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE, 0, FALSE, nullptr, &s);
    if (SUCCEEDED(hr)) hr = init->Initialize(s, STGM_READ);
    IThumbnailProvider* tp = nullptr;
    if (SUCCEEDED(hr)) hr = init->QueryInterface(IID_PPV_ARGS(&tp));
    WTS_ALPHATYPE a{};
    if (SUCCEEDED(hr)) hr = tp->GetThumbnail(cx, &bmp, &a);
    if (tp) tp->Release();
    if (s) s->Release();
    init->Release();
    return hr;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 2) return std::printf("用法：iis_thumb_check <iis_thumb.dll> [输出.bmp]\n"), 2;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    namespace fs = std::filesystem;
    const fs::path dir = fs::current_path() / L"thumb_check";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);

    const std::vector<uint8_t> img = tiff16(640, 400);
    iis_buffer c{};
    iis_compress_result r{};
    check(iis_compress(img.data(), img.size(), nullptr, &c, &r) == IIS_OK && r.method == IIS_METHOD_IMAGE, "生成 .iis（16 位 TIFF，按图像压缩）");
    std::vector<uint8_t> iis(c.data, c.data + c.size);
    iis_buffer_free(&c);
    write(dir / L"测试 图.tif.iis", iis);
    std::vector<uint8_t> bad = iis;
    bad[bad.size() / 2] ^= 0xFF;
    write(dir / L"坏.tif.iis", bad);

    HMODULE dll = LoadLibraryW(argv[1]);
    check(dll != nullptr, "加载 iis_thumb.dll");
    if (!dll) return 1;
    using GetClass = HRESULT(__stdcall*)(REFCLSID, REFIID, void**);
    using CanUnload = HRESULT(__stdcall*)();
    auto get = reinterpret_cast<GetClass>(GetProcAddress(dll, "DllGetClassObject"));
    auto can_unload = reinterpret_cast<CanUnload>(GetProcAddress(dll, "DllCanUnloadNow"));
    check(get && can_unload, "导出 DllGetClassObject、DllCanUnloadNow");
    if (!get || !can_unload) return 1;
    IClassFactory* cf = nullptr;
    check(SUCCEEDED(get(kClsid, IID_PPV_ARGS(&cf))) && cf, "取得类工厂");
    void* other = nullptr;
    check(get(IID_IUnknown, IID_IClassFactory, &other) == CLASS_E_CLASSNOTAVAILABLE, "别的 CLSID 拒绝");

    HBITMAP bmp = nullptr;
    HRESULT hr = thumb(cf, dir / L"测试 图.tif.iis", 256, bmp);
    BITMAP bm{};
    const bool got = SUCCEEDED(hr) && bmp && GetObjectW(bmp, sizeof bm, &bm);
    check(got && bm.bmWidth == 256 && bm.bmHeight == 160 && bm.bmBitsPixel == 32, "缩略图 256×160、32 位");
    if (got) {
        const auto* px = static_cast<const uint8_t*>(bm.bmBits);
        const uint8_t* mid = px + (size_t(64) * 256 + 106) * 4;  // 亮方块里
        const uint8_t* bg = px + (size_t(150) * 256 + 10) * 4;   // 暗处
        check(mid[0] > 200 && mid[0] == mid[1] && mid[1] == mid[2] && bg[0] < mid[0], "内容正确（亮方块比背景亮，灰度）");
        if (argc > 2) {  // 存成 BMP 以便查看
            std::vector<uint8_t> f(54 + size_t(bm.bmWidth) * bm.bmHeight * 4, 0);
            auto le = [&](size_t at, uint32_t v, int n) {
                for (int i = 0; i < n; ++i) f[at + i] = uint8_t(v >> (8 * i));
            };
            f[0] = 'B', f[1] = 'M';
            le(2, uint32_t(f.size()), 4), le(10, 54, 4), le(14, 40, 4), le(18, uint32_t(bm.bmWidth), 4);
            le(22, uint32_t(bm.bmHeight), 4), le(26, 1, 2), le(28, 32, 2);
            for (int y = 0; y < bm.bmHeight; ++y)  // BMP 自下而上
                memcpy(&f[54 + size_t(bm.bmHeight - 1 - y) * bm.bmWidth * 4], px + size_t(y) * bm.bmWidthBytes, size_t(bm.bmWidth) * 4);
            write(argv[2], f);
        }
        DeleteObject(bmp);
    }
    hr = thumb(cf, dir / L"测试 图.tif.iis", 96, bmp);
    check(SUCCEEDED(hr) && bmp && GetObjectW(bmp, sizeof bm, &bm) && bm.bmWidth == 96, "小尺寸 96");
    if (bmp) DeleteObject(bmp);
    hr = thumb(cf, dir / L"坏.tif.iis", 256, bmp);
    check(FAILED(hr) && !bmp, "损坏的 .iis：返回失败（资源管理器改显示图标）");

    // .iisz 包。alpha(x, y)：位图该点的不透明度
    iis::pkg::PackOptions po;
    auto alpha = [&](int x, int y) { return static_cast<const uint8_t*>(bm.bmBits)[(size_t(y) * bm.bmWidth + x) * 4 + 3]; };
    auto pack = [&](const wchar_t* name, const std::vector<const wchar_t*>& tifs) {
        const fs::path f = dir / name;
        for (const wchar_t* t : tifs) {
            fs::create_directories((f / t).parent_path());
            write(f / t, img);
        }
        write(f / L"a 说明.txt", std::vector<uint8_t>(3000, 'x'));  // 非图像，不进预览
        const auto r = iis::pkg::pack_folder(f.u8string(), "", po);
        return r.status == IIS_OK && r.files == tifs.size() + 1 ? fs::u8path(r.package) : fs::path();
    };
    const fs::path one = pack(L"一张", {L"a.tif"});
    hr = thumb(cf, one, 256, bmp);
    check(!one.empty() && SUCCEEDED(hr) && bmp && GetObjectW(bmp, sizeof bm, &bm) && bm.bmWidth == 256 && bm.bmHeight == 160 &&
              alpha(128, 80) == 255,
          ".iisz 包，1 张图：按原比例 256×160");
    if (bmp) DeleteObject(bmp);
    const fs::path two = pack(L"两张", {L"子/a.tif", L"b.tif"});
    hr = thumb(cf, two, 256, bmp);
    check(!two.empty() && SUCCEEDED(hr) && bmp && GetObjectW(bmp, sizeof bm, &bm) && bm.bmWidth == 256 && bm.bmHeight == 256 &&
              alpha(128, 128) == 0 && alpha(60, 128) == 255 && alpha(200, 128) == 255,
          ".iisz 包，2 张图（一张在子文件夹）：左右并排 256×256，中缝透明");
    if (bmp) DeleteObject(bmp);
    const fs::path five = pack(L"五张", {L"1.tif", L"2.tif", L"3.tif", L"4.tif", L"子/5.tif"});
    hr = thumb(cf, five, 256, bmp);
    check(!five.empty() && SUCCEEDED(hr) && bmp && GetObjectW(bmp, sizeof bm, &bm) && bm.bmWidth == 256 && bm.bmHeight == 256 &&
              alpha(128, 60) == 0 && alpha(60, 128) == 0 && alpha(60, 60) == 255 && alpha(200, 60) == 255 &&
              alpha(60, 200) == 255 && alpha(200, 200) == 255,
          ".iisz 包，5 张图：取 4 张拼 2×2，十字缝透明");
    if (bmp) DeleteObject(bmp);
    const fs::path txt = dir / L"文档";
    fs::create_directories(txt);
    write(txt / L"a.txt", std::vector<uint8_t>(3000, 'y'));
    const auto pt = iis::pkg::pack_folder(txt.u8string(), "", po);
    hr = thumb(cf, fs::u8path(pt.package), 256, bmp);
    check(pt.status == IIS_OK && FAILED(hr) && !bmp, "没有图像的 .iisz 包：返回失败（显示包图标）");
    if (bmp) DeleteObject(bmp);
    cf->Release();
    check(can_unload() == S_OK, "全部释放后可以卸载");
    FreeLibrary(dll);
    fs::remove_all(dir, ec);
    CoUninitialize();
    std::printf(fails ? "失败 %d 项\n" : "全部通过\n", fails);
    return fails ? 1 : 0;
}
