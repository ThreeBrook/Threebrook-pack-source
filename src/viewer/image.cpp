#include "image.hpp"

#include "i18n.hpp"

#include "iis/iis.h"

#include "detect.hpp"
#include "formats.hpp"
#include "package.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>

#include <nlohmann/json.hpp>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincodec.h>
#undef small  // rpcndr.h 把 small 定义成了 char

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace iis::view {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = char(tolower(uint8_t(c)));
    return s;
}

std::string hex(const uint8_t* d) {
    static const char* x = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 32; ++i) s += x[d[i] >> 4], s += x[d[i] & 15];
    return s;
}

bool valid_cfa(const std::string& p) {
    return p == "RGGB" || p == "GRBG" || p == "GBRG" || p == "BGGR";
}

// BMP：按识别出的头部信息直接读像素（8 位走调色板，24 / 32 位为 B G R (A)）
bool bmp_pixels(const std::vector<uint8_t>& d, const fmt::BmpInfo& b, Image& img) {
    if (b.damaged() || !b.width || !b.height) return false;
    const bool gray = b.channels == 1 && b.gray_palette();
    img.width = b.width;
    img.height = b.height;
    img.bits = 8;
    img.channels = b.channels == 1 ? (gray ? 1 : 3) : (b.channels == 4 ? 4 : 3);
    img.px.assign(size_t(img.width) * img.height * img.channels, 0);
    for (uint32_t y = 0; y < b.height; ++y) {
        const uint8_t* row = &d[size_t(b.pixel_offset + (b.top_down ? y : b.height - 1 - y) * b.stride)];
        uint16_t* o = &img.px[size_t(y) * img.width * img.channels];
        for (uint32_t x = 0; x < b.width; ++x) {
            if (b.channels == 1) {
                const uint8_t i = row[x];
                if (gray) {
                    o[x] = i;
                } else {  // 彩色调色板
                    const size_t k = size_t(i) * 4;
                    const bool in = k + 2 < b.palette.size();
                    o[x * 3 + 0] = in ? b.palette[k + 2] : i;
                    o[x * 3 + 1] = in ? b.palette[k + 1] : i;
                    o[x * 3 + 2] = in ? b.palette[k + 0] : i;
                }
            } else {
                const uint8_t* p = row + size_t(x) * b.channels;
                uint16_t* q = o + size_t(x) * img.channels;
                q[0] = p[2], q[1] = p[1], q[2] = p[0];
                if (img.channels == 4) q[3] = p[3];
            }
        }
    }
    return true;
}

// 其他图像格式（PNG、JPEG、GIF…）：用 Windows 自带的 WIC 解码为 8 位 RGBA，只供预览
bool wic_decode(const std::vector<uint8_t>& d, Image& img, std::string& format) {
    if (d.empty() || d.size() >= 0xFFFFFFFFull) return false;
    const HRESULT hi = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* f = nullptr;
    IWICStream* s = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICFormatConverter* cv = nullptr;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
        SUCCEEDED(f->CreateStream(&s)) &&
        SUCCEEDED(s->InitializeFromMemory(const_cast<BYTE*>(d.data()), DWORD(d.size()))) &&
        SUCCEEDED(f->CreateDecoderFromStream(s, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(f->CreateFormatConverter(&cv)) &&
        SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                 WICBitmapPaletteTypeCustom))) {
        UINT w = 0, h = 0;
        cv->GetSize(&w, &h);
        if (w && h && uint64_t(w) * h <= (1ull << 28)) {
            std::vector<uint8_t> px(size_t(w) * h * 4);
            if (SUCCEEDED(cv->CopyPixels(nullptr, w * 4, UINT(px.size()), px.data()))) {
                img.width = w;
                img.height = h;
                img.channels = 4;
                img.bits = 8;
                img.px.assign(px.begin(), px.end());
                GUID g{};
                dec->GetContainerFormat(&g);
                format = g == GUID_ContainerFormatPng    ? "PNG"
                         : g == GUID_ContainerFormatJpeg ? "JPEG"
                         : g == GUID_ContainerFormatGif  ? "GIF"
                         : g == GUID_ContainerFormatTiff ? "TIFF"
                         : g == GUID_ContainerFormatBmp  ? "BMP"
                                                         : i18n::tr("图像", "Image");
                ok = true;
            }
        }
    }
    for (IUnknown* u : std::initializer_list<IUnknown*>{cv, fr, dec, s, f})
        if (u) u->Release();
    if (SUCCEEDED(hi)) CoUninitialize();
    return ok;
}

}  // namespace

bool listable(const std::string& name) {
    const std::string n = lower(name);
    for (const char* e : {".iis", ".iisz", ".bmp", ".dib", ".tif", ".tiff", ".raw", ".bin", ".dat", ".png", ".jpg", ".jpeg"})
        if (n.size() >= std::strlen(e) && n.compare(n.size() - std::strlen(e), std::strlen(e), e) == 0) return true;
    return false;
}

Loaded load_file(const std::string& path, const RawParams* raw) {
    Loaded r;
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f) {
        r.error = i18n::tr("无法打开文件", "Cannot open the file");
        return r;
    }
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return load_bytes(std::move(d), fs::u8path(path).filename().u8string(), raw);
}

std::shared_ptr<const pkg::Reader> open_package(const std::string& path, std::string* why) {
    static std::mutex mu;
    static std::map<std::string, std::shared_ptr<const pkg::Reader>> cache;  // 最近打开的几个包
    std::lock_guard<std::mutex> lk(mu);
    if (auto it = cache.find(path); it != cache.end()) return it->second;
    auto r = std::make_shared<pkg::Reader>();
    if (r->open(path) != IIS_OK) {
        if (why) *why = r->error();
        return nullptr;
    }
    if (cache.size() >= 4) cache.clear();
    cache[path] = r;
    return r;
}

std::string package_item(const std::string& package, size_t index) { return package + "|" + std::to_string(index); }

Loaded load_any(const std::string& path, const RawParams* raw) {
    const size_t bar = path.rfind('|');  // Windows 路径里不会有 '|'
    if (bar == std::string::npos) return load_file(path, raw);
    Loaded r;
    std::string why;
    const auto pk = open_package(path.substr(0, bar), &why);
    const size_t i = size_t(std::strtoull(path.c_str() + bar + 1, nullptr, 10));
    if (!pk || i >= pk->entries().size()) {
        r.error = i18n::tr("无法打开包：", "Cannot open the package: ") + why;
        return r;
    }
    const pkg::Entry& e = pk->entries()[i];
    std::vector<uint8_t> d;
    if (pk->original(i, d, why) != IIS_OK) {  // 核对 CRC-32 与 SHA-256
        r.error = i18n::tr("无法取出：", "Cannot take it out of the package: ") + why;
        return r;
    }
    const std::string name = e.path.substr(e.path.rfind('/') + 1);
    r = load_bytes(std::move(d), name, raw);
    r.in_package = true;
    r.file_size = e.size;
    return r;
}

bool package_mosaic(const pkg::Reader& pk, int size, std::vector<uint8_t>& rgba, int& w, int& h) {
    const auto& es = pk.entries();
    std::vector<size_t> cand;
    for (size_t i = 0; i < es.size(); ++i) {
        const pkg::Entry& e = es[i];
        if (!e.dir && e.bad.empty() && listable(e.path) && !pkg::is_package_name(e.path) && e.orig_size <= (256u << 20))
            cand.push_back(i);
    }
    auto depth = [&](size_t i) { return std::count(es[i].path.begin(), es[i].path.end(), '/'); };
    std::sort(cand.begin(), cand.end(), [&](size_t a, size_t b) {
        const auto da = depth(a), db = depth(b);
        return da != db ? da < db : es[a].path < es[b].path;
    });
    // 一张一张取：取出、缩小后立即释放原图，内存只占一张
    std::vector<Image> imgs;
    for (size_t k = 0; k < cand.size() && k < 12 && imgs.size() < 4; ++k) {
        std::vector<uint8_t> d;
        std::string why;
        const pkg::Entry& e = es[cand[k]];
        if (pk.original(cand[k], d, why) != IIS_OK) continue;  // 核对 CRC-32 与 SHA-256
        Loaded l = load_bytes(std::move(d), e.path.substr(e.path.rfind('/') + 1));
        if (l.ok && !l.pages.empty()) imgs.push_back(std::move(l.pages[0]));
    }
    if (imgs.empty()) return false;
    if (imgs.size() == 1) {
        thumbnail(imgs[0], size, rgba, w, h);
        for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
        return true;
    }
    // 格子：{x, y, 宽, 高}
    const int gap = std::max(2, size / 64), half = (size - gap) / 2, rest = size - half - gap;
    std::vector<std::array<int, 4>> cells;
    if (imgs.size() == 2) cells = {{0, 0, half, size}, {half + gap, 0, rest, size}};
    else if (imgs.size() == 3) cells = {{0, 0, half, size}, {half + gap, 0, rest, half}, {half + gap, half + gap, rest, rest}};
    else cells = {{0, 0, half, half}, {half + gap, 0, rest, half}, {0, half + gap, half, rest}, {half + gap, half + gap, rest, rest}};
    w = h = size;
    rgba.assign(size_t(size) * size * 4, 0);
    for (size_t i = 0; i < cells.size(); ++i) {
        const auto [cx, cy, cw, ch] = cells[i];
        const Image& im = imgs[i];
        // 缩到刚好盖满格子（短边对齐），过长的图最多放大到 4 倍格子再居中裁
        const double s = std::max(double(cw) / im.width, double(ch) / im.height);
        const int side = std::min(std::max(cw, ch) * 4, std::max(1, int(std::ceil(std::max(im.width, im.height) * s))));
        std::vector<uint8_t> t;
        int tw = 0, th = 0;
        thumbnail(im, side, t, tw, th);
        const int ox = (tw - cw) / 2, oy = (th - ch) / 2;
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x) {
                const int sx = x + ox, sy = y + oy;
                if (sx < 0 || sy < 0 || sx >= tw || sy >= th) continue;  // 太小的图：格子里留空
                uint8_t* q = &rgba[(size_t(cy + y) * size + cx + x) * 4];
                std::memcpy(q, &t[(size_t(sy) * tw + sx) * 4], 3);
                q[3] = 255;  // 只有缝透明
            }
    }
    return true;
}

Loaded load_bytes(std::vector<uint8_t> d, const std::string& name, const RawParams* raw) {
    Loaded r;
    r.name = name;
    r.file_size = d.size();
    // .iis：先还原（容器记录的 SHA-256 核对通过才继续）
    std::string orig_name = name;
    if (d.size() >= 8 && std::memcmp(d.data(), "IISCONT\0", 8) == 0) {
        iis_reader* rd = nullptr;
        iis_buffer out{};
        if (iis_reader_open_memory(d.data(), d.size(), &rd) != IIS_OK || iis_reader_restore(rd, &out) != IIS_OK) {
            r.error = std::string(i18n::tr("压缩文件无法还原：", "Cannot restore the compressed file: ")) + iis_last_error();
            iis_reader_close(rd);
            return r;
        }
        iis_container_info info{};
        iis_reader_info(rd, &info);
        r.sha256 = hex(info.orig_sha256);
        iis_reader_close(rd);
        d.assign(out.data, out.data + out.size);
        iis_buffer_free(&out);
        r.from_iis = true;
        if (lower(orig_name).size() > 4 && lower(orig_name).compare(orig_name.size() - 4, 4, ".iis") == 0)
            orig_name.resize(orig_name.size() - 4);
    } else {
        uint8_t h[32];
        iis_sha256(d.data(), d.size(), h);
        r.sha256 = hex(h);
    }
    r.orig_size = d.size();

    // 识别（与压缩相同）；RAW 参数来自界面输入
    fmt::RawRules rules;
    if (raw && raw->width && raw->height) {
        std::string j = "{\"raw\":[{\"match\":\"*\",\"width\":" + std::to_string(raw->width) +
                        ",\"height\":" + std::to_string(raw->height) + ",\"pixel_format\":\"" + raw->pixel_format + "\"";
        if (raw->stride) j += ",\"stride\":" + std::to_string(raw->stride);
        if (raw->offset) j += ",\"offset\":" + std::to_string(raw->offset);
        j += "}]}";
        std::string why;
        if (!fmt::parse_raw_rules(j, rules, why)) {
            r.error = i18n::tr("RAW 参数有误：", "Invalid RAW parameters: ") + why;
            return r;
        }
    }
    // 没给参数的无文件头 RAW 自动识别；识别不出来再请用户填参数
    const detect::Detection det = detect::detect(d.data(), d.size(), orig_name, raw ? &rules : nullptr, {}, !raw);
    const std::string kind = det.container == "bmp" ? "BMP" : det.container == "tiff" ? "TIFF" : det.container == "raw" ? "RAW" : det.container;
    auto from_iis = [&](const std::string& k) { return i18n::trf(".iis 压缩图像（原为 %s）", ".iis compressed image (originally %s)", k.c_str()); };
    r.kind = r.from_iis ? from_iis(kind.empty() ? std::string(i18n::tr("未知格式", "unknown format")) : kind) : kind;
    const std::string sep = i18n::tr("，", ", ");
    r.desc = std::to_string(det.width) + " × " + std::to_string(det.height) + sep +
             i18n::trf("%d 位存储 / %d 位有效", "%d-bit stored / %d-bit effective", int(det.bits_stored), int(det.bits_effective)) +
             (det.channels > 1 ? sep + i18n::trf("%d 通道", "%d channels", int(det.channels)) : "") +
             (det.compression.empty() || det.compression == "none" ? "" : sep + i18n::tr("压缩 ", "compression ") + det.compression) +
             (det.pages > 1 ? sep + i18n::trf("%d 页", "%d pages", int(det.pages)) : "") +
             (det.bayer_guess.empty() ? "" : sep + "Bayer " + det.bayer_guess);

    if (det.container == "bmp") {
        Image img;
        if (!bmp_pixels(d, det.bmp, img)) {
            r.error = i18n::tr("BMP 数据不完整", "The BMP data is incomplete");
            return r;
        }
        if (img.channels == 1 && valid_cfa(det.bayer_guess)) img.bayer = det.bayer_guess;
        r.pages.push_back(std::move(img));
    } else if (det.container == "tiff") {
        for (size_t i = 0; i < det.tiff_pages.size() && i < det.tiff.pages.size(); ++i) {
            const auto& pd = det.tiff_pages[i];
            const auto& pg = det.tiff.pages[i];
            if (pd.canvas.empty() || pg.samples < 1) continue;
            Image img;
            img.width = uint32_t(pg.width);
            img.height = uint32_t(pg.height);
            img.bits = pg.bits > 0 ? pg.bits : 16;
            const int s = pg.samples;
            img.channels = s >= 3 ? (s >= 4 ? 4 : 3) : 1;
            img.px.resize(size_t(img.width) * img.height * img.channels);
            const uint32_t maxv = (1u << img.bits) - 1;
            for (uint64_t y = 0; y < pg.height; ++y)
                for (uint64_t x = 0; x < pg.width; ++x)
                    for (int c = 0; c < img.channels; ++c) {
                        uint32_t v = pd.canvas[size_t((y * pd.canvas_w + x) * s + c)];
                        if (pg.photometric == 0 && img.channels == 1) v = maxv - std::min(v, maxv);  // 白为 0
                        img.px[size_t((y * img.width + x) * img.channels + c)] = uint16_t(v);
                    }
            if (img.channels == 1 && valid_cfa(det.bayer_guess)) img.bayer = det.bayer_guess;
            r.pages.push_back(std::move(img));
        }
        if (r.pages.empty()) {
            r.error = i18n::tr("TIFF 无法解码：", "Cannot decode the TIFF: ") +
                      (det.error.empty() ? std::string(i18n::tr("不支持的压缩或参数", "unsupported compression or parameters")) : det.error);
            return r;
        }
    } else if (det.raw && !det.raw_img.empty()) {
        Image img;
        img.width = uint32_t(det.raw->width);
        img.height = uint32_t(det.raw->height);
        img.bits = det.raw->pf ? det.raw->pf->bits : 16;
        img.px = det.raw_img;
        if (det.raw->pf && det.raw->pf->bayer) img.bayer = det.raw->pf->bayer;
        else if (valid_cfa(det.bayer_guess)) img.bayer = det.bayer_guess;
        r.pages.push_back(std::move(img));
        r.kind = r.from_iis ? from_iis("RAW " + det.raw->pixel_format) : "RAW " + det.raw->pixel_format;
        r.raw_auto = det.raw->origin == "auto";
        if (r.raw_auto) r.kind += i18n::tr("（宽高与格式为自动识别）", " (size and format detected automatically)");
    } else if (Image img; !(raw && raw->width) && wic_decode(d, img, r.kind)) {  // PNG、JPEG 等：Windows 自带解码器
        r.desc = std::to_string(img.width) + " × " + std::to_string(img.height) + i18n::tr("，8 位（仅预览）", ", 8-bit (preview only)");
        if (r.from_iis) r.kind = from_iis(r.kind);
        r.pages.push_back(std::move(img));
    } else {
        r.need_raw_params = det.container.empty() || det.container == "raw" || det.cls == "unknown";
        r.error = r.need_raw_params ? i18n::tr("无文件头的数据：请给出宽、高与像素格式（RAW）",
                                               "Raw data without a header: please give the width, height and pixel format (RAW)")
                                    : i18n::tr("不支持显示的格式：", "This format cannot be shown: ") + det.cls +
                                          (det.error.empty() ? "" : " (" + det.error + ")");
        r.original = std::move(d);
        return r;
    }
    r.original = std::move(d);
    r.ok = true;
    return r;
}

Window auto_window(const Image& img) {
    if (img.px.empty()) return {};
    const uint32_t maxv = img.bits >= 16 ? 65535u : (1u << img.bits) - 1;
    std::vector<uint32_t> hist(size_t(maxv) + 1, 0);
    const size_t step = std::max<size_t>(1, img.px.size() / 4000000);  // 大图抽样
    size_t n = 0;
    for (size_t i = 0; i < img.px.size(); i += step) {
        if (img.channels == 4 && i % 4 == 3) continue;  // 不算 Alpha
        ++hist[std::min<uint32_t>(img.px[i], maxv)];
        ++n;
    }
    const size_t lo_cut = n / 1000, hi_cut = n - n / 1000;
    Window w{0, double(maxv)};
    size_t acc = 0;
    bool lo_set = false;
    for (uint32_t v = 0; v <= maxv; ++v) {
        acc += hist[v];
        if (!lo_set && acc > lo_cut) w.lo = v, lo_set = true;
        if (acc >= hi_cut) {
            w.hi = v;
            break;
        }
    }
    if (w.hi <= w.lo) w.hi = w.lo + 1;
    return w;
}

void to_rgba(const Image& img, Window win, bool demosaic, const std::string& bayer, std::vector<uint8_t>& rgba,
             bool white_balance) {
    const size_t W = img.width, H = img.height;
    rgba.assign(W * H * 4, 255);
    const double scale = 255.0 / std::max(1e-9, win.hi - win.lo);
    auto map = [&](uint32_t v) -> uint8_t {
        const double t = (double(v) - win.lo) * scale;
        return uint8_t(t <= 0 ? 0 : t >= 255 ? 255 : int(t + 0.5));
    };
    // 8 / 16 位查找表，避免逐像素浮点
    const uint32_t maxv = img.bits >= 16 ? 65535u : (1u << img.bits) - 1;
    std::vector<uint8_t> lut(size_t(maxv) + 1);
    for (uint32_t v = 0; v <= maxv; ++v) lut[v] = map(v);
    auto L = [&](uint16_t v) { return lut[std::min<uint32_t>(v, maxv)]; };
    if (img.channels == 1 && demosaic && valid_cfa(bayer)) {
        // 双线性去马赛克：每个颜色取 3×3 邻域里同色像素的平均
        auto color = [&](size_t y, size_t x) { return bayer[(y % 2) * 2 + (x % 2)]; };
        std::vector<uint16_t> rgb(W * H * 3);
        for (size_t y = 0; y < H; ++y)
            for (size_t x = 0; x < W; ++x) {
                uint32_t sum[3] = {0, 0, 0}, cnt[3] = {0, 0, 0};
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int64_t yy = int64_t(y) + dy, xx = int64_t(x) + dx;
                        if (yy < 0 || xx < 0 || yy >= int64_t(H) || xx >= int64_t(W)) continue;
                        const char c = color(size_t(yy), size_t(xx));
                        const int k = c == 'R' ? 0 : c == 'G' ? 1 : 2;
                        // 本身就是该颜色时只用自己
                        if (color(y, x) == c && (dy || dx)) continue;
                        sum[k] += img.px[size_t(yy) * W + size_t(xx)];
                        ++cnt[k];
                    }
                for (int k = 0; k < 3; ++k) rgb[(y * W + x) * 3 + k] = uint16_t(cnt[k] ? sum[k] / cnt[k] : 0);
            }
        double gain[3] = {1, 1, 1};
        if (white_balance) {  // 灰度世界：R、B 的均值拉到与 G 相同
            double mean[3] = {0, 0, 0};
            for (size_t i = 0; i < W * H; ++i)
                for (int k = 0; k < 3; ++k) mean[k] += rgb[i * 3 + k];
            for (int k : {0, 2}) gain[k] = mean[k] > 0 ? std::clamp(mean[1] / mean[k], 0.25, 4.0) : 1.0;
        }
        for (size_t i = 0; i < W * H; ++i)
            for (int k = 0; k < 3; ++k)
                rgba[i * 4 + k] = L(uint16_t(std::min(65535.0, rgb[i * 3 + k] * gain[k])));
        return;
    }
    for (size_t i = 0; i < W * H; ++i) {
        uint8_t* o = &rgba[i * 4];
        if (img.channels == 1) {
            o[0] = o[1] = o[2] = L(img.px[i]);
        } else {
            const uint16_t* p = &img.px[i * img.channels];
            o[0] = L(p[0]), o[1] = L(p[1]), o[2] = L(p[2]);
        }
    }
}

void thumbnail(const Image& img, int max_side, std::vector<uint8_t>& rgba, int& tw, int& th) {
    const double s = std::min(1.0, double(max_side) / std::max(img.width, img.height));
    tw = std::max(1, int(img.width * s));
    th = std::max(1, int(img.height * s));
    // 先按区域平均缩小原始样本，再映射
    Image small;
    small.width = uint32_t(tw);
    small.height = uint32_t(th);
    small.channels = img.channels;
    small.bits = img.bits;
    small.px.assign(size_t(tw) * th * img.channels, 0);
    for (int y = 0; y < th; ++y)
        for (int x = 0; x < tw; ++x) {
            const size_t y0 = size_t(y) * img.height / th, y1 = std::max(y0 + 1, size_t(y + 1) * img.height / th);
            const size_t x0 = size_t(x) * img.width / tw, x1 = std::max(x0 + 1, size_t(x + 1) * img.width / tw);
            // 稀疏取样；步长取奇数，Bayer 图的奇偶行列都取到，不出条纹
            const size_t sy = std::max<size_t>(1, (y1 - y0) / 4) | 1, sx = std::max<size_t>(1, (x1 - x0) / 4) | 1;
            for (int c = 0; c < img.channels; ++c) {
                uint64_t sum = 0, n = 0;
                for (size_t yy = y0; yy < y1; yy += sy)
                    for (size_t xx = x0; xx < x1; xx += sx) sum += img.px[(yy * img.width + xx) * img.channels + c], ++n;
                small.px[(size_t(y) * tw + x) * img.channels + c] = uint16_t(n ? sum / n : 0);
            }
        }
    to_rgba(small, auto_window(small), false, "", rgba);
}

namespace {

std::string u8s(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

bool is_iis_name(const fs::path& p) {
    const std::wstring n = p.filename().wstring();
    return n.size() > 4 && _wcsicmp(n.c_str() + n.size() - 4, L".iis") == 0;
}

}  // namespace

RestoreResult restore_tree(const std::string& src_utf8, const std::string& out_utf8, bool recursive,
                           const std::function<void(const RestoreResult&)>& progress) {
    RestoreResult r;
    std::vector<fs::path> files;
    std::error_code ec;
    const fs::path src = fs::u8path(src_utf8), out = fs::u8path(out_utf8);
    const bool single = fs::is_regular_file(src, ec);
    const fs::path base = single ? src.parent_path() : src;
    if (single) {
        if (is_iis_name(src)) files.push_back(src);
    } else {
        auto take = [&](const fs::directory_entry& e) {
            std::error_code e2;
            if (e.is_regular_file(e2) && is_iis_name(e.path())) files.push_back(e.path());
        };
        if (recursive)
            for (const auto& e : fs::recursive_directory_iterator(src, fs::directory_options::skip_permission_denied, ec)) take(e);
        else
            for (const auto& e : fs::directory_iterator(src, ec)) take(e);
    }
    std::sort(files.begin(), files.end());
    r.total = int(files.size());
    if (progress) progress(r);
    for (const auto& f : files) {
        fs::path rel = fs::relative(f, base, ec);
        if (ec || rel.empty()) rel = f.filename();
        rel.replace_extension();  // 去掉 .iis
        const fs::path target = out / rel;
        fs::create_directories(target.parent_path(), ec);
        if (fs::exists(target, ec)) {
            ++r.skipped;
        } else {
            iis_reader* rd = nullptr;
            int st = iis_reader_open_file(u8s(f).c_str(), &rd);
            if (st == IIS_OK) st = iis_reader_restore_file(rd, u8s(target).c_str(), 1);
            iis_reader_close(rd);
            if (st == IIS_OK) {
                ++r.ok;
            } else {
                ++r.failed;
                if (r.errors.size() < 100) r.errors.push_back(u8s(rel) + "：" + iis_last_error());
            }
        }
        if (progress) progress(r);
    }
    return r;
}

std::string watched_root(const std::string& dir_utf8, const std::string& config_utf8) {
    fs::path cfg = fs::u8path(config_utf8);
    if (cfg.empty()) {
        wchar_t buf[1024];
        DWORD n = sizeof buf;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\IIS\\Install", L"DataFolder", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
                         nullptr, buf, &n) == ERROR_SUCCESS)
            cfg = fs::path(buf) / L"config.json";
        else if (const DWORD m = GetEnvironmentVariableW(L"ProgramData", buf, 1024); m && m < 1024)
            cfg = fs::path(buf) / L"IIS" / L"config.json";
    }
    std::ifstream f(cfg, std::ios::binary);
    if (!f) return "";
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
    const auto j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("watch") || !j["watch"].is_array()) return "";
    auto norm = [](fs::path p) {
        std::wstring w = p.lexically_normal().wstring();
        while (w.size() > 3 && (w.back() == L'\\' || w.back() == L'/')) w.pop_back();
        std::transform(w.begin(), w.end(), w.begin(), [](wchar_t c) { return wchar_t(towlower(c)); });
        return w;
    };
    const std::wstring d = norm(fs::u8path(dir_utf8));
    for (const auto& w : j["watch"]) {
        if (!w.is_object() || !w.contains("path") || !w["path"].is_string()) continue;
        fs::path root = fs::u8path(w["path"].get<std::string>());
        if (root.is_relative()) root = cfg.parent_path() / root;
        const std::wstring r = norm(root);
        if (d == r || (d.size() > r.size() && d.compare(0, r.size(), r) == 0 && (r.back() == L'\\' || d[r.size()] == L'\\')))
            return u8s(root);
    }
    return "";
}

}  // namespace iis::view
