// iis-decode：IIS 容器 v1 的独立开源解码器（BSD-3-Clause，见 ../LICENSE）。
//
// 按公开规格（spec/container-v1.md）把 .iis 还原为与原文件逐字节相同的文件，并用 SHA-256 核对。
// 只依赖标准编码库：CharLS（JPEG-LS）、zstd、zlib、libdeflate，以及 nlohmann/json（配方解析）。
// 与 IIS 设备 / 服务的实现相互独立：客户不依赖我方设备或软件也能取回自己的数据。
//
// 结构：
//   1. 基础：小端读取、CRC32C、SHA-256、LEB128
//   2. 容器：文件头、文件尾、段表、段（CRC 校验后按编码解码）
//   3. 版面重组：BMP / RAW（含打包格式）/ TIFF（条带、图块、多页、Predictor 2、LZW / ZIP / PackBits 复现编码）
//   4. 还原：原样字节 + 重组像素，检查每个字节恰好覆盖一次，SHA-256 比对
#include "iis_decode.hpp"

#include <charls/charls.h>
#include <libdeflate.h>
#include <nlohmann/json.hpp>
#include <zlib.h>
#include <zstd.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace iis_decode {

using json = nlohmann::json;
using Bytes = std::vector<uint8_t>;
using Samples = std::vector<uint16_t>;

namespace {

struct Bad : std::runtime_error {
    using std::runtime_error::runtime_error;
};
void need(bool ok, const std::string& what) {
    if (!ok) throw Bad(what);
}

// ======================= 1. 基础 =======================

uint16_t g16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t g32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t g64(const uint8_t* p) { return uint64_t(g32(p)) | uint64_t(g32(p + 4)) << 32; }

// CRC32C（Castagnoli，反射多项式 0x82F63B78），首次 crc 传 0
uint32_t crc32c(uint32_t crc, const uint8_t* p, size_t n) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? (c >> 1) ^ 0x82F63B78u : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    while (n--) crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

// SHA-256（FIPS 180-4）
void sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
        0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
        0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
        0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    auto block = [&](const uint8_t* p) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
    };
    size_t i = 0;
    for (; i + 64 <= len; i += 64) block(data + i);
    uint8_t tail[128] = {};
    const size_t rest = len - i;
    std::memcpy(tail, data + i, rest);
    tail[rest] = 0x80;
    const size_t tl = rest < 56 ? 64 : 128;
    const uint64_t bits = uint64_t(len) * 8;
    for (int k = 0; k < 8; ++k) tail[tl - 1 - k] = uint8_t(bits >> (8 * k));
    block(tail);
    if (tl == 128) block(tail + 64);
    for (int k = 0; k < 8; ++k)
        for (int b = 0; b < 4; ++b) out[4 * k + b] = uint8_t(h[k] >> (24 - 8 * b));
}

bool leb(const uint8_t*& p, const uint8_t* end, uint64_t& v) {
    v = 0;
    for (int shift = 0; p < end && shift < 64; shift += 7) {
        const uint8_t b = *p++;
        v |= uint64_t(b & 0x7F) << shift;
        if (!(b & 0x80)) return true;
    }
    return false;
}

// ======================= 2. 容器 =======================

enum : uint16_t { RECIPE = 1, SKELETON = 2, PLANE = 3, EXTRA = 4, META = 5 };
enum : uint16_t { C_NONE = 0, C_ZSTD = 1, C_JPEGLS = 2 };
constexpr uint64_t kMaxOrig = 8ull << 30;

struct Seg {
    uint64_t offset, raw_len, data_len;
    uint32_t meta_len, data_crc;
    uint16_t type, codec, flags, id;
};

struct Container {
    const uint8_t* d = nullptr;
    size_t n = 0;
    uint64_t orig_size = 0;
    uint8_t sha[32] = {};
    std::vector<Seg> segs;

    void open() {
        need(n >= 72 + 16 + 4 + 32, "文件过短，不是完整的 IIS 容器");
        need(std::memcmp(d, "IISCONT\0", 8) == 0, "不是 IIS 容器（魔数不符）");
        const uint16_t major = g16(d + 8);
        const uint32_t hsize = g32(d + 12);
        need(major == 1, "只支持容器主版本 1（本文件为 " + std::to_string(major) + "）");
        need(hsize >= 72 && hsize <= 4096 && hsize <= n, "文件头长度无效");
        need(g32(d + hsize - 4) == crc32c(0, d, hsize - 4), "文件头校验失败");
        need(g32(d + 16) == 0, "使用了本解码器不支持的不兼容特性");
        orig_size = g64(d + 24);
        std::memcpy(sha, d + 32, 32);
        need(orig_size <= kMaxOrig, "原文件过大");
        const uint8_t* t = d + n - 32;
        need(std::memcmp(t + 28, "IEND", 4) == 0, "文件尾缺失（文件可能被截断）");
        need(g32(t + 24) == crc32c(0, t, 24), "文件尾校验失败");
        const uint64_t ioff = g64(t), isize = g64(t + 8);
        need(g64(t + 16) == n, "容器长度与记录不符");
        need(ioff >= hsize && isize >= 20 && ioff + isize + 32 == n, "段表位置无效");
        const uint8_t* idx = d + ioff;
        need(std::memcmp(idx, "IIDX", 4) == 0 && g32(idx + isize - 4) == crc32c(0, idx, isize - 4), "段表损坏");
        const uint32_t count = g32(idx + 4), esize = g32(idx + 8);
        need(esize >= 40 && esize <= 4096 && isize == 16 + uint64_t(esize) * count + 4, "段表格式无效");
        uint64_t pos = hsize;
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* e = idx + 16 + size_t(esize) * i;
            Seg s{g64(e), g64(e + 24), g64(e + 32), g32(e + 16), g32(e + 20), g16(e + 8), g16(e + 10), g16(e + 12), g16(e + 14)};
            need(s.offset == pos && s.data_len <= ioff && 40 + uint64_t(s.meta_len) <= ioff - s.data_len &&
                     pos <= ioff - 40 - s.meta_len - s.data_len,
                 "第 " + std::to_string(i) + " 段位置无效");
            pos += 40 + s.meta_len + s.data_len;
            // 不认识的关键段：不能还原；不认识的非关键段：跳过
            need(s.type <= META || !(s.flags & 1), "含本解码器不认识的关键段（类型 " + std::to_string(s.type) + "）");
            segs.push_back(s);
        }
        need(pos == ioff, "段与段表不连续");
    }

    // 段的原样内容（校验段头与 CRC32C）
    void raw(const Seg& s, Bytes* meta, Bytes& data) const {
        const uint8_t* h = d + s.offset;
        need(std::memcmp(h, "ISEG", 4) == 0 && g32(h + 36) == crc32c(0, h, 36), "段头损坏");
        need(g16(h + 4) == s.type && g16(h + 6) == s.codec && g16(h + 10) == s.id && g32(h + 12) == s.meta_len &&
                 g64(h + 16) == s.raw_len && g64(h + 24) == s.data_len && g32(h + 32) == s.data_crc,
             "段头与段表不一致");
        const uint8_t* m = h + 40;
        const uint8_t* p = m + s.meta_len;
        need(crc32c(crc32c(0, m, s.meta_len), p, size_t(s.data_len)) == s.data_crc, "段数据校验失败（CRC32C 不符）");
        if (meta) meta->assign(m, m + s.meta_len);
        data.assign(p, p + s.data_len);
    }

    // 按编码解码：none / zstd → 原字节；jpegls → 像素（≤8 位每样本 1 字节，否则 2 字节小端，按像素交织）
    Bytes decode(const Seg& s, Bytes* meta = nullptr) const {
        Bytes data;
        raw(s, meta, data);
        need(s.raw_len <= kMaxOrig * 2, "段长度过大");
        if (s.codec == C_NONE) {
            need(data.size() == s.raw_len, "段长度不符");
            return data;
        }
        if (s.codec == C_ZSTD) {
            Bytes out(size_t(s.raw_len));
            const size_t got = ZSTD_decompress(out.data(), out.size(), data.data(), data.size());
            need(!ZSTD_isError(got) && got == s.raw_len, "zstd 解压失败");
            return out;
        }
        if (s.codec == C_JPEGLS) {
            try {
                charls::jpegls_decoder dec(data.data(), data.size(), true);
                need(dec.near_lossless() == 0, "有损 JPEG-LS 码流");
                const auto fi = dec.frame_info();
                const size_t bps = fi.bits_per_sample <= 8 ? 1 : 2;
                const size_t bytes = size_t(fi.width) * fi.height * size_t(fi.component_count) * bps;
                need(bytes == s.raw_len, "JPEG-LS 解码长度与记录不符");
                Bytes out(bytes);
                if (fi.component_count > 1 && dec.interleave_mode() == charls::interleave_mode::none) {
                    Bytes tmp(bytes);  // 非交织码流：按平面解出后转为按像素交织
                    dec.decode(tmp.data(), tmp.size());
                    const size_t plane = size_t(fi.width) * fi.height;
                    for (size_t c = 0; c < size_t(fi.component_count); ++c)
                        for (size_t i = 0; i < plane; ++i)
                            std::memcpy(&out[(i * fi.component_count + c) * bps], &tmp[(c * plane + i) * bps], bps);
                } else {
                    dec.decode(out.data(), out.size());
                }
                return out;
            } catch (const charls::jpegls_error& e) {
                throw Bad(std::string("JPEG-LS 解码失败：") + e.what());
            }
        }
        throw Bad("不支持的编码 " + std::to_string(s.codec) + "（本版本支持 none / zstd / jpegls）");
    }

    const Seg* find(uint16_t type, uint16_t id) const {
        for (const auto& s : segs)
            if (s.type == type && s.id == id) return &s;
        return nullptr;
    }
};

// ======================= 3. TIFF 复现编码器 =======================
// 已压缩 TIFF 的像素块按配方记录的编码器重新编码，必须与原文件逐字节相同（复现式还原）。

// 精确 LZW 字典：键 < 2^20
struct Dict {
    std::vector<int32_t> v = std::vector<int32_t>(size_t(1) << 20, -1);
    std::vector<uint32_t> used;
    int find(uint32_t k) const { return v[k]; }
    void put(uint32_t k, uint32_t code) {
        v[k] = int32_t(code);
        used.push_back(k);
    }
    void clear() {
        for (uint32_t k : used) v[k] = -1;
        used.clear();
    }
};

// libtiff tif_lzw.c 的编码行为
Bytes lzw_libtiff(const uint8_t* data, size_t n) {
    constexpr uint32_t kClear = 256, kEoi = 257, kFirst = 258, kCodeMax = 4095;
    constexpr uint64_t kGap = 10000;
    Bytes out;
    uint32_t nextdata = 0, maxcode = 511;
    int nextbits = 0, nbits = 9;
    uint64_t outcount = 0;
    auto put = [&](uint32_t code) {
        nextdata = (nextdata << nbits) | code;
        nextbits += nbits;
        out.push_back(uint8_t(nextdata >> (nextbits - 8)));
        nextbits -= 8;
        if (nextbits >= 8) {
            out.push_back(uint8_t(nextdata >> (nextbits - 8)));
            nextbits -= 8;
        }
        outcount += uint64_t(nbits);
    };
    if (n) {
        Dict table;
        uint32_t free_ent = kFirst;
        uint64_t checkpoint = kGap, ratio = 0, incount = 1;
        put(kClear);
        uint32_t ent = data[0];
        for (size_t i = 1; i < n; ++i) {
            const uint32_t c = data[i];
            ++incount;
            const uint32_t key = (c << 12) | ent;
            const int code = table.find(key);
            if (code >= 0) {
                ent = uint32_t(code);
                continue;
            }
            put(ent);
            ent = c;
            table.put(key, free_ent++);
            bool reset = false;
            if (free_ent == kCodeMax - 1) {
                reset = true;
            } else if (free_ent > maxcode) {
                ++nbits;
                maxcode = (1u << nbits) - 1;
            } else if (incount >= checkpoint) {
                checkpoint = incount + kGap;
                uint64_t rat;
                if (incount > 0x007FFFFF) {
                    const uint64_t r = outcount >> 8;
                    rat = r == 0 ? 0x7FFFFFFF : incount / r;
                } else {
                    rat = (incount << 8) / outcount;
                }
                if (rat <= ratio) reset = true;
                else ratio = rat;
            }
            if (reset) {
                table.clear();
                ratio = 0;
                incount = outcount = 0;
                free_ent = kFirst;
                put(kClear);
                nbits = 9;
                maxcode = 511;
            }
        }
        put(ent);
        ++free_ent;
        if (free_ent == kCodeMax - 1) {
            outcount = 0;
            put(kClear);
            nbits = 9;
        } else if (free_ent > maxcode) {
            ++nbits;
        }
    }
    put(kEoi);
    if (nextbits > 0) out.push_back(uint8_t(nextdata << (8 - nextbits)));
    return out;
}

// imagecodecs imcd.c 的 LZW 编码（tifffile 写出的 LZW；算法 © Christoph Gohlke，BSD-3-Clause）
Bytes lzw_imcd(const uint8_t* src, size_t n) {
    constexpr uint32_t kClear = 256, kEoi = 257, kFirst = 258;
    constexpr int64_t kGap = 10000;
    Bytes dst;
    uint32_t dstbyte = kClear;
    int bitw = 9, bitc = 1;
    auto write = [&] { dst.push_back(uint8_t(dstbyte >> bitc)); };
    write();
    if (n < 1) {
        dstbyte = ((dstbyte << bitw) | kEoi) << 8;
        bitc += bitw;
        write();
        bitc -= 8;
        write();
        return dst;
    }
    Dict table;
    uint32_t nextcode = kFirst, omega = src[0];
    int64_t incount_base = 0, outcount_base = 1, checkpoint = kGap, enc_ratio = 0;
    auto put_code = [&](uint32_t code) {
        dstbyte = (dstbyte << bitw) | code;
        bitc += bitw - 8;
        write();
        if (bitc >= 8) {
            bitc -= 8;
            write();
        }
    };
    auto reset = [&](int64_t at) {
        table.clear();
        nextcode = kFirst;
        bitw = 9;
        incount_base = at;
        outcount_base = int64_t(dst.size());
        enc_ratio = 0;
    };
    for (int64_t i = 1; i < int64_t(n); ++i) {
        const uint32_t k = src[i];
        const uint32_t key = (omega << 8) | k;
        const int found = table.find(key);
        if (found >= 0) {
            omega = uint32_t(found);
            continue;
        }
        table.put(key, nextcode++);
        put_code(omega);
        omega = k;
        if (nextcode == 4096) {
            put_code(kClear);
            reset(i);
            checkpoint = i + kGap;
        } else {
            if (nextcode == 512) bitw = 10;
            else if (nextcode == 1024) bitw = 11;
            else if (nextcode == 2048) bitw = 12;
            if (i >= checkpoint) {
                const int64_t incount = i - incount_base, outcount = int64_t(dst.size()) - outcount_base;
                checkpoint = i + kGap;
                if (outcount > 0) {
                    int64_t ratio;
                    if (incount > 0x007fffff) {
                        ratio = outcount >> 8;
                        ratio = ratio == 0 ? 0x7fffffff : incount / ratio;
                    } else {
                        ratio = (incount << 8) / outcount;
                    }
                    if (ratio <= enc_ratio) {
                        put_code(kClear);
                        reset(i);
                    } else {
                        enc_ratio = ratio;
                    }
                }
            }
        }
    }
    put_code(omega);
    if (nextcode == 511) bitw = 10;
    else if (nextcode == 1023) bitw = 11;
    else if (nextcode == 2047) bitw = 12;
    dstbyte = ((dstbyte << bitw) | kEoi) << 8;
    bitc += bitw;
    write();
    if (bitc >= 8) {
        bitc -= 8;
        write();
        if (bitc >= 8) {
            bitc -= 8;
            write();
        }
    }
    return dst;
}

// imagecodecs imcd.c 的 PackBits 编码（© Christoph Gohlke，BSD-3-Clause）
Bytes packbits(const uint8_t* src, size_t srcsize) {
    Bytes dst;
    if (!srcsize) return dst;
    const size_t maxdst = srcsize + (srcsize + 127) / 128, dstend = maxdst - 1;
    dst.resize(maxdst);
    size_t s = 0, d = 0;
    auto next_rep = [&](size_t p, size_t end) -> size_t {
        while (++p < end)
            if (src[p - 1] == src[p]) return p - 1;
        return SIZE_MAX;
    };
    auto rep_len = [&](size_t p, size_t end) -> size_t {
        const uint8_t v = p < end ? src[p] : 0;
        size_t q = p;
        while (++q < end && src[q] == v) {
        }
        return q - p;
    };
    bool overflow = false;
    while (s < srcsize) {
        const size_t lkend = s + 129 < srcsize ? s + 129 : srcsize;
        size_t dup = next_rep(s, lkend);
        if (dup == s) {
            const size_t rep = std::min<size_t>(128, rep_len(dup, srcsize));
            if (d >= dstend) {
                overflow = true;
                break;
            }
            dst[d++] = uint8_t(257 - int(rep));
            dst[d++] = src[s];
            s += rep;
            continue;
        }
        size_t literal;
        if (dup == SIZE_MAX) {
            literal = srcsize - s;
        } else {
            const size_t rep = rep_len(dup, srcsize);
            if (rep < 3) {
                const size_t nextsrc = dup + rep, nextdup = next_rep(nextsrc, lkend);
                if (nextdup != SIZE_MAX && nextdup > nextsrc) dup = nextdup;
            }
            literal = dup - s;
        }
        literal = std::min<size_t>(128, literal);
        if (d + literal >= dstend) {
            overflow = true;
            break;
        }
        dst[d++] = uint8_t(literal - 1);
        std::memcpy(&dst[d], src + s, literal);
        d += literal;
        s += literal;
    }
    if (overflow) {
        s = d = 0;
        while (s < srcsize) {
            const size_t literal = std::min<size_t>(128, srcsize - s);
            dst[d++] = uint8_t(literal - 1);
            std::memcpy(&dst[d], src + s, literal);
            d += literal;
            s += literal;
        }
    }
    dst.resize(d);
    return dst;
}

Bytes zlib_compress(const uint8_t* src, size_t n, int level) {
    uLongf cap = compressBound(uLong(n));
    Bytes out(cap);
    need(compress2(out.data(), &cap, src, uLong(n), level) == Z_OK, "zlib 压缩失败");
    out.resize(cap);
    return out;
}

Bytes libdeflate_compress(const uint8_t* src, size_t n, int level) {
    libdeflate_compressor* c = libdeflate_alloc_compressor(level);
    need(c != nullptr, "libdeflate 级别无效");
    Bytes out(libdeflate_zlib_compress_bound(c, n));
    out.resize(libdeflate_zlib_compress(c, src, n, out.data(), out.size()));
    libdeflate_free_compressor(c);
    return out;
}

Bytes encode_block(const json& enc, char variant, const Bytes& raw, size_t row_bytes) {
    const std::string name = enc.at("name").get<std::string>();
    const uint8_t* p = raw.data();
    const size_t n = raw.size();
    if (name == "imcd-lzw" || (name == "lzw" && variant == 'I')) return lzw_imcd(p, n);
    if (name == "libtiff-lzw" || (name == "lzw" && variant == 'L')) return lzw_libtiff(p, n);
    if (name == "zlib") return zlib_compress(p, n, enc.at("level").get<int>());
    if (name == "libdeflate") return libdeflate_compress(p, n, enc.at("level").get<int>());
    if (name == "packbits") return packbits(p, n);
    if (name == "packbits-rows") {
        need(row_bytes > 0, "行字节数无效");
        Bytes out;
        for (size_t i = 0; i < n; i += row_bytes) {
            const Bytes r = packbits(p + i, std::min(row_bytes, n - i));
            out.insert(out.end(), r.begin(), r.end());
        }
        return out;
    }
    throw Bad("未知的复现编码器 " + name);
}

// ======================= 4. 版面重组 =======================

struct Range {
    uint64_t offset, length;
};

uint64_t u(const json& j, const char* k) {
    const json& v = j.at(k);
    need(v.is_number_unsigned() || (v.is_number_integer() && v.get<int64_t>() >= 0), std::string("字段 ") + k + " 无效");
    return v.get<uint64_t>();
}

struct Rebuild {
    const Container& c;
    const json& recipe;
    Bytes& out;
    std::vector<Range>& covered;

    Samples plane(const json& pm) const {
        const Seg* s = c.find(PLANE, uint16_t(u(pm, "id")));
        need(s, "缺少像素平面段");
        const Bytes b = c.decode(*s);
        const uint64_t cnt = u(pm, "width") * u(pm, "height") * u(pm, "components"), bits = u(pm, "bits");
        need(bits >= 1 && bits <= 16 && b.size() == cnt * (bits <= 8 ? 1 : 2), "像素平面长度不符");
        Samples v(static_cast<size_t>(cnt));
        for (size_t i = 0; i < v.size(); ++i) v[i] = bits <= 8 ? b[i] : uint16_t(b[2 * i] | b[2 * i + 1] << 8);
        return v;
    }

    Bytes extra(const std::string& role, uint64_t image, int64_t block = -1, bool required = true) const {
        for (const auto& e : recipe.at("extras"))
            if (e.at("role") == role && u(e, "image") == image &&
                (block < 0 || (e.contains("block") && u(e, "block") == uint64_t(block)))) {
                const Seg* s = c.find(EXTRA, uint16_t(u(e, "id")));
                need(s, "缺少附加字节段");
                return c.decode(*s);
            }
        need(!required, "缺少附加字节 " + role);
        return {};
    }
    bool has_extra(const std::string& role, uint64_t image, int64_t block) const {
        for (const auto& e : recipe.at("extras"))
            if (e.at("role") == role && u(e, "image") == image && e.contains("block") && u(e, "block") == uint64_t(block))
                return true;
        return false;
    }

    const json& role_plane(uint64_t image, const char* role) const {
        for (const auto& p : recipe.at("planes"))
            if (u(p, "image") == image && p.at("role") == role) return p;
        throw Bad(std::string("缺少平面 ") + role);
    }

    // 单通道：一个平面，或四个 Bayer 相位平面（原图 (y,x) 来自相位 [y%2, x%2] 的 (y/2, x/2)），再左移 shift
    Samples gray(uint64_t image, uint64_t h, uint64_t w, int sample = -1) const {
        std::vector<const json*> ps;
        for (const auto& p : recipe.at("planes")) {
            if (u(p, "image") != image) continue;
            const std::string role = p.at("role");
            if (role == "gray" || role == "index" || (role == "sample" && sample >= 0 && u(p, "sample") == uint64_t(sample)))
                ps.push_back(&p);
        }
        need(!ps.empty(), "缺少灰度平面");
        const uint64_t shift = u(*ps[0], "shift");
        need(shift < 16, "移位无效");
        Samples img(size_t(h * w));
        if (ps.size() == 1 && ps[0]->at("bayer_phase").is_null()) {
            need(u(*ps[0], "width") == w && u(*ps[0], "height") == h && u(*ps[0], "components") == 1, "平面尺寸不符");
            img = plane(*ps[0]);
        } else {
            need(ps.size() == 4, "Bayer 平面数不是 4");
            for (const json* p : ps) {
                const auto& ph = p->at("bayer_phase");
                need(ph.is_array() && ph.size() == 2, "Bayer 相位无效");
                const uint64_t dy = ph[0].get<uint64_t>(), dx = ph[1].get<uint64_t>();
                const uint64_t ph_h = (h - dy + 1) / 2, ph_w = (w - dx + 1) / 2;
                need(dy < 2 && dx < 2 && u(*p, "shift") == shift && u(*p, "height") == ph_h && u(*p, "width") == ph_w,
                     "Bayer 平面尺寸不符");
                const Samples v = plane(*p);
                for (uint64_t y = 0; y < ph_h; ++y)
                    for (uint64_t x = 0; x < ph_w; ++x) img[size_t((2 * y + dy) * w + 2 * x + dx)] = v[size_t(y * ph_w + x)];
            }
        }
        if (shift)
            for (auto& v : img) v = uint16_t(v << shift);
        return img;
    }

    void put(uint64_t off, const Bytes& b) {
        need(off <= out.size() && b.size() <= out.size() - off, "像素区超出原文件");
        if (b.empty()) return;
        std::memcpy(&out[size_t(off)], b.data(), b.size());
        covered.push_back({off, b.size()});
    }

    void bmp() {
        const json& L = recipe.at("layout");
        const uint64_t w = u(L, "width"), h = u(L, "height"), ch = u(L, "channels"), stride = u(L, "stride"), off = u(L, "pixel_offset");
        need((ch == 1 || ch == 3 || ch == 4) && w && h && stride >= w * ch && h * stride <= out.size(), "BMP 版面参数无效");
        const bool top_down = L.at("top_down").get<bool>();
        const uint64_t rb = w * ch;
        Bytes rows(size_t(h * stride), 0);
        if (!L.at("padding_zero").get<bool>()) {
            const Bytes pad = extra("row_padding", 0);
            need(pad.size() == h * (stride - rb), "行填充长度不符");
            for (uint64_t y = 0; y < h; ++y) std::memcpy(&rows[size_t(y * stride + rb)], &pad[size_t(y * (stride - rb))], size_t(stride - rb));
        }
        auto row = [&](uint64_t y) { return &rows[size_t((top_down ? y : h - 1 - y) * stride)]; };
        if (ch == 1) {
            const Samples img = gray(0, h, w);
            for (uint64_t y = 0; y < h; ++y)
                for (uint64_t x = 0; x < w; ++x) row(y)[x] = uint8_t(img[size_t(y * w + x)]);
        } else {
            const json& pm = role_plane(0, "rgb");
            need(u(pm, "width") == w && u(pm, "height") == h && u(pm, "components") == 3, "RGB 平面尺寸不符");
            const Samples rgb = plane(pm);
            Samples alpha;
            const json& ac = L.at("alpha_const");
            if (ch == 4 && ac.is_null()) alpha = plane(role_plane(0, "alpha"));
            need(ch != 4 || !ac.is_null() || alpha.size() == w * h, "Alpha 平面尺寸不符");
            for (uint64_t y = 0; y < h; ++y)
                for (uint64_t x = 0; x < w; ++x) {
                    const uint16_t* s = &rgb[size_t((y * w + x) * 3)];
                    uint8_t* px = row(y) + x * ch;
                    px[0] = uint8_t(s[2]), px[1] = uint8_t(s[1]), px[2] = uint8_t(s[0]);  // 文件里是 B G R
                    if (ch == 4) px[3] = alpha.empty() ? uint8_t(ac.get<uint64_t>()) : uint8_t(alpha[size_t(y * w + x)]);
                }
        }
        put(off, rows);
    }

    void raw() {
        const json& L = recipe.at("layout");
        const std::string pf = L.at("pixel_format");
        const uint64_t w = u(L, "width"), h = u(L, "height"), off = u(L, "body_offset"), len = u(L, "body_len");
        need(w && h && len <= out.size() && w * h <= out.size() * 8, "RAW 版面参数无效");
        // 像素格式（GenICam PFNC）：Mono 或 Bayer{RG,GB,GR,BG} 加位深后缀；8 位每像素 1 字节，10~16 位 2 字节小端，
        // 10p / 12p 为 LSB 优先的连续位流，12Packed 为每 2 像素 3 字节
        std::string suffix;
        if (pf.rfind("Mono", 0) == 0) suffix = pf.substr(4);
        else if (pf.size() > 7 && pf.rfind("Bayer", 0) == 0 &&
                 (pf.compare(5, 2, "RG") == 0 || pf.compare(5, 2, "GB") == 0 || pf.compare(5, 2, "GR") == 0 || pf.compare(5, 2, "BG") == 0))
            suffix = pf.substr(7);
        static const char* const kSuffixes[] = {"8", "10", "12", "14", "16", "10p", "12p", "12Packed"};
        need(std::find(std::begin(kSuffixes), std::end(kSuffixes), suffix) != std::end(kSuffixes), "未知的像素格式 " + pf);
        const bool p10 = suffix == "10p", p12 = suffix == "12p", p12x = suffix == "12Packed";
        const bool packed = p10 || p12 || p12x;
        const Samples img = gray(0, h, w);
        Bytes body;
        if (packed) {
            const uint64_t n = w * h, bits = p10 ? 10 : 12, size = (n * bits + 7) / 8;
            body.assign(size_t(size), 0);
            if (p12x) {  // 每 2 像素 3 字节：b0 = p0>>4，b1 = (p0&0xF) | (p1&0xF)<<4，b2 = p1>>4
                for (uint64_t i = 0; i < n; i += 2) {
                    const uint32_t a = img[size_t(i)], b = i + 1 < n ? img[size_t(i + 1)] : 0;
                    const uint8_t t[3] = {uint8_t(a >> 4), uint8_t((a & 0xF) | ((b & 0xF) << 4)), uint8_t(b >> 4)};
                    for (int k = 0; k < 3; ++k)
                        if (i / 2 * 3 + k < size) body[size_t(i / 2 * 3 + k)] = t[k];
                }
            } else {  // Mono10p / Mono12p：LSB 优先的连续位流
                for (uint64_t i = 0; i < n; ++i) {
                    const uint64_t bit = i * bits;
                    const uint32_t v = uint32_t(img[size_t(i)]) << (bit % 8);
                    for (int k = 0; k < 3; ++k)
                        if (bit / 8 + k < size) body[size_t(bit / 8 + k)] |= uint8_t(v >> (8 * k));
                }
            }
            const Bytes tail = extra("packed_tail", 0);
            need(tail.size() == 1 && !body.empty(), "打包尾字节无效");
            body.back() = tail[0];
        } else {
            const uint64_t bpp = suffix == "8" ? 1 : 2, rb = w * bpp, stride = L.at("stride").is_null() ? rb : u(L, "stride");
            need(stride >= rb, "行跨距无效");
            body.assign(size_t(stride * h), 0);
            for (uint64_t y = 0; y < h; ++y)
                for (uint64_t x = 0; x < w; ++x) {
                    const uint16_t v = img[size_t(y * w + x)];
                    uint8_t* o = &body[size_t(y * stride + x * bpp)];
                    o[0] = uint8_t(v);
                    if (bpp == 2) o[1] = uint8_t(v >> 8);
                }
            if (!L.at("padding_zero").get<bool>()) {
                const Bytes pad = extra("row_padding", 0);
                need(pad.size() == h * (stride - rb), "行填充长度不符");
                for (uint64_t y = 0; y < h; ++y) std::memcpy(&body[size_t(y * stride + rb)], &pad[size_t(y * (stride - rb))], size_t(stride - rb));
            }
        }
        need(body.size() == len, "RAW 重组长度不符");
        put(off, body);
    }

    void tiff() {
        const json& L = recipe.at("layout");
        const bool little = L.at("byteorder") == "II";
        for (const auto& pr : L.at("pages")) {
            const uint64_t image = u(pr, "index"), width = u(pr, "width"), height = u(pr, "height"), samples = u(pr, "samples");
            const uint64_t bits = u(pr, "bits"), planar = u(pr, "planar");
            need(width && height && samples >= 1 && samples <= 64 && (bits == 8 || bits == 16) && (planar == 1 || planar == 2),
                 "TIFF 页参数无效");
            const bool tiled = pr.at("tiled").get<bool>();
            // 像素块几何（规格 13.4）：条带每条带 min(rps, 剩余) 行、整宽；图块按网格逐行、每块满尺寸；planar=2 时按分量重复
            struct Blk {
                uint64_t rows, cols, y, x;
                int64_t sample;
            };
            std::vector<Blk> geo, blocks;
            uint64_t ch, cw;
            if (tiled) {
                const uint64_t th = pr.at("tile").at(0).get<uint64_t>(), tw = pr.at("tile").at(1).get<uint64_t>();
                need(th && tw, "图块尺寸无效");
                const uint64_t ny = (height + th - 1) / th, nx = (width + tw - 1) / tw;
                need(ny * nx <= (1u << 24), "图块过多");
                for (uint64_t i = 0; i < ny * nx; ++i) geo.push_back({th, tw, i / nx * th, i % nx * tw, -1});
                ch = ny * th, cw = nx * tw;
            } else {
                const uint64_t rps0 = pr.at("rowsperstrip").is_null() ? 0 : u(pr, "rowsperstrip");
                const uint64_t rps = std::min(rps0 ? rps0 : height, height), ns = (height + rps - 1) / rps;
                need(ns <= (1u << 24), "条带过多");
                for (uint64_t i = 0; i < ns; ++i) geo.push_back({std::min(rps, height - i * rps), width, i * rps, 0, -1});
                ch = height, cw = width;
            }
            for (uint64_t s = 0; s < (planar == 1 ? 1 : samples); ++s)
                for (auto b : geo) {
                    b.sample = planar == 2 ? int64_t(s) : -1;
                    blocks.push_back(b);
                }
            need(ch * cw * samples <= out.size() * 8 + 1024, "TIFF 画布过大");
            const auto& offsets = pr.at("offsets");
            const auto& counts = pr.at("bytecounts");
            need(offsets.size() == blocks.size() && counts.size() == blocks.size(), "像素块数不符");
            Samples canvas(size_t(ch * cw * samples));
            if (samples == 1) {
                canvas = gray(image, ch, cw);
            } else if (samples == 3) {
                const json& pm = role_plane(image, "rgb");
                need(u(pm, "width") == cw && u(pm, "height") == ch && u(pm, "components") == 3, "RGB 平面尺寸不符");
                canvas = plane(pm);
            } else {
                for (uint64_t k = 0; k < samples; ++k) {
                    const Samples v = gray(image, ch, cw, int(k));
                    for (uint64_t i = 0; i < ch * cw; ++i) canvas[size_t(i * samples + k)] = v[size_t(i)];
                }
            }
            const json& comp = pr.at("compression");
            const uint64_t predictor = u(pr, "predictor");
            need(predictor == 1 || predictor == 2, "Predictor 无效");
            const uint64_t bps = bits / 8, spp = planar == 1 ? samples : 1, mask = bps == 1 ? 0xFF : 0xFFFF;
            for (size_t i = 0; i < blocks.size(); ++i) {
                const Blk& b = blocks[i];
                Bytes raw(size_t(b.rows * b.cols * spp * bps));
                for (uint64_t r = 0; r < b.rows; ++r)
                    for (uint64_t s = 0; s < spp; ++s) {
                        uint32_t prev = 0;
                        for (uint64_t col = 0; col < b.cols; ++col) {
                            const uint64_t chn = b.sample >= 0 ? uint64_t(b.sample) : s;
                            uint32_t v = canvas[size_t(((b.y + r) * cw + b.x + col) * samples + chn)];
                            if (predictor == 2) {  // 水平差分，按位宽回绕
                                const uint32_t cur = v;
                                v = uint32_t((uint64_t(v) - prev) & mask);
                                prev = cur;
                            }
                            uint8_t* o = &raw[size_t(((r * b.cols + col) * spp + s) * bps)];
                            if (bps == 1) o[0] = uint8_t(v);
                            else if (little) o[0] = uint8_t(v), o[1] = uint8_t(v >> 8);
                            else o[0] = uint8_t(v >> 8), o[1] = uint8_t(v);
                        }
                    }
                Bytes blk;
                if (comp.is_null()) {
                    blk = std::move(raw);
                    if (has_extra("block_tail", image, int64_t(i))) {
                        const Bytes tail = extra("block_tail", image, int64_t(i));
                        blk.insert(blk.end(), tail.begin(), tail.end());
                    }
                } else {
                    const json& encs = pr.at("encoders");
                    char variant = 0;
                    if (!encs.is_null()) {
                        const std::string v = encs.get<std::string>();
                        need(v.size() == blocks.size(), "逐块编码器记录长度不符");
                        variant = v[i];
                    }
                    blk = encode_block(pr.at("encoder"), variant, raw, size_t(b.cols * spp * bps));
                }
                need(counts[i].is_number_unsigned() && offsets[i].is_number_unsigned(), "像素块位置无效");
                need(blk.size() == counts[i].get<uint64_t>(), "像素块重组长度与原文件不符");
                put(offsets[i].get<uint64_t>(), blk);
            }
        }
    }
};

}  // namespace

bool restore(const uint8_t* data, size_t size, std::vector<uint8_t>& out, std::string& error) {
    out.clear();
    try {
        Container c;
        c.d = data;
        c.n = size;
        c.open();
        int recipes = 0;
        json recipe;
        for (const auto& s : c.segs)
            if (s.type == RECIPE) {
                ++recipes;
                const Bytes t = c.decode(s);
                recipe = json::parse(t.begin(), t.end());
            }
        need(recipes == 1, "配方段数量不对");
        need(recipe.value("iis_recipe", 0) == 1, "配方版本不支持");
        out.assign(size_t(c.orig_size), 0);
        std::vector<Range> covered;
        for (const auto& s : c.segs) {
            if (s.type != SKELETON) continue;
            Bytes meta;
            const Bytes bytes = c.decode(s, &meta);
            // 区间表：LEB128 编码的 个数, (间隔, 长度)…；区间升序、互不重叠
            const uint8_t* p = meta.data();
            const uint8_t* end = p + meta.size();
            uint64_t count = 0, pos = 0, at = 0;
            need(leb(p, end, count) && count > 0 && count <= meta.size(), "区间表无效");
            for (uint64_t i = 0; i < count; ++i) {
                uint64_t gap = 0, len = 0;
                need(leb(p, end, gap) && leb(p, end, len) && len > 0, "区间表无效");
                need(gap <= c.orig_size - pos && len <= c.orig_size - pos - gap, "区间超出原文件");
                need(at + len <= bytes.size(), "原样字节长度不符");
                std::memcpy(&out[size_t(pos + gap)], &bytes[size_t(at)], size_t(len));
                covered.push_back({pos + gap, len});
                pos += gap + len;
                at += len;
            }
            need(p == end && at == bytes.size(), "原样字节长度与区间表不符");
        }
        const std::string format = recipe.at("format");
        if (format != "bytes") {
            Rebuild rb{c, recipe, out, covered};
            if (format == "bmp") rb.bmp();
            else if (format == "raw") rb.raw();
            else if (format == "tiff") rb.tiff();
            else throw Bad("不支持的原格式 " + format);
        }
        // 每个字节恰好写一次
        std::sort(covered.begin(), covered.end(), [](const Range& a, const Range& b) { return a.offset < b.offset; });
        uint64_t total = 0;
        for (size_t i = 0; i < covered.size(); ++i) {
            need(i == 0 || covered[i].offset >= covered[i - 1].offset + covered[i - 1].length, "还原区间重叠");
            total += covered[i].length;
        }
        need(total == c.orig_size, "还原区间没有恰好覆盖整个原文件");
        uint8_t d[32];
        sha256(out.data(), out.size(), d);
        need(std::memcmp(d, c.sha, 32) == 0, "还原结果的 SHA-256 与原文件不符");
        return true;
    } catch (const std::exception& e) {
        out.clear();
        error = e.what();
        return false;
    }
}

std::string sha256_hex(const uint8_t* data, size_t size) {
    uint8_t d[32];
    sha256(data, size, d);
    static const char* x = "0123456789abcdef";
    std::string s;
    for (uint8_t b : d) s += x[b >> 4], s += x[b & 15];
    return s;
}

}  // namespace iis_decode
