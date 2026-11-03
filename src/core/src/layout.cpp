#include "layout.hpp"

#include "tiffcodec.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <stdexcept>

namespace iis::layout {

using nlohmann::json;
using fmt::Samples;

namespace {

constexpr int kPhases[4][2] = {{0, 0}, {0, 1}, {1, 0}, {1, 1}};

int eff_bits(const Samples& v) {
    uint32_t m = 0;
    for (auto x : v) m = std::max<uint32_t>(m, x);
    int n = 0;
    while (m) ++n, m >>= 1;
    return std::max(1, n);
}

// 单通道图像 → 平面（移位 → Bayer 拆分），与 bench/layout/transforms.gray_to_planes 相同
bool gray_to_planes(const Samples& img, uint64_t h, uint64_t w, int shift, bool bayer, const std::string& role,
                    uint32_t image, std::vector<Plane>& out) {
    Samples v = img;
    if (shift) {
        const uint32_t low = (1u << shift) - 1;
        for (auto& x : v) {
            if (x & low) return false;  // 低位不全为 0，不能右移
            x = uint16_t(x >> shift);
        }
    }
    if (!bayer) {
        Plane p;
        p.role = role;
        p.image = image;
        p.width = w;
        p.height = h;
        p.shift = shift;
        p.px = std::move(v);
        p.bits = eff_bits(p.px);
        out.push_back(std::move(p));
        return true;
    }
    for (const auto& ph : kPhases) {
        Plane p;
        p.role = role;
        p.image = image;
        p.bayer_dy = ph[0];
        p.bayer_dx = ph[1];
        p.height = (h - uint64_t(ph[0]) + 1) / 2;
        p.width = (w - uint64_t(ph[1]) + 1) / 2;
        p.shift = shift;
        p.px.resize(size_t(p.height * p.width));
        for (uint64_t y = 0; y < p.height; ++y)
            for (uint64_t x = 0; x < p.width; ++x)
                p.px[size_t(y * p.width + x)] = v[size_t((2 * y + ph[0]) * w + 2 * x + ph[1])];
        p.bits = eff_bits(p.px);
        out.push_back(std::move(p));
    }
    return true;
}

// 像素区间之外的部分即原样字节区间（bench/layout/model.complement）
bool complement(std::vector<cont::Range> pix, uint64_t size, std::vector<cont::Range>& out, std::string& why) {
    pix.erase(std::remove_if(pix.begin(), pix.end(), [](const cont::Range& r) { return r.length == 0; }), pix.end());
    std::sort(pix.begin(), pix.end(), [](const auto& a, const auto& b) { return a.offset < b.offset; });
    uint64_t pos = 0;
    out.clear();
    for (const auto& r : pix) {
        if (r.offset < pos) return why = "像素段重叠或乱序", false;
        if (r.offset > size || r.length > size - r.offset) return why = "像素段越界", false;
        if (r.offset > pos) out.push_back({pos, r.offset - pos});
        pos = r.offset + r.length;
    }
    if (pos < size) out.push_back({pos, size - pos});
    return true;
}

// ---------------- 复现式还原：找能逐字节重编码的编码器（bench/layout/reproduce.py）----------------

struct ReproSeg {
    const uint8_t* orig;
    size_t orig_len;
    const std::vector<uint8_t>* raw;
    size_t row_bytes;
};

bool same(const tiffcodec::Bytes& a, const uint8_t* b, size_t n) { return a.size() == n && std::memcmp(a.data(), b, n) == 0; }

tiffcodec::Bytes encode_block(const json& enc, char variant, const std::vector<uint8_t>& raw, size_t row_bytes) {
    const std::string name = enc.at("name").get<std::string>();
    const uint8_t* p = raw.data();
    const size_t n = raw.size();
    if (name == "imcd-lzw" || (name == "lzw" && variant == 'I')) return tiffcodec::lzw_encode_imcd(p, n);
    if (name == "libtiff-lzw" || (name == "lzw" && variant == 'L')) return tiffcodec::lzw_encode_libtiff(p, n);
    if (name == "zlib") return tiffcodec::zlib_encode(p, n, enc.at("level").get<int>());
    if (name == "libdeflate") return tiffcodec::libdeflate_encode(p, n, enc.at("level").get<int>());
    if (name == "packbits-rows") return tiffcodec::packbits_encode_rows(p, n, row_bytes);
    if (name == "packbits") return tiffcodec::packbits_encode(p, n);
    throw std::runtime_error("未知的复现编码器 " + name);
}

bool find_encoder(const char* comp, const std::vector<ReproSeg>& segs, json& encoder, json& encoders) {
    encoders = nullptr;
    if (segs.empty()) return false;
    if (std::strcmp(comp, "lzw") == 0) {  // 逐块：先 imagecodecs 再 libtiff
        std::string v;
        for (const auto& s : segs) {
            if (same(tiffcodec::lzw_encode_imcd(s.raw->data(), s.raw->size()), s.orig, s.orig_len)) v += 'I';
            else if (same(tiffcodec::lzw_encode_libtiff(s.raw->data(), s.raw->size()), s.orig, s.orig_len)) v += 'L';
            else return false;
        }
        if (v.find('L') == std::string::npos) encoder = {{"name", "imcd-lzw"}};
        else if (v.find('I') == std::string::npos) encoder = {{"name", "libtiff-lzw"}};
        else encoder = {{"name", "lzw"}}, encoders = v;
        return true;
    }
    std::vector<json> cands;
    if (std::strcmp(comp, "zip") == 0) {
        for (int lv : {6, 9, 1, 2, 3, 4, 5, 7, 8}) {  // 常见默认值优先
            cands.push_back({{"name", "zlib"}, {"level", lv}});
            cands.push_back({{"name", "libdeflate"}, {"level", lv}});
        }
    } else {
        cands = {{{"name", "packbits-rows"}}, {{"name", "packbits"}}};
    }
    for (const auto& c : cands) {
        bool all = true;
        for (const auto& s : segs)
            if (!same(encode_block(c, 0, *s.raw, s.row_bytes), s.orig, s.orig_len)) {
                all = false;
                break;
            }
        if (all) return encoder = c, true;
    }
    return false;
}

// ---------------- 拆分 ----------------

bool split_bmp(const uint8_t* d, size_t n, const detect::Detection& det, Split& s, std::string& why) {
    const fmt::BmpInfo& b = det.bmp;
    if (b.damaged()) return why = "BMP 截断", false;
    const uint64_t h = b.height, w = b.width, ch = b.channels, rb = b.row_bytes(), stride = b.stride;
    const uint8_t* base = d + b.pixel_offset;
    // 行填充（文件行序）
    std::vector<uint8_t> pad;
    bool pad_zero = true;
    if (stride > rb) {
        pad.resize(size_t(h * (stride - rb)));
        for (uint64_t y = 0; y < h; ++y) {
            std::memcpy(&pad[size_t(y * (stride - rb))], base + y * stride + rb, size_t(stride - rb));
        }
        pad_zero = std::all_of(pad.begin(), pad.end(), [](uint8_t c) { return c == 0; });
    }
    auto file_row = [&](uint64_t y) { return base + (b.top_down ? y : h - 1 - y) * stride; };
    json alpha_const = nullptr;
    if (ch == 1) {
        Samples img(size_t(h * w));
        for (uint64_t y = 0; y < h; ++y) {
            const uint8_t* r = file_row(y);
            for (uint64_t x = 0; x < w; ++x) img[size_t(y * w + x)] = r[x];
        }
        const bool gray = b.gray_palette();
        if (!gray_to_planes(img, h, w, 0, det.cls == "bayer" && gray, gray ? "gray" : "index", 0, s.planes))
            return why = "移位失败", false;
    } else {
        Plane rgb;
        rgb.role = "rgb";
        rgb.width = w;
        rgb.height = h;
        rgb.components = 3;
        rgb.px.resize(size_t(h * w * 3));
        Samples alpha;
        if (ch == 4) alpha.resize(size_t(h * w));
        for (uint64_t y = 0; y < h; ++y) {
            const uint8_t* r = file_row(y);
            for (uint64_t x = 0; x < w; ++x) {
                const uint8_t* px = r + x * ch;
                uint16_t* o = &rgb.px[size_t((y * w + x) * 3)];
                o[0] = px[2];
                o[1] = px[1];
                o[2] = px[0];
                if (ch == 4) alpha[size_t(y * w + x)] = px[3];
            }
        }
        rgb.bits = eff_bits(rgb.px);
        s.planes.push_back(std::move(rgb));
        if (ch == 4) {
            if (std::all_of(alpha.begin(), alpha.end(), [&](uint16_t a) { return a == alpha[0]; })) {
                alpha_const = alpha[0];
            } else {
                Plane a;
                a.role = "alpha";
                a.width = w;
                a.height = h;
                a.px = std::move(alpha);
                a.bits = eff_bits(a.px);
                s.planes.push_back(std::move(a));
            }
        }
    }
    if (!pad_zero) s.extras.push_back({"row_padding", 0, -1, std::move(pad)});
    s.format = "bmp";
    s.layout = {{"width", w},          {"height", h},         {"channels", ch},
                {"top_down", b.top_down}, {"stride", stride},  {"pixel_offset", b.pixel_offset},
                {"padding_zero", pad_zero}, {"alpha_const", alpha_const}};
    return complement({{b.pixel_offset, b.pixel_len()}}, n, s.skeleton, why);
}

bool split_raw(const uint8_t* d, size_t n, const detect::Detection& det, Split& s, std::string& why) {
    if (!det.raw) return why = "RAW 无参数", false;
    const fmt::RawParams& p = *det.raw;
    const uint64_t off = p.offset, len = p.data_len();
    if (off + len > n) return why = "RAW 参数超出文件长度", false;
    const uint8_t* body = d + off;
    bool pad_zero = true;
    if (p.pf->packed) {
        const auto again = fmt::pack(p.pf->packed, det.raw_img.data(), p.width * p.height);
        if (again.size() != len || (len && std::memcmp(again.data(), body, size_t(len - 1)) != 0))
            return why = "打包格式重新打包不一致", false;
        if (len) s.extras.push_back({"packed_tail", 0, -1, {body[len - 1]}});  // 末字节可能含未用位
    } else {
        const uint64_t rb = p.width * uint64_t(p.pf->bpp), stride = p.row_stride();
        if (stride > rb) {
            std::vector<uint8_t> pad(size_t(p.height * (stride - rb)));
            for (uint64_t y = 0; y < p.height; ++y)
                std::memcpy(&pad[size_t(y * (stride - rb))], body + y * stride + rb, size_t(stride - rb));
            pad_zero = std::all_of(pad.begin(), pad.end(), [](uint8_t c) { return c == 0; });
            if (!pad_zero) s.extras.push_back({"row_padding", 0, -1, std::move(pad)});
        }
    }
    if (!gray_to_planes(det.raw_img, p.height, p.width, det.shift, det.cls == "bayer", "gray", 0, s.planes))
        return why = "低位不全为 0，不能右移", false;
    s.format = "raw";
    s.layout = {{"pixel_format", p.pixel_format}, {"width", p.width},       {"height", p.height},
                {"stride", p.stride && !p.pf->packed ? json(p.stride) : json(nullptr)},   {"body_offset", off},
                {"body_len", len},                {"padding_zero", pad_zero}};
    return complement({{off, len}}, n, s.skeleton, why);
}

bool split_tiff(const uint8_t* d, size_t n, const detect::Detection& det, Split& s, std::string& why) {
    if (det.tiff_pages.size() != det.tiff.pages.size()) return why = "TIFF 未解码", false;
    std::vector<cont::Range> ranges;
    json pages = json::array();
    for (size_t pi = 0; pi < det.tiff.pages.size(); ++pi) {
        const fmt::TiffPage& p = det.tiff.pages[pi];
        const detect::TiffPageData& pd = det.tiff_pages[pi];
        const char* comp = p.compression_name();
        if (comp && std::strcmp(comp, "other") == 0) return why = "压缩方式不支持", false;
        if (!pd.split_error.empty()) return why = pd.split_error, false;
        std::vector<ReproSeg> repro;
        for (size_t k = 0; k < pd.segs.size(); ++k) {
            ranges.push_back({p.offsets[k], p.bytecounts[k]});
            const size_t rb = size_t(pd.segs[k].cols * uint64_t(pd.spp) * uint64_t(pd.bps));
            if (comp) repro.push_back({d + p.offsets[k], size_t(p.bytecounts[k]), &pd.raw[k], rb});
            else if (!pd.tails[k].empty()) s.extras.push_back({"block_tail", uint32_t(pi), int64_t(k), pd.tails[k]});
        }
        json encoder = nullptr, encoders = nullptr;
        if (comp && !find_encoder(comp, repro, encoder, encoders))
            return why = "第 " + std::to_string(pi) + " 页 " + comp + " 复现式还原失败：候选编码器都无法逐字节复现", false;

        const uint64_t ch = pd.canvas_h, cw = pd.canvas_w, ns = uint64_t(p.samples);
        if (p.samples == 1) {
            const std::string role = p.photometric == 3 ? "index" : "gray";
            const bool bayer = det.cls == "bayer";
            if (!gray_to_planes(pd.canvas, ch, cw, det.shift, bayer, role, uint32_t(pi), s.planes))
                gray_to_planes(pd.canvas, ch, cw, 0, bayer, role, uint32_t(pi), s.planes);  // 图块填充区低位非 0 等：不移位
        } else if (p.samples == 3) {
            Plane rgb;
            rgb.role = "rgb";
            rgb.image = uint32_t(pi);
            rgb.width = cw;
            rgb.height = ch;
            rgb.components = 3;
            rgb.px = pd.canvas;
            rgb.bits = eff_bits(rgb.px);
            s.planes.push_back(std::move(rgb));
        } else {
            for (uint64_t k = 0; k < ns; ++k) {
                Plane pl;
                pl.role = "sample";
                pl.image = uint32_t(pi);
                pl.sample = int(k);
                pl.width = cw;
                pl.height = ch;
                pl.px.resize(size_t(ch * cw));
                for (uint64_t i = 0; i < ch * cw; ++i) pl.px[size_t(i)] = pd.canvas[size_t(i * ns + k)];
                pl.bits = eff_bits(pl.px);
                s.planes.push_back(std::move(pl));
            }
        }
        pages.push_back({{"index", pi},
                         {"width", p.width},
                         {"height", p.height},
                         {"samples", p.samples},
                         {"bits", p.bits},
                         {"planar", p.planar},
                         {"tiled", p.tiled},
                         {"tile", p.tiled ? json::array({p.tile_h, p.tile_w}) : json(nullptr)},
                         {"rowsperstrip", p.tiled || !p.rowsperstrip ? json(nullptr) : json(p.rowsperstrip)},
                         {"predictor", p.predictor},
                         {"compression", comp ? json(comp) : json(nullptr)},
                         {"offsets", p.offsets},
                         {"bytecounts", p.bytecounts},
                         {"encoder", encoder},
                         {"encoders", encoders}});
    }
    s.format = "tiff";
    s.layout = {{"byteorder", det.tiff.little ? "II" : "MM"}, {"bigtiff", det.tiff.bigtiff}, {"pages", pages}};
    return complement(ranges, n, s.skeleton, why);
}

// ---------------- 重组 ----------------

struct Bad : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void need(bool ok, const char* what) {
    if (!ok) throw Bad(what);
}

uint64_t u64(const json& j, const char* k) {
    const json& v = j.at(k);
    need(v.is_number_unsigned() || (v.is_number_integer() && v.get<int64_t>() >= 0), k);
    return v.get<uint64_t>();
}

struct Ctx {
    const json& recipe;
    const Fetch& fetch;
    uint8_t* out;
    uint64_t size;
    std::vector<cont::Range>& covered;

    // 解码后的平面 → 16 位样本（单样本字节数由平面位数决定，见规格 6.2）
    Samples plane_samples(const json& pm) const {
        std::vector<uint8_t> b;
        const uint16_t id = uint16_t(u64(pm, "id"));
        if (fetch(cont::PLANE, id, b) != IIS_OK) throw Bad(iis_last_error());
        const uint64_t cnt = u64(pm, "width") * u64(pm, "height") * u64(pm, "components");
        const uint64_t bits = u64(pm, "bits");
        need(bits >= 1 && bits <= 16, "平面位数无效");
        Samples v(static_cast<size_t>(cnt));
        if (bits <= 8) {
            need(b.size() == cnt, "平面长度不符");
            for (size_t i = 0; i < v.size(); ++i) v[i] = b[i];
        } else {
            need(b.size() == cnt * 2, "平面长度不符");
            for (size_t i = 0; i < v.size(); ++i) v[i] = uint16_t(b[2 * i] | b[2 * i + 1] << 8);
        }
        return v;
    }

    std::vector<uint8_t> extra(const json& em) const {
        std::vector<uint8_t> b;
        if (fetch(cont::EXTRA, uint16_t(u64(em, "id")), b) != IIS_OK) throw Bad(iis_last_error());
        return b;
    }

    std::vector<const json*> planes_of(uint64_t image) const {
        std::vector<const json*> v;
        for (const auto& p : recipe.at("planes"))
            if (u64(p, "image") == image) v.push_back(&p);
        return v;
    }

    const json* extra_of(const std::string& role, uint64_t image, int64_t block = -1) const {
        for (const auto& e : recipe.at("extras"))
            if (e.at("role") == role && u64(e, "image") == image &&
                (block < 0 || (e.contains("block") && u64(e, "block") == uint64_t(block))))
                return &e;
        return nullptr;
    }

    // 单通道图像：1 个平面或 4 个 Bayer 相位平面 → h×w，再左移还原
    Samples gray(uint64_t image, uint64_t h, uint64_t w, int sample = -1) const {
        std::vector<const json*> ps;
        for (const json* p : planes_of(image)) {
            const std::string role = p->at("role");
            if (role == "gray" || role == "index" || (role == "sample" && sample >= 0 && u64(*p, "sample") == uint64_t(sample)))
                ps.push_back(p);
        }
        need(!ps.empty(), "缺少灰度平面");
        Samples img(size_t(h * w));
        const uint64_t shift = u64(*ps[0], "shift");
        need(shift < 16, "移位无效");
        if (ps.size() == 1 && ps[0]->at("bayer_phase").is_null()) {
            need(u64(*ps[0], "width") == w && u64(*ps[0], "height") == h && u64(*ps[0], "components") == 1, "平面尺寸不符");
            img = plane_samples(*ps[0]);
        } else {
            need(ps.size() == 4, "Bayer 平面数不是 4");
            for (const json* p : ps) {
                const auto& ph = p->at("bayer_phase");
                need(ph.is_array() && ph.size() == 2, "Bayer 相位无效");
                const uint64_t dy = ph[0].get<uint64_t>(), dx = ph[1].get<uint64_t>();
                need(dy < 2 && dx < 2 && u64(*p, "shift") == shift, "Bayer 相位无效");
                const uint64_t ph_h = (h - dy + 1) / 2, ph_w = (w - dx + 1) / 2;
                need(u64(*p, "height") == ph_h && u64(*p, "width") == ph_w && u64(*p, "components") == 1, "Bayer 平面尺寸不符");
                const Samples v = plane_samples(*p);
                for (uint64_t y = 0; y < ph_h; ++y)
                    for (uint64_t x = 0; x < ph_w; ++x) img[size_t((2 * y + dy) * w + 2 * x + dx)] = v[size_t(y * ph_w + x)];
            }
        }
        if (shift)
            for (auto& v : img) v = uint16_t(v << shift);
        return img;
    }

    const json& role_plane(uint64_t image, const char* role) const {
        for (const json* p : planes_of(image))
            if (p->at("role") == role) return *p;
        throw Bad(std::string("缺少平面 ") + role);
    }

    void put(uint64_t off, const uint8_t* p, uint64_t len) {
        need(off <= size && len <= size - off, "像素区超出原文件");
        if (len) std::memcpy(out + off, p, size_t(len));
        if (len) covered.push_back({off, len});
    }
};

void rebuild_bmp(Ctx& c) {
    const json& L = c.recipe.at("layout");
    const uint64_t w = u64(L, "width"), h = u64(L, "height"), ch = u64(L, "channels"), stride = u64(L, "stride"),
                   off = u64(L, "pixel_offset");
    need(ch == 1 || ch == 3 || ch == 4, "通道数无效");
    need(w && h && stride >= w * ch && h <= c.size && stride <= c.size, "BMP 版面参数无效");
    const bool top_down = L.at("top_down").get<bool>();
    const uint64_t rb = w * ch;
    std::vector<uint8_t> rows(size_t(h * stride), 0);
    if (!L.at("padding_zero").get<bool>()) {
        const json* e = c.extra_of("row_padding", 0);
        need(e, "缺少行填充附加段");
        const auto pad = c.extra(*e);
        need(pad.size() == h * (stride - rb), "行填充长度不符");
        for (uint64_t y = 0; y < h; ++y) std::memcpy(&rows[size_t(y * stride + rb)], &pad[size_t(y * (stride - rb))], size_t(stride - rb));
    }
    auto file_row = [&](uint64_t y) { return &rows[size_t((top_down ? y : h - 1 - y) * stride)]; };
    if (ch == 1) {
        const Samples img = c.gray(0, h, w);
        for (uint64_t y = 0; y < h; ++y) {
            uint8_t* r = file_row(y);
            for (uint64_t x = 0; x < w; ++x) r[x] = uint8_t(img[size_t(y * w + x)]);
        }
    } else {
        const json& pm = c.role_plane(0, "rgb");
        need(u64(pm, "width") == w && u64(pm, "height") == h && u64(pm, "components") == 3, "RGB 平面尺寸不符");
        const Samples rgb = c.plane_samples(pm);
        Samples alpha;
        const json& ac = L.at("alpha_const");
        if (ch == 4 && ac.is_null()) {
            const json& am = c.role_plane(0, "alpha");
            need(u64(am, "width") == w && u64(am, "height") == h, "Alpha 平面尺寸不符");
            alpha = c.plane_samples(am);
        }
        for (uint64_t y = 0; y < h; ++y) {
            uint8_t* r = file_row(y);
            for (uint64_t x = 0; x < w; ++x) {
                const uint16_t* s = &rgb[size_t((y * w + x) * 3)];
                uint8_t* px = r + x * ch;
                px[0] = uint8_t(s[2]);
                px[1] = uint8_t(s[1]);
                px[2] = uint8_t(s[0]);
                if (ch == 4) px[3] = alpha.empty() ? uint8_t(ac.get<uint64_t>()) : uint8_t(alpha[size_t(y * w + x)]);
            }
        }
    }
    c.put(off, rows.data(), rows.size());
}

void rebuild_raw(Ctx& c) {
    const json& L = c.recipe.at("layout");
    const fmt::PixelFormat* pf = fmt::find_pixel_format(L.at("pixel_format").get<std::string>());
    need(pf, "像素格式未知");
    const uint64_t w = u64(L, "width"), h = u64(L, "height"), off = u64(L, "body_offset"), len = u64(L, "body_len");
    need(w && h && len <= c.size && w * h <= c.size * 8, "RAW 版面参数无效");
    const Samples img = c.gray(0, h, w);
    std::vector<uint8_t> body;
    if (pf->packed) {
        body = fmt::pack(pf->packed, img.data(), w * h);
        const json* e = c.extra_of("packed_tail", 0);
        need(e && !body.empty(), "缺少打包尾字节");
        const auto tail = c.extra(*e);
        need(tail.size() == 1, "打包尾字节长度不符");
        body.back() = tail[0];
    } else {
        const uint64_t rb = w * uint64_t(pf->bpp), stride = L.at("stride").is_null() ? rb : u64(L, "stride");
        need(stride >= rb, "行跨距无效");
        body.assign(size_t(stride * h), 0);
        for (uint64_t y = 0; y < h; ++y)
            for (uint64_t x = 0; x < w; ++x) {
                const uint16_t v = img[size_t(y * w + x)];
                uint8_t* o = &body[size_t(y * stride + x * pf->bpp)];
                o[0] = uint8_t(v);
                if (pf->bpp == 2) o[1] = uint8_t(v >> 8);
            }
        if (!L.at("padding_zero").get<bool>()) {
            const json* e = c.extra_of("row_padding", 0);
            need(e, "缺少行填充附加段");
            const auto pad = c.extra(*e);
            need(pad.size() == h * (stride - rb), "行填充长度不符");
            for (uint64_t y = 0; y < h; ++y)
                std::memcpy(&body[size_t(y * stride + rb)], &pad[size_t(y * (stride - rb))], size_t(stride - rb));
        }
    }
    need(body.size() == len, "RAW 重组长度不符");
    c.put(off, body.data(), body.size());
}

void rebuild_tiff(Ctx& c) {
    const json& L = c.recipe.at("layout");
    const bool little = L.at("byteorder") == "II";
    for (const auto& pr : L.at("pages")) {
        const uint64_t image = u64(pr, "index");
        detect::TiffPageData g;
        const uint64_t samples = u64(pr, "samples");
        need(samples >= 1 && samples <= 64, "分量数无效");
        const bool tiled = pr.at("tiled").get<bool>();
        uint64_t th = 0, tw = 0;
        if (tiled) {
            th = pr.at("tile").at(0).get<uint64_t>();
            tw = pr.at("tile").at(1).get<uint64_t>();
        }
        const uint64_t rps = pr.at("rowsperstrip").is_null() ? 0 : u64(pr, "rowsperstrip");
        need(detect::tiff_geometry(u64(pr, "width"), u64(pr, "height"), int(samples), int(u64(pr, "bits")),
                                   int(u64(pr, "planar")), tiled, th, tw, rps, g),
             "TIFF 页参数无效");
        need(g.canvas_h * g.canvas_w * samples <= c.size * 8 + 1024, "TIFF 画布过大");
        const auto& offsets = pr.at("offsets");
        const auto& counts = pr.at("bytecounts");
        need(offsets.size() == g.segs.size() && counts.size() == g.segs.size(), "像素块数不符");
        const uint64_t ch = g.canvas_h, cw = g.canvas_w;
        Samples canvas(size_t(ch * cw * samples));
        if (samples == 1) {
            canvas = c.gray(image, ch, cw);
        } else if (samples == 3) {
            const json& pm = c.role_plane(image, "rgb");
            need(u64(pm, "width") == cw && u64(pm, "height") == ch && u64(pm, "components") == 3, "RGB 平面尺寸不符");
            canvas = c.plane_samples(pm);
        } else {
            for (uint64_t k = 0; k < samples; ++k) {
                const Samples v = c.gray(image, ch, cw, int(k));
                for (uint64_t i = 0; i < ch * cw; ++i) canvas[size_t(i * samples + k)] = v[size_t(i)];
            }
        }
        const json& comp = pr.at("compression");
        const int predictor = int(u64(pr, "predictor"));
        need(predictor == 1 || predictor == 2, "Predictor 无效");
        const uint64_t mask = g.bps == 1 ? 0xFF : 0xFFFF;
        for (size_t i = 0; i < g.segs.size(); ++i) {
            const detect::TiffSeg& sg = g.segs[i];
            const uint64_t spp = uint64_t(g.spp);
            std::vector<uint8_t> raw(size_t(sg.rows * sg.cols * spp * uint64_t(g.bps)));
            for (uint64_t r = 0; r < sg.rows; ++r)
                for (uint64_t s = 0; s < spp; ++s) {
                    uint32_t prev = 0;
                    for (uint64_t col = 0; col < sg.cols; ++col) {
                        const uint64_t chn = sg.sample >= 0 ? uint64_t(sg.sample) : s;
                        uint32_t v = canvas[size_t(((sg.y + r) * cw + sg.x + col) * samples + chn)];
                        if (predictor == 2) {  // 水平差分，按位宽回绕
                            const uint32_t cur = v;
                            v = uint32_t((uint64_t(v) - prev) & mask);
                            prev = cur;
                        }
                        uint8_t* o = &raw[size_t(((r * sg.cols + col) * spp + s) * uint64_t(g.bps))];
                        if (g.bps == 1) o[0] = uint8_t(v);
                        else if (little) o[0] = uint8_t(v), o[1] = uint8_t(v >> 8);
                        else o[0] = uint8_t(v >> 8), o[1] = uint8_t(v);
                    }
                }
            std::vector<uint8_t> seg;
            if (comp.is_null()) {
                seg = std::move(raw);
                if (const json* e = c.extra_of("block_tail", image, int64_t(i))) {
                    const auto tail = c.extra(*e);
                    seg.insert(seg.end(), tail.begin(), tail.end());
                }
            } else {
                const json& enc = pr.at("encoder");
                const json& encs = pr.at("encoders");
                char variant = 0;
                if (!encs.is_null()) {
                    const std::string v = encs.get<std::string>();
                    need(v.size() == g.segs.size(), "逐块编码器记录长度不符");
                    variant = v[i];
                }
                seg = encode_block(enc, variant, raw, size_t(sg.cols * spp * uint64_t(g.bps)));
            }
            const json& cj = counts[i];
            const json& oj = offsets[i];
            need(cj.is_number_unsigned() && oj.is_number_unsigned(), "像素块位置无效");
            need(seg.size() == cj.get<uint64_t>(), "像素块重组长度与原文件不符");
            c.put(oj.get<uint64_t>(), seg.data(), seg.size());
        }
    }
}

}  // namespace

bool split(const uint8_t* d, size_t n, const detect::Detection& det, Split& out, std::string& why) {
    out = {};
    try {
        if (det.container == "bmp") return split_bmp(d, n, det, out, why);
        if (det.container == "raw") return split_raw(d, n, det, out, why);
        if (det.container == "tiff") return split_tiff(d, n, det, out, why);
    } catch (const std::exception& e) {
        why = std::string("拆分异常：") + e.what();
        return false;
    }
    why = "容器 " + det.container + " 不拆分";
    return false;
}

int rebuild(const json& recipe, const Fetch& fetch, uint8_t* out, uint64_t size, std::vector<cont::Range>& covered) {
    Ctx c{recipe, fetch, out, size, covered};
    try {
        const std::string f = recipe.at("format").get<std::string>();
        if (f == "bmp") rebuild_bmp(c);
        else if (f == "raw") rebuild_raw(c);
        else if (f == "tiff") rebuild_tiff(c);
        else return fail(IIS_E_UNSUPPORTED, "不支持的配方格式 " + f);
    } catch (const Bad& e) {
        return fail(IIS_E_CORRUPT, std::string("配方与数据不符：") + e.what());
    } catch (const nlohmann::json::exception& e) {
        return fail(IIS_E_CORRUPT, std::string("配方字段缺失或类型不符：") + e.what());
    } catch (const std::runtime_error& e) {
        return fail(IIS_E_CORRUPT, e.what());
    }
    return IIS_OK;
}

}  // namespace iis::layout
