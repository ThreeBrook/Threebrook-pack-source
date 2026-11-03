#include "detect.hpp"
#include "rawguess.hpp"

#include "tiffcodec.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

namespace iis::detect {

namespace {

int bit_length(uint64_t v) {
    int n = 0;
    while (v) ++n, v >>= 1;
    return n;
}

}  // namespace

// ---------------- 统计（bench/detect/stats.py）----------------

ValueStats value_stats(const std::vector<std::pair<const uint16_t*, size_t>>& arrays, int bits_stored) {
    uint32_t vmax = 0, acc = 0;
    for (const auto& [p, n] : arrays) {
        uint32_t m = 0, o = 0;
        for (size_t i = 0; i < n; ++i) {
            m = std::max<uint32_t>(m, p[i]);
            o |= p[i];
        }
        vmax = std::max(vmax, m);
        if (bits_stored > 8) acc |= o;
    }
    ValueStats s;
    s.value_max = vmax;
    if (acc) {
        int tz = 0;
        while (!((acc >> tz) & 1)) ++tz;
        s.shift = std::min(tz, bits_stored - 1);
    }
    s.bits_effective = bit_length(vmax >> s.shift);
    return s;
}

Bayer bayer_detect(const uint16_t* img, uint64_t h, uint64_t w, uint32_t maxval) {
    constexpr double kThreshold = 0.04, kMinAgree = 0.8;
    constexpr uint64_t kGrid = 8;
    Bayer out;
    const uint64_t h2 = h - h % 2, w2 = w - w % 2;
    if (h2 < 8 || w2 < 8) return out;
    const uint64_t th = std::max<uint64_t>(2, h2 / kGrid / 2 * 2), tw = std::max<uint64_t>(2, w2 / kGrid / 2 * 2);
    const double lo = 0.02 * maxval, hi = 0.90 * maxval;
    std::vector<int> votes;
    std::vector<double> mags;
    double sums[4] = {0, 0, 0, 0};
    const double cnt = double((th / 2) * (tw / 2));
    for (uint64_t y = 0; y + th <= h2; y += th) {
        for (uint64_t x = 0; x + tw <= w2; x += tw) {
            int64_t s[4] = {0, 0, 0, 0};
            for (uint64_t yy = 0; yy < th; ++yy) {
                const uint16_t* row = img + (y + yy) * w + x;
                int64_t* sp = s + 2 * (yy & 1);
                for (uint64_t xx = 0; xx < tw; xx += 2) {
                    sp[0] += row[xx];
                    sp[1] += row[xx + 1];
                }
            }
            double m[4];
            for (int k = 0; k < 4; ++k) m[k] = double(s[k]) / cnt;
            const double mean = (((0.0 + m[0]) + m[1]) + m[2]) + m[3];  // numpy 对 4 个元素顺序求和
            const double meanv = mean / 4.0;
            const double mmax = std::max(std::max(m[0], m[1]), std::max(m[2], m[3]));
            if (meanv < lo || mmax > hi) continue;  // 太暗或接近饱和
            const double d = ((m[0] + m[3]) - (m[1] + m[2])) / (2 * meanv);
            for (int k = 0; k < 4; ++k) sums[k] += m[k];
            mags.push_back(std::fabs(d));
            votes.push_back(std::fabs(d) > kThreshold ? (d > 0 ? 1 : (d < 0 ? -1 : 0)) : 0);
        }
    }
    if (votes.size() < 4) return out;
    const double n = double(votes.size());
    const double pos = double(std::count(votes.begin(), votes.end(), 1)) / n;
    const double neg = double(std::count(votes.begin(), votes.end(), -1)) / n;
    const double agree = std::max(pos, neg);
    std::vector<double> sorted = mags;
    std::sort(sorted.begin(), sorted.end());
    const size_t k = sorted.size();
    const double score = k % 2 ? sorted[k / 2] : (sorted[k / 2 - 1] + sorted[k / 2]) / 2.0;
    out.tiles_valid = int(votes.size());
    out.agree = agree;
    out.score = score;
    if (agree < kMinAgree) return out;
    const bool main = pos > neg;
    if (main) {  // G 在 00/11
        out.candidates = {"GRBG", "GBRG"};
        out.guess = sums[1] >= sums[2] ? "GRBG" : "GBRG";
    } else {
        out.candidates = {"RGGB", "BGGR"};
        out.guess = sums[0] >= sums[3] ? "RGGB" : "BGGR";
    }
    out.is_bayer = true;
    out.g_diagonal = main ? "main" : "anti";
    out.confidence = agree * std::min(1.0, score / (2.5 * kThreshold));
    return out;
}

// ---------------- TIFF 页几何与解码 ----------------

bool tiff_geometry(uint64_t width, uint64_t height, int samples, int bits, int planar, bool tiled, uint64_t tile_h,
                   uint64_t tile_w, uint64_t rowsperstrip, TiffPageData& o) {
    if (!width || !height || samples < 1 || (bits != 8 && bits != 16)) return false;
    o.bps = bits / 8;
    o.spp = planar == 1 ? samples : 1;
    const int groups = planar == 1 ? 1 : samples;
    std::vector<TiffSeg> segs;
    if (tiled) {
        if (!tile_h || !tile_w) return false;
        const uint64_t ny = (height + tile_h - 1) / tile_h, nx = (width + tile_w - 1) / tile_w;
        if (ny * nx > (uint64_t(1) << 24)) return false;
        for (uint64_t i = 0; i < ny * nx; ++i) segs.push_back({tile_h, tile_w, i / nx * tile_h, i % nx * tile_w, -1});
        o.canvas_h = ny * tile_h;
        o.canvas_w = nx * tile_w;
    } else {
        const uint64_t rps = std::min(rowsperstrip ? rowsperstrip : height, height);
        const uint64_t ns = (height + rps - 1) / rps;
        if (ns > (uint64_t(1) << 24)) return false;
        for (uint64_t i = 0; i < ns; ++i) segs.push_back({std::min(rps, height - i * rps), width, i * rps, 0, -1});
        o.canvas_h = height;
        o.canvas_w = width;
    }
    o.segs.clear();
    for (int s = 0; s < groups; ++s)
        for (auto g : segs) {
            g.sample = planar == 2 ? s : -1;
            o.segs.push_back(g);
        }
    return true;
}

namespace {

// 解码一页到画布。hard 为真表示数据损坏（识别为 unknown）；split_error 表示能识别但不能安全拆分
bool decode_tiff_page(const uint8_t* d, size_t n, bool little, const fmt::TiffPage& p, TiffPageData& o,
                      std::string& hard) {
    if (!tiff_geometry(p.width, p.height, p.samples, p.bits, p.planar, p.tiled, p.tile_h, p.tile_w, p.rowsperstrip,
                       o)) {
        hard = "第 " + std::to_string(p.index) + " 页几何参数无效";
        return false;
    }
    const uint64_t total = o.canvas_h * o.canvas_w * uint64_t(p.samples);
    if (total > (uint64_t(1) << 32)) {
        hard = "图像过大";
        return false;
    }
    o.canvas.assign(size_t(total), 0);
    if (p.offsets.size() != o.segs.size() || p.bytecounts.size() != o.segs.size()) {
        o.split_error = "第 " + std::to_string(p.index) + " 页像素块数与几何推算不符";
        hard = o.split_error;
        return false;
    }
    if (p.predictor != 1 && p.predictor != 2) o.split_error = "Predictor=" + std::to_string(p.predictor) + " 不支持";
    const char* comp = p.compression_name();
    const bool compressed = comp != nullptr;
    if (compressed) o.raw.resize(o.segs.size());
    o.tails.assign(o.segs.size(), {});
    const uint64_t cw = o.canvas_w, ns = uint64_t(p.samples);
    tiffcodec::Bytes buf;
    for (size_t i = 0; i < o.segs.size(); ++i) {
        const TiffSeg& g = o.segs[i];
        const uint64_t off = p.offsets[i], cnt = p.bytecounts[i];
        if (off > n || cnt > n - off) {
            hard = "第 " + std::to_string(p.index) + " 页像素块越界";
            return false;
        }
        const uint64_t row_bytes = g.cols * uint64_t(o.spp) * uint64_t(o.bps), expect = g.rows * row_bytes;
        const uint8_t* src = d + off;
        uint64_t len = cnt;
        if (compressed) {
            bool ok = false;
            const size_t cap = size_t(expect) + 1;  // 只需知道是否超出几何推算
            if (std::strcmp(comp, "lzw") == 0) ok = tiffcodec::lzw_decode(src, size_t(cnt), cap, buf);
            else if (std::strcmp(comp, "zip") == 0) ok = tiffcodec::zlib_decode(src, size_t(cnt), cap, buf);
            else ok = tiffcodec::packbits_decode(src, size_t(cnt), cap, buf);
            if (!ok && buf.size() != cap) {
                hard = "第 " + std::to_string(p.index) + " 页第 " + std::to_string(i) + " 块解压失败";
                return false;
            }
            if (buf.size() > expect) o.split_error = "解压长度超出几何推算，无法复现";
            src = buf.data();
            len = buf.size();
        }
        if (len < expect) {
            o.split_error = "第 " + std::to_string(p.index) + " 页第 " + std::to_string(i) + " 块数据不足";
        } else if (len > expect && !compressed) {
            o.tails[i].assign(src + expect, src + len);
        }
        const uint64_t use = std::min(len, expect);
        if (compressed) o.raw[i].assign(src, src + use);
        // 样本写入画布（不足部分为 0），差分还原
        const uint64_t spp = uint64_t(o.spp);
        for (uint64_t r = 0; r < g.rows; ++r) {
            std::vector<uint32_t> prev(size_t(spp), 0);
            for (uint64_t c = 0; c < g.cols; ++c) {
                for (uint64_t s = 0; s < spp; ++s) {
                    const uint64_t at = ((r * g.cols + c) * spp + s) * uint64_t(o.bps);
                    uint32_t v = 0;
                    if (at + uint64_t(o.bps) <= use) {
                        if (o.bps == 1) v = src[at];
                        else v = little ? uint32_t(src[at] | src[at + 1] << 8) : uint32_t(src[at] << 8 | src[at + 1]);
                    }
                    if (p.predictor == 2) {
                        v = (v + prev[size_t(s)]) & (o.bps == 1 ? 0xFFu : 0xFFFFu);
                        prev[size_t(s)] = v;
                    }
                    const uint64_t y = g.y + r, x = g.x + c;
                    if (y >= o.canvas_h || x >= cw) continue;
                    const uint64_t ch = g.sample >= 0 ? uint64_t(g.sample) : s;
                    o.canvas[size_t((y * cw + x) * ns + ch)] = uint16_t(v);
                }
            }
        }
    }
    return true;
}

void apply_stats(Detection& r, const std::vector<std::pair<const uint16_t*, size_t>>& arrays, int bits_stored,
                 const uint16_t* bayer_img, uint64_t h, uint64_t w) {
    const ValueStats s = value_stats(arrays, bits_stored);
    r.value_max = s.value_max;
    r.shift = s.shift;
    r.bits_effective = s.bits_effective;
    if (!bayer_img) return;
    std::vector<uint16_t> shifted;
    const uint16_t* img = bayer_img;
    if (r.shift) {
        shifted.assign(bayer_img, bayer_img + h * w);
        for (auto& v : shifted) v = uint16_t(v >> r.shift);
        img = shifted.data();
    }
    const Bayer b = bayer_detect(img, h, w, (1u << (bits_stored - r.shift)) - 1);
    if (b.is_bayer) {
        r.bayer_candidates = b.candidates;
        r.bayer_guess = b.guess;
        r.bayer_confidence = b.confidence;
    }
}

const char* sniff_other(const uint8_t* h, size_t n) {
    auto has = [&](size_t off, const char* s, size_t len) { return n >= off + len && std::memcmp(h + off, s, len) == 0; };
    if (has(0, "\xff\xd8\xff", 3)) return "jpeg";
    if (has(0, "\x89PNG\r\n\x1a\n", 8)) return "png";
    if (has(0, "GIF8", 4)) return "gif";
    if (has(0, "RIFF", 4) && has(8, "WEBP", 4)) return "webp";
    if (has(0, "\0\0\0\x0cjP  \r\n\x87\n", 12) || has(0, "\xff\x4f\xff\x51", 4)) return "jpeg2000";
    if (has(0, "\xff\x0a", 2) || has(0, "\0\0\0\x0cJXL \r\n\x87\n", 12)) return "jpegxl";
    if (has(4, "ftyp", 4)) return "mp4/mov";
    if (has(128, "DICM", 4)) return "dicom";
    if (has(0, "SIMPLE  =", 9)) return "fits";
    if (has(0, "PK\x03\x04", 4)) return "zip";
    if (has(0, "\x1f\x8b", 2)) return "gzip";
    if (has(0, "7z\xbc\xaf\x27\x1c", 6)) return "7z";
    if (has(0, "Rar!", 4)) return "rar";
    if (has(0, "%PDF", 4)) return "pdf";
    if (has(0, "IISCONT\0", 8)) return "iis";
    return nullptr;
}

void detect_bmp(Detection& r, const uint8_t* d, size_t n) {
    r.container = "bmp";
    std::string why;
    const fmt::Parse st = fmt::parse_bmp(d, n, r.bmp, why);
    if (st == fmt::Parse::Unsupported) {
        r.cls = "unsupported";
        r.error = why;
        return;
    }
    if (st != fmt::Parse::Ok) {
        r.cls = "unknown";
        r.damaged = true;
        r.error = why;
        return;
    }
    const fmt::BmpInfo& b = r.bmp;
    r.width = b.width;
    r.height = b.height;
    r.channels = int(b.channels);
    r.bits_stored = 8;
    r.row_order = b.top_down ? "top_down" : "bottom_up";
    r.row_padding = b.stride - b.row_bytes();
    r.trailer_bytes = b.pixel_end() < n ? n - b.pixel_end() : 0;
    if (b.damaged()) {
        r.cls = "unknown";
        r.damaged = true;
        r.trailer_bytes = 0;
        r.error = "像素数据截断";
        return;
    }
    if (b.bits == 8) r.palette = b.gray_palette() ? "gray" : "color";
    // 自上而下的像素（含全部通道）
    const uint64_t rb = b.row_bytes();
    fmt::Samples px(size_t(rb * b.height));
    for (uint64_t y = 0; y < b.height; ++y) {
        const uint64_t fy = b.top_down ? y : b.height - 1 - y;
        const uint8_t* row = d + b.pixel_offset + fy * b.stride;
        for (uint64_t i = 0; i < rb; ++i) px[size_t(y * rb + i)] = row[i];
    }
    const bool gray = b.channels == 1 && r.palette == "gray";
    apply_stats(r, {{px.data(), px.size()}}, 8, gray ? px.data() : nullptr, b.height, b.width);
    r.cls = r.bayer_candidates.empty() ? "bmp" : "bayer";
    r.processable = true;
}

void detect_tiff(Detection& r, const uint8_t* d, size_t n) {
    r.container = "tiff";
    std::string why;
    if (!fmt::parse_tiff(d, n, r.tiff, why)) {
        r.cls = "unknown";
        r.damaged = true;
        r.error = why;
        return;
    }
    const auto& pages = r.tiff.pages;
    const fmt::TiffPage& p0 = pages[0];
    r.byteorder = r.tiff.little ? "II" : "MM";
    r.pages = uint32_t(pages.size());
    r.width = p0.width;
    r.height = p0.height;
    r.channels = p0.samples;
    r.bits_stored = p0.bits;
    r.predictor = p0.predictor;
    r.row_order = p0.orientation == 1 ? "top_down" : "orientation=" + std::to_string(p0.orientation);
    std::set<std::string> comps;
    bool other = false;
    for (const auto& p : pages) {
        const char* c = p.compression_name();
        if (c && std::strcmp(c, "other") == 0) other = true;
        else if (c) comps.insert(c);
    }
    std::string reason;
    if (other) reason = "压缩方式不在复现式还原范围";
    else if (std::any_of(pages.begin(), pages.end(), [](const auto& p) { return p.sampleformat != 1; }))
        reason = "非无符号整数采样（浮点 / 有符号）";
    else if (std::any_of(pages.begin(), pages.end(), [](const auto& p) { return p.bits != 8 && p.bits != 16; }))
        reason = "位深 " + std::to_string(p0.bits) + " 不在 8 / 16 位范围";
    if (!reason.empty()) {
        r.cls = "unsupported";
        r.error = reason;
        return;
    }
    for (const auto& c : comps) r.compression += (r.compression.empty() ? "" : "+") + c;
    r.tiff_pages.resize(pages.size());
    std::vector<fmt::Samples> crops(pages.size());
    std::vector<std::pair<const uint16_t*, size_t>> arrays;
    for (size_t i = 0; i < pages.size(); ++i) {
        std::string hard;
        if (!decode_tiff_page(d, n, r.tiff.little, pages[i], r.tiff_pages[i], hard)) {
            r.cls = "unknown";
            r.damaged = true;
            r.error = "像素解码失败：" + hard;
            r.tiff_pages.clear();
            return;
        }
        const TiffPageData& pd = r.tiff_pages[i];
        const uint64_t s = uint64_t(pages[i].samples), w = pages[i].width, h = pages[i].height;
        if (pd.canvas_w == w && pd.canvas_h == h) {
            arrays.push_back({pd.canvas.data(), pd.canvas.size()});
        } else {  // 图块边缘填充区不计入统计
            crops[i].resize(size_t(w * h * s));
            for (uint64_t y = 0; y < h; ++y)
                std::memcpy(&crops[i][size_t(y * w * s)], &pd.canvas[size_t(y * pd.canvas_w * s)], size_t(w * s * 2));
            arrays.push_back({crops[i].data(), crops[i].size()});
        }
    }
    const bool single = p0.samples == 1 && (p0.photometric == 0 || p0.photometric == 1);
    apply_stats(r, arrays, p0.bits, single ? arrays[0].first : nullptr, p0.height, p0.width);
    if (!comps.empty()) r.cls = "tiff_compressed";
    else r.cls = r.bayer_candidates.empty() ? "tiff_uncompressed" : "bayer";
    r.processable = true;
}

void detect_raw(Detection& r, const uint8_t* d, size_t n, const fmt::RawParams& p) {
    r.container = "raw";
    r.raw = p;
    r.width = p.width;
    r.height = p.height;
    r.channels = 1;
    r.bits_stored = p.bits_stored();
    r.pixel_format = p.pixel_format;
    r.byteorder = r.bits_stored == 8 ? "" : "II";
    r.row_order = "top_down";
    r.row_padding = p.pf->packed ? 0 : p.row_stride() - p.width * uint64_t(p.pf->bpp);
    const uint64_t need = p.offset + p.data_len();
    if (n < need || p.width * p.height > (uint64_t(1) << 32)) {
        r.cls = "raw";
        r.error = "参数与文件不符：文件 " + std::to_string(n) + " 字节 < 参数要求 " + std::to_string(need) + " 字节";
        return;
    }
    const uint8_t* body = d + p.offset;
    const uint64_t px = p.width * p.height;
    if (p.pf->packed) {
        fmt::unpack(p.pf->packed, body, size_t(p.data_len()), px, r.raw_img);
    } else {
        r.raw_img.resize(size_t(px));
        const uint64_t stride = p.row_stride();
        for (uint64_t y = 0; y < p.height; ++y) {
            const uint8_t* row = body + y * stride;
            for (uint64_t x = 0; x < p.width; ++x)
                r.raw_img[size_t(y * p.width + x)] =
                    p.pf->bpp == 1 ? row[x] : uint16_t(row[2 * x] | row[2 * x + 1] << 8);
        }
    }
    r.trailer_bytes = n - need;
    apply_stats(r, {{r.raw_img.data(), r.raw_img.size()}}, p.pf->bpp == 2 ? 16 : r.bits_stored, r.raw_img.data(),
                p.height, p.width);
    if (p.pf->bayer) {
        r.bayer_config = p.pf->bayer;
        r.bayer_candidates = {p.pf->bayer};
        r.bayer_guess = p.pf->bayer;
    }
    r.cls = r.bayer_candidates.empty() ? "raw" : "bayer";
    r.processable = true;
}

}  // namespace

Detection detect(const uint8_t* d, size_t n, const std::string& rel_path, const fmt::RawRules* rules,
                 const std::string& sidecar_json, bool guess_raw) {
    Detection r;
    if (!n) {
        r.cls = "unknown";
        return r;
    }
    if (fmt::is_bmp(d, std::min<size_t>(n, 256))) {
        detect_bmp(r, d, n);
    } else if (fmt::is_tiff(d, n)) {
        detect_tiff(r, d, n);
    } else if (const char* other = sniff_other(d, std::min<size_t>(n, 256))) {
        r.cls = "unsupported";
        r.container = other;
    } else if (auto p = rules ? fmt::match_raw_rule(*rules, rel_path) : std::nullopt) {
        detect_raw(r, d, n, *p);
    } else if (auto q = sidecar_json.empty() ? std::nullopt : fmt::parse_raw_sidecar(sidecar_json)) {
        detect_raw(r, d, n, *q);
    } else if (auto g = guess_raw ? rawguess::guess(d, n) : std::nullopt) {
        detect_raw(r, d, n, *g);  // 自动识别的参数（origin = "auto"）
    } else {
        r.cls = "unknown";  // 没有 RAW 配置的二进制：不猜参数，按兜底处理
    }
    return r;
}

std::string to_json(const Detection& r) {
    using nlohmann::json;
    auto str = [](const std::string& s) -> json { return s.empty() ? json(nullptr) : json(s); };
    json j = {
        {"class", r.cls}, {"container", str(r.container)}, {"processable", r.processable}, {"damaged", r.damaged},
        {"error", str(r.error)},
    };
    if (r.container == "bmp" || r.container == "tiff" || r.container == "raw") {
        if (r.width) {
            j["width"] = r.width;
            j["height"] = r.height;
            j["channels"] = r.channels;
            j["bits_stored"] = r.bits_stored;
        }
        if (r.processable) {
            j["bits_effective"] = r.bits_effective;
            j["value_max"] = r.value_max;
            j["shift"] = r.shift;
            j["bayer_candidates"] = r.bayer_candidates;
            j["bayer_guess"] = str(r.bayer_guess);
            j["bayer_confidence"] = r.bayer_candidates.empty() ? json(nullptr) : json(std::round(r.bayer_confidence * 1000) / 1000);
        }
        j["byteorder"] = str(r.byteorder);
        j["pages"] = r.pages;
        j["compression"] = str(r.compression);
        j["predictor"] = r.container == "tiff" ? json(r.predictor) : json(nullptr);
        j["palette"] = str(r.palette);
        j["row_order"] = str(r.row_order);
        j["row_padding"] = r.row_padding;
        j["trailer_bytes"] = r.trailer_bytes;
        j["pixel_format"] = str(r.pixel_format);
    }
    return j.dump();
}

}  // namespace iis::detect
