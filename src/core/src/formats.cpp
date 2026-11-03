#include "formats.hpp"

#include <algorithm>
#include <cstring>
#include <set>

namespace iis::fmt {

namespace {

uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

bool dib_known(uint32_t s) { return s == 12 || s == 40 || s == 52 || s == 56 || s == 108 || s == 124; }

}  // namespace

// ---------------- BMP（bench/formats/bmp.py）----------------

bool is_bmp(const uint8_t* d, size_t n) { return n >= 18 && d[0] == 'B' && d[1] == 'M' && dib_known(le32(d + 14)); }

bool BmpInfo::gray_palette() const {
    const size_t n = palette.size() / 4;
    if (!n) return false;
    const size_t den = std::max<size_t>(1, n - 1);
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* p = &palette[4 * i];
        if (p[0] != p[1] || p[1] != p[2] || p[0] != i * 255 / den) return false;
    }
    return true;
}

Parse parse_bmp(const uint8_t* d, size_t n, BmpInfo& o, std::string& why) {
    if (!is_bmp(d, n)) return why = "缺少 BM 魔数或 DIB 头长度异常", Parse::Bad;
    o = {};
    o.file_len = n;
    o.pixel_offset = le32(d + 10);
    o.dib = le32(d + 14);
    if (o.dib == 12) return why = "OS/2 BITMAPCOREHEADER", Parse::Unsupported;
    if (n < 14 + uint64_t(o.dib)) return why = "DIB 头被截断", Parse::Bad;
    const int32_t w = int32_t(le32(d + 18)), h = int32_t(le32(d + 22));
    const uint16_t planes = le16(d + 26), bits = le16(d + 28);
    const uint32_t comp = le32(d + 30);
    o.clr_used = le32(d + 46);
    if (w <= 0 || h == 0 || planes != 1) return why = "非法尺寸或 planes", Parse::Bad;
    o.top_down = h < 0;
    o.width = uint32_t(w);
    o.height = h < 0 ? uint32_t(-int64_t(h)) : uint32_t(h);
    o.bits = bits;
    o.compression = comp;
    std::vector<uint32_t> masks;
    if (comp == 3 || comp == 6) {  // BI_BITFIELDS / BI_ALPHABITFIELDS：掩码紧跟 40 字节信息头
        const int nm = (comp == 6 || o.dib >= 56) ? 4 : 3;
        if (n < 54 + 4u * nm) return why = "位掩码被截断", Parse::Bad;
        for (int i = 0; i < nm; ++i) masks.push_back(le32(d + 54 + 4 * i));
    }
    if (comp == 1 || comp == 2) return why = "RLE 压缩 BMP", Parse::Unsupported;
    if (comp == 4 || comp == 5) return why = "内嵌 JPEG/PNG 的 BMP", Parse::Unsupported;
    if (bits != 8 && bits != 24 && bits != 32) return why = std::to_string(bits) + " 位 BMP", Parse::Unsupported;
    const bool std_masks = bits == 32 && masks.size() >= 3 && masks[0] == 0x00FF0000 && masks[1] == 0x0000FF00 &&
                           masks[2] == 0x000000FF;
    if (comp != 0 && !std_masks) return why = "非标准位掩码", Parse::Unsupported;

    o.pal_off = 14 + o.dib;
    if (comp == 3 && o.dib == 40) o.pal_off += 12;
    else if (comp == 6 && o.dib == 40) o.pal_off += 16;
    if (bits == 8) {
        const uint32_t cnt = o.clr_used ? o.clr_used : 256;
        if (cnt > 256 || o.pal_off + 4ull * cnt > o.pixel_offset) return why = "调色板长度与像素偏移冲突", Parse::Bad;
        if (o.pal_off + 4ull * cnt > n) return why = "调色板被截断", Parse::Bad;
        o.palette.assign(d + o.pal_off, d + o.pal_off + 4ull * cnt);
    }
    o.channels = bits / 8;
    o.stride = (uint64_t(o.width) * bits + 31) / 32 * 4;
    if (o.pixel_offset < o.pal_off + o.palette.size()) return why = "像素偏移落在头部或调色板之内", Parse::Bad;
    return Parse::Ok;
}

// ---------------- TIFF ----------------

bool is_tiff(const uint8_t* d, size_t n) {
    if (n < 4) return false;
    return (d[0] == 'I' && d[1] == 'I' && (d[2] == 42 || d[2] == 43) && d[3] == 0) ||
           (d[0] == 'M' && d[1] == 'M' && d[2] == 0 && (d[3] == 42 || d[3] == 43));
}

const char* TiffPage::compression_name() const {
    switch (compression) {
        case 1: return nullptr;
        case 5: return "lzw";
        case 8:
        case 32946: return "zip";
        case 32773: return "packbits";
        default: return "other";
    }
}

namespace {

struct TiffReader {
    const uint8_t* d;
    size_t n;
    bool le;
    bool ok(uint64_t off, uint64_t len) const { return off <= n && len <= n - off; }
    uint64_t u(uint64_t off, int size) const {
        uint64_t v = 0;
        for (int i = 0; i < size; ++i) {
            const uint64_t b = d[off + i];
            v |= le ? b << (8 * i) : b << (8 * (size - 1 - i));
        }
        return v;
    }
};

int type_size(uint16_t t) {
    switch (t) {
        case 1: case 2: case 6: case 7: return 1;
        case 3: case 8: return 2;
        case 4: case 9: case 11: case 13: return 4;
        case 5: case 10: case 12: case 16: case 17: case 18: return 8;
        default: return 0;
    }
}

}  // namespace

bool parse_tiff(const uint8_t* d, size_t n, TiffInfo& out, std::string& why) {
    out = {};
    if (!is_tiff(d, n) || n < 8) return why = "不是 TIFF", false;
    TiffReader r{d, n, d[0] == 'I'};
    out.little = r.le;
    out.bigtiff = r.u(2, 2) == 43;
    uint64_t ifd;
    if (out.bigtiff) {
        if (n < 16 || r.u(4, 2) != 8) return why = "BigTIFF 头无效", false;
        ifd = r.u(8, 8);
    } else {
        ifd = r.u(4, 4);
    }
    const int cnt_size = out.bigtiff ? 8 : 2, ent_size = out.bigtiff ? 20 : 12, off_size = out.bigtiff ? 8 : 4;
    std::set<uint64_t> seen;
    while (ifd && out.pages.size() < 65536) {
        if (!seen.insert(ifd).second) break;  // IFD 环
        if (!r.ok(ifd, uint64_t(cnt_size))) {
            if (out.pages.empty()) return why = "IFD 偏移越界", false;
            break;
        }
        const uint64_t count = r.u(ifd, cnt_size);
        if (count > 100000 || !r.ok(ifd + cnt_size, count * ent_size + off_size)) {
            if (out.pages.empty()) return why = "IFD 越界", false;
            break;
        }
        TiffPage p;
        p.index = uint32_t(out.pages.size());
        std::vector<uint64_t> bits, sfmt, strip_off, strip_cnt, tile_off, tile_cnt;
        bool has_tw = false, has_tl = false, has_photo = false;
        for (uint64_t k = 0; k < count; ++k) {
            const uint64_t e = ifd + cnt_size + k * ent_size;
            const uint16_t tag = uint16_t(r.u(e, 2)), type = uint16_t(r.u(e + 2, 2));
            const uint64_t num = r.u(e + 4, off_size);
            const int ts = type_size(type);
            if (!ts || num == 0) continue;
            if (num > (uint64_t(1) << 32)) return why = "标签项数异常", false;
            const uint64_t total = num * ts;
            uint64_t at = e + 4 + off_size;
            if (total > uint64_t(off_size)) at = r.u(at, off_size);
            auto values = [&](std::vector<uint64_t>& v) -> bool {
                if (!r.ok(at, total)) return false;
                if (ts != 1 && ts != 2 && ts != 4 && ts != 8) return false;
                v.resize(size_t(num));
                for (uint64_t i = 0; i < num; ++i) v[size_t(i)] = r.u(at + i * ts, ts);
                return true;
            };
            auto one = [&](uint64_t& dst) -> bool {
                std::vector<uint64_t> v;
                if (!values(v)) return false;
                dst = v[0];
                return true;
            };
            uint64_t v = 0;
            bool good = true;
            switch (tag) {
                case 256: good = one(p.width); break;
                case 257: good = one(p.height); break;
                case 258: good = values(bits); break;
                case 259: good = one(v); p.compression = int(v); break;
                case 262: good = one(v); p.photometric = int(v); has_photo = true; break;
                case 273: good = values(strip_off); break;
                case 274: good = one(v); p.orientation = int(v); break;
                case 277: good = one(v); p.samples = int(v); break;
                case 278: good = one(p.rowsperstrip); break;
                case 279: good = values(strip_cnt); break;
                case 284: good = one(v); p.planar = int(v); break;
                case 317: good = one(v); p.predictor = int(v); break;
                case 322: good = one(p.tile_w); has_tw = true; break;
                case 323: good = one(p.tile_h); has_tl = true; break;
                case 324: good = values(tile_off); break;
                case 325: good = values(tile_cnt); break;
                case 339: good = values(sfmt); break;
                default: break;
            }
            if (!good) return why = "标签 " + std::to_string(tag) + " 数据越界", false;
        }
        p.bits = bits.empty() ? 1 : int(bits[0]);
        for (auto b : bits)
            if (int(b) != p.bits) p.bits = -1;
        p.sampleformat = sfmt.empty() ? 1 : int(sfmt[0]);
        for (auto s : sfmt)
            if (int(s) != p.sampleformat) p.sampleformat = -1;
        if (!has_photo) p.photometric = p.samples >= 3 ? 2 : 1;
        p.tiled = has_tw && has_tl;
        if (p.tiled) {
            p.offsets = tile_off.empty() ? strip_off : tile_off;
            p.bytecounts = tile_cnt.empty() ? strip_cnt : tile_cnt;
        } else {
            p.offsets = strip_off;
            p.bytecounts = strip_cnt;
        }
        out.pages.push_back(std::move(p));
        ifd = r.u(ifd + cnt_size + count * ent_size, off_size);
    }
    if (out.pages.empty()) return why = "TIFF 没有图像页", false;
    return true;
}

// ---------------- RAW（bench/formats/raw.py）----------------

namespace {

std::vector<PixelFormat> make_formats() {
    std::vector<PixelFormat> v = {
        {"Mono8", 1, 8, nullptr, nullptr},          {"Mono10", 2, 10, nullptr, nullptr},
        {"Mono12", 2, 12, nullptr, nullptr},        {"Mono14", 2, 14, nullptr, nullptr},
        {"Mono16", 2, 16, nullptr, nullptr},        {"Mono10p", 0, 10, nullptr, "Mono10p"},
        {"Mono12p", 0, 12, nullptr, "Mono12p"},     {"Mono12Packed", 0, 12, nullptr, "Mono12Packed"},
    };
    static const char* const keys[4] = {"RG", "GB", "GR", "BG"};
    static const char* const pats[4] = {"RGGB", "GBRG", "GRBG", "BGGR"};
    static std::vector<std::string> names;  // 保持名称字符串的生存期
    names.reserve(64);
    auto add = [&](std::string name, int bpp, int bits, const char* pat, const char* packed) {
        names.push_back(std::move(name));
        v.push_back({names.back().c_str(), bpp, bits, pat, packed});
    };
    for (int i = 0; i < 4; ++i) {
        const std::string k = std::string("Bayer") + keys[i];
        add(k + "8", 1, 8, pats[i], nullptr);
        for (int b : {10, 12, 14, 16}) add(k + std::to_string(b), 2, b, pats[i], nullptr);
        add(k + "10p", 0, 10, pats[i], "Mono10p");
        add(k + "12p", 0, 12, pats[i], "Mono12p");
        add(k + "12Packed", 0, 12, pats[i], "Mono12Packed");
    }
    return v;
}

}  // namespace

const PixelFormat* find_pixel_format(const std::string& name) {
    static const std::vector<PixelFormat> all = make_formats();
    for (const auto& f : all)
        if (name == f.name) return &f;
    return nullptr;
}

uint64_t RawParams::data_len() const {
    if (pf->packed) return packed_size(pf->packed, width * height);
    return row_stride() * height;
}

namespace {

bool json_uint(const nlohmann::json& j, uint64_t& out) {
    if (j.is_number_unsigned()) return out = j.get<uint64_t>(), true;
    if (j.is_number_integer()) {
        const auto v = j.get<int64_t>();
        return v >= 0 ? (out = uint64_t(v), true) : false;
    }
    if (j.is_string()) {
        try {
            size_t pos = 0;
            const auto v = std::stoull(j.get<std::string>(), &pos);
            return pos == j.get<std::string>().size() ? (out = v, true) : false;
        } catch (...) {
            return false;
        }
    }
    return false;
}

}  // namespace

// 一组 RAW 参数（规则或同名参数文件中的一项）
bool parse_raw_params(const nlohmann::json& r, const std::string& where, RawParams& p, std::string& why) {
    p = {};
    if (!r.is_object()) return why = where + "不是对象", false;
    if (!r.contains("width") || !json_uint(r["width"], p.width) || !r.contains("height") ||
        !json_uint(r["height"], p.height) || !p.width || !p.height)
        return why = where + "的 width / height 无效", false;
    if (!r.contains("pixel_format") || !r["pixel_format"].is_string()) return why = where + "缺少 pixel_format", false;
    p.pixel_format = r["pixel_format"].get<std::string>();
    p.pf = find_pixel_format(p.pixel_format);
    if (!p.pf) return why = where + "的像素格式 " + p.pixel_format + " 不认识", false;
    if (r.contains("stride") && !r["stride"].is_null() && !(r["stride"].is_number() && r["stride"] == 0) &&
        !json_uint(r["stride"], p.stride))
        return why = where + "的 stride 无效", false;
    if (r.contains("offset") && !json_uint(r["offset"], p.offset)) return why = where + "的 offset 无效", false;
    if (!p.pf->packed && p.stride && p.stride < p.width * uint64_t(p.pf->bpp))
        return why = where + "的 stride 小于一行像素字节数", false;
    return true;
}

bool parse_raw_rules(const std::string& text, RawRules& out, std::string& why) {
    out = {};
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(text);
    } catch (const nlohmann::json::exception& e) {
        return why = std::string("RAW 配置不是合法 JSON：") + e.what(), false;
    }
    const nlohmann::json* list = &doc;
    if (doc.is_object()) {
        if (!doc.contains("raw")) return true;
        list = &doc["raw"];
    }
    if (!list->is_array()) return why = "RAW 配置的 raw 必须是数组", false;
    for (size_t i = 0; i < list->size(); ++i) {
        const auto& r = (*list)[i];
        const std::string where = "RAW 规则第 " + std::to_string(i + 1) + " 条";
        if (!r.is_object() || !r.contains("match") || !r["match"].is_string()) return why = where + "缺少 match", false;
        RawParams p;
        if (!parse_raw_params(r, where, p, why)) return false;
        p.origin = r["match"].get<std::string>();
        out.rules.emplace_back(p.origin, p);
    }
    return true;
}

std::optional<RawParams> parse_raw_sidecar(const std::string& text) {
    try {
        const auto j = nlohmann::json::parse(text);
        RawParams p;
        std::string why;
        if (j.is_object() && j.contains("width") && j.contains("height") && j.contains("pixel_format") &&
            parse_raw_params(j, "参数文件", p, why)) {
            p.origin = "sidecar";
            return p;
        }
    } catch (const nlohmann::json::exception&) {
    }
    return std::nullopt;  // 与参考实现相同：不合法的参数文件当作没有
}

namespace {

std::u32string to_u32(const std::string& s) {
    std::u32string o;
    for (size_t i = 0; i < s.size();) {
        const uint8_t c = uint8_t(s[i]);
        uint32_t cp = c;
        int extra = 0;
        if (c >= 0xF0) cp = c & 0x07, extra = 3;
        else if (c >= 0xE0) cp = c & 0x0F, extra = 2;
        else if (c >= 0xC0) cp = c & 0x1F, extra = 1;
        ++i;
        for (int k = 0; k < extra && i < s.size(); ++k, ++i) cp = (cp << 6) | (uint8_t(s[i]) & 0x3F);
#ifdef _WIN32
        if (cp >= 'A' && cp <= 'Z') cp += 32;  // Windows 下 fnmatch 先 normcase：不区分大小写、/ 与 \ 等同
        if (cp == '/') cp = '\\';
#endif
        o.push_back(char32_t(cp));
    }
    return o;
}

// fnmatch.translate 的语义：* 任意串（含路径分隔符）、? 单个字符、[...] 字符集（! 取反），整串匹配
bool match_from(const std::u32string& p, size_t pi, const std::u32string& s, size_t si) {
    while (pi < p.size()) {
        const char32_t c = p[pi];
        if (c == U'*') {
            while (pi < p.size() && p[pi] == U'*') ++pi;
            if (pi == p.size()) return true;
            for (size_t k = si; k <= s.size(); ++k)
                if (match_from(p, pi, s, k)) return true;
            return false;
        }
        if (si >= s.size()) return false;
        if (c == U'?') {
            ++pi, ++si;
            continue;
        }
        if (c == U'[') {
            size_t j = pi + 1;
            if (j < p.size() && p[j] == U'!') ++j;
            if (j < p.size() && p[j] == U']') ++j;
            while (j < p.size() && p[j] != U']') ++j;
            if (j < p.size()) {  // 合法字符集 p[pi+1, j)
                size_t k = pi + 1;
                bool neg = false;
                if (p[k] == U'!') neg = true, ++k;
                bool hit = false;
                for (size_t q = k; q < j; ++q) {
                    if (q + 2 < j && p[q + 1] == U'-') {
                        if (p[q] <= s[si] && s[si] <= p[q + 2]) hit = true;
                        q += 2;
                    } else if (p[q] == s[si]) {
                        hit = true;
                    }
                }
                if (hit == neg) return false;
                pi = j + 1, ++si;
                continue;
            }
            // 没有闭合的 ] 时，[ 按字面字符处理
        }
        if (c != s[si]) return false;
        ++pi, ++si;
    }
    return si == s.size();
}

}  // namespace

bool glob_match(const std::string& pattern, const std::string& name) {
    return match_from(to_u32(pattern), 0, to_u32(name), 0);
}

std::optional<RawParams> match_raw_rule(const RawRules& rules, const std::string& rel) {
    for (const auto& [pat, p] : rules.rules)
        if (glob_match(pat, rel)) return p;
    return std::nullopt;
}

// ---------------- 打包格式（bench/layout/packing.py）----------------

namespace {

int packed_bits(const char* f) { return std::strcmp(f, "Mono10p") == 0 ? 10 : 12; }

}  // namespace

uint64_t packed_size(const char* f, uint64_t n) { return (n * packed_bits(f) + 7) / 8; }

bool unpack(const char* f, const uint8_t* data, size_t len, uint64_t n, Samples& out) {
    const uint64_t need = packed_size(f, n);
    if (len < need) return false;
    out.resize(size_t(n));
    if (std::strcmp(f, "Mono12Packed") == 0) {
        for (uint64_t i = 0; i < n; ++i) {
            const uint64_t g = i / 2 * 3;
            auto b = [&](uint64_t k) -> uint32_t { return g + k < need ? data[g + k] : 0; };
            out[size_t(i)] = uint16_t(i % 2 == 0 ? (b(0) << 4) | (b(1) & 0xF) : (b(2) << 4) | (b(1) >> 4));
        }
        return true;
    }
    // Mono10p / Mono12p：LSB 优先的连续位流
    const int bits = packed_bits(f);
    const uint32_t mask = (1u << bits) - 1;
    for (uint64_t i = 0; i < n; ++i) {
        const uint64_t bit = i * bits;
        uint32_t w = 0;
        for (int k = 0; k < 3; ++k) {
            const uint64_t at = bit / 8 + k;
            if (at < need) w |= uint32_t(data[at]) << (8 * k);
        }
        out[size_t(i)] = uint16_t((w >> (bit % 8)) & mask);
    }
    return true;
}

std::vector<uint8_t> pack(const char* f, const uint16_t* v, uint64_t n) {
    const uint64_t size = packed_size(f, n);
    std::vector<uint8_t> out(size_t(size), 0);
    if (std::strcmp(f, "Mono12Packed") == 0) {
        for (uint64_t i = 0; i < n; i += 2) {
            const uint32_t p0 = v[i], p1 = i + 1 < n ? v[i + 1] : 0;
            const uint64_t g = i / 2 * 3;
            const uint8_t b[3] = {uint8_t(p0 >> 4), uint8_t((p0 & 0xF) | ((p1 & 0xF) << 4)), uint8_t(p1 >> 4)};
            for (int k = 0; k < 3; ++k)
                if (g + k < size) out[size_t(g + k)] = b[k];
        }
        return out;
    }
    const int bits = packed_bits(f);
    for (uint64_t i = 0; i < n; ++i) {
        const uint64_t bit = i * bits;
        const uint32_t w = uint32_t(v[i]) << (bit % 8);
        for (int k = 0; k < 3; ++k) {
            const uint64_t at = bit / 8 + k;
            if (at < size) out[size_t(at)] |= uint8_t(w >> (8 * k));
        }
    }
    return out;
}

}  // namespace iis::fmt
