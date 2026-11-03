#include "tiffcodec.hpp"

#include <libdeflate.h>
#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <memory>

namespace iis::tiffcodec {

namespace {

constexpr uint32_t kClear = 256, kEoi = 257, kFirst = 258;

// LZW 编码字典：键 (前缀码, 字节) < 2^20，值为码。按“代”标记，换代即清空，避免每个条带清 4 MB。
// 只影响速度，不影响输出——两种编码器的语义都是精确字典。
class LzwDict {
public:
    LzwDict() : slots_(size_t(1) << 20, 0) {}
    void clear() {
        if (++gen_ == (1u << 19)) {
            std::fill(slots_.begin(), slots_.end(), 0u);
            gen_ = 1;
        }
    }
    int find(uint32_t key) const {
        const uint32_t v = slots_[key];
        return (v >> 13) == gen_ ? int(v & 0x1FFF) : -1;
    }
    void put(uint32_t key, uint32_t code) { slots_[key] = (gen_ << 13) | code; }

private:
    std::vector<uint32_t> slots_;
    uint32_t gen_ = 1;
};

LzwDict& dict() {
    thread_local LzwDict d;
    d.clear();
    return d;
}

}  // namespace

// ---------------- LZW 解码（TIFF 新式 MSB 与旧式 LSB，与 imagecodecs 的行为一致）----------------

bool lzw_decode(const uint8_t* src, size_t n, size_t max_out, Bytes& out) {
    out.clear();
    if (n == 0) return true;
    if (n < 2) return false;
    bool msb = true;
    if (src[0] == 0 && (src[1] & 1)) msb = false;
    else if (src[0] != 128 || (src[1] & 128)) return false;  // 必须以 CLEAR 开头

    constexpr uint32_t kTable = 5120;  // 容忍码表满后不发 CLEAR 的写图程序
    std::vector<uint64_t> start(kTable);
    std::vector<uint32_t> len(kTable);
    uint32_t tablesize = 258, bitw = 9, maxcode = msb ? 510 : 511;
    int64_t oldcode = -1;
    size_t prev_out = 0, prev_len = 0, pos = 0;
    uint64_t bitbuf = 0;
    int bitcnt = 0;
    out.reserve(std::min<size_t>(max_out, n * 4));

    auto next_code = [&](uint32_t& code) {
        while (bitcnt < int(bitw) && pos < n) {
            if (msb) bitbuf |= uint64_t(src[pos++]) << (56 - bitcnt);
            else bitbuf |= uint64_t(src[pos++]) << bitcnt;
            bitcnt += 8;
        }
        if (bitcnt < int(bitw)) {
            code = kEoi;  // 数据结束视同 EOI
            return;
        }
        if (msb) {
            code = uint32_t(bitbuf >> (64 - bitw)) & ((1u << bitw) - 1);
            bitbuf <<= bitw;
        } else {
            code = uint32_t(bitbuf & ((1u << bitw) - 1));
            bitbuf >>= bitw;
        }
        bitcnt -= int(bitw);
    };

    for (;;) {
        uint32_t code;
        next_code(code);
        if (code == kEoi) break;
        if (code == kClear) {
            tablesize = 258;
            bitw = 9;
            maxcode = msb ? 510 : 511;
            oldcode = -1;
        } else {
            if (tablesize >= kTable || code > tablesize) return false;
            if (code == tablesize && oldcode < 0) return false;
            if (oldcode >= 0) {  // 新条目 = 上一个输出串 + 当前串首字节（输出连续，按位置引用）
                start[tablesize] = prev_out;
                len[tablesize] = uint32_t(prev_len + 1);
                ++tablesize;
            }
            oldcode = code;
            const size_t s = code < 256 ? SIZE_MAX : size_t(start[code]);
            const size_t l = code < 256 ? 1 : len[code];
            if (out.size() + l > max_out) return false;
            const size_t at = out.size();
            if (code < 256) {
                out.push_back(uint8_t(code));
            } else {
                for (size_t j = 0; j < l; ++j) {
                    const uint8_t b = out[s + j];  // K-w-K 时 s+j 可能是刚写入的字节；先取值再追加
                    out.push_back(b);
                }
            }
            prev_out = at;
            prev_len = l;
        }
        if (tablesize > maxcode) {
            ++bitw;
            if (msb) maxcode = bitw < 12 ? ((maxcode << 1) | 2) : kTable;
            else maxcode = bitw < 12 ? ((maxcode << 1) | 1) : kTable;
            if (bitw > 12) bitw = 12;
        }
    }
    return true;
}

// ---------------- libtiff 兼容 LZW 编码（tif_lzw.c；移植自 bench/layout/lzw_libtiff.py）----------------

Bytes lzw_encode_libtiff(const uint8_t* data, size_t n) {
    constexpr uint32_t kCodeMax = (1u << 12) - 1;
    constexpr uint64_t kGap = 10000;
    Bytes out;
    out.reserve(n + n / 2 + 16);
    uint32_t nextdata = 0;
    int nextbits = 0, nbits = 9;
    uint32_t maxcode = (1u << nbits) - 1;
    uint64_t outcount = 0;

    auto put = [&](uint32_t code) {  // libtiff PutNextCode
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
        LzwDict& table = dict();
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
            table.put(key, free_ent);
            ++free_ent;
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
                maxcode = (1u << nbits) - 1;
            }
        }
        put(ent);  // LZWPostEncode
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

// ---------------- imagecodecs 兼容 LZW 编码（imcd.c imcd_lzw_encode，v2026.10.10）----------------

Bytes lzw_encode_imcd(const uint8_t* src, size_t n) {
    constexpr int64_t kGap = 10000;
    Bytes dst;
    dst.reserve(n * 141 / 100 + 3);
    uint32_t dstbyte;  // 原实现为 int，只用到低 27 位，按无符号处理结果相同
    int bitw = 9, bitc;
    auto write = [&] { dst.push_back(uint8_t(dstbyte >> bitc)); };

    bitc = 1;
    dstbyte = kClear;
    write();
    if (n < 1) {
        dstbyte = ((dstbyte << bitw) | kEoi) << 8;
        bitc += bitw;
        write();
        bitc -= 8;
        write();
        return dst;
    }

    LzwDict& table = dict();
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
    auto reset = [&](int64_t srcindex) {
        table.clear();
        nextcode = kFirst;
        bitw = 9;
        incount_base = srcindex;
        outcount_base = int64_t(dst.size());
        enc_ratio = 0;
    };

    for (int64_t srcindex = 1; srcindex < int64_t(n); ++srcindex) {
        const uint32_t k = src[srcindex];
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
            reset(srcindex);
            checkpoint = srcindex + kGap;
        } else {
            switch (nextcode) {
                case 512: bitw = 10; break;
                case 1024: bitw = 11; break;
                case 2048: bitw = 12; break;
                default: break;
            }
            if (srcindex >= checkpoint) {
                const int64_t incount = srcindex - incount_base;
                const int64_t outcount = int64_t(dst.size()) - outcount_base;
                checkpoint = srcindex + kGap;
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
                        reset(srcindex);
                    } else {
                        enc_ratio = ratio;
                    }
                }
            }
        }
    }
    put_code(omega);
    switch (nextcode) {
        case 511: bitw = 10; break;
        case 1023: bitw = 11; break;
        case 2047: bitw = 12; break;
        default: break;
    }
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

// ---------------- PackBits（imcd.c imcd_packbits_encode / decode）----------------

Bytes packbits_encode(const uint8_t* src, size_t srcsize) {
    Bytes dst;
    if (!srcsize) return dst;
    const size_t maxdst = srcsize + (srcsize + 127) / 128;
    dst.resize(maxdst);
    const size_t dstend = maxdst - 1;  // imagecodecs 以 maxdst 为输出容量
    size_t s = 0, d = 0;

    auto next_replicate = [&](size_t p, size_t end) -> size_t {  // 返回 SIZE_MAX 表示没有
        while (++p < end)
            if (src[p - 1] == src[p]) return p - 1;
        return SIZE_MAX;
    };
    auto replicate_length = [&](size_t p, size_t end) -> size_t {
        const uint8_t v = p < end ? src[p] : 0;
        size_t q = p;
        while (++q < end && src[q] == v) {
        }
        return q - p;
    };

    bool overflow = false;
    while (s < srcsize) {
        const size_t lkend = s + 129 < srcsize ? s + 129 : srcsize;
        size_t dup = next_replicate(s, lkend);
        if (dup == s) {
            const size_t rep = std::min<size_t>(128, replicate_length(dup, srcsize));
            if (d < dstend) {
                dst[d++] = uint8_t(257 - int(rep));
                dst[d++] = src[s];
            } else {
                overflow = true;
                break;
            }
            s += rep;
            continue;
        }
        size_t literal;
        if (dup == SIZE_MAX) {
            literal = srcsize - s;
        } else {
            const size_t rep = replicate_length(dup, srcsize);
            if (rep < 3) {
                const size_t nextsrc = dup + rep;
                const size_t nextdup = next_replicate(nextsrc, lkend);
                if (nextdup != SIZE_MAX && nextdup > nextsrc) dup = nextdup;  // 跳过 2 字节游程
            }
            literal = dup - s;
        }
        literal = std::min<size_t>(128, literal);
        if (d + literal < dstend) {
            dst[d++] = uint8_t(literal - 1);
            std::memcpy(&dst[d], src + s, literal);
            d += literal;
            s += literal;
        } else {
            overflow = true;
            break;
        }
    }
    if (overflow) {  // 超出“全字面量”长度：改为只用字面量包重新编码
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

Bytes packbits_encode_rows(const uint8_t* src, size_t n, size_t row_bytes) {
    Bytes out;
    if (!row_bytes) return out;
    for (size_t i = 0; i < n; i += row_bytes) {
        const Bytes r = packbits_encode(src + i, std::min(row_bytes, n - i));
        out.insert(out.end(), r.begin(), r.end());
    }
    return out;
}

bool packbits_decode(const uint8_t* src, size_t n, size_t max_out, Bytes& out) {
    out.clear();
    size_t i = 0;
    while (i < n) {
        const uint8_t c = src[i++];
        if (c < 128) {
            const size_t cnt = std::min<size_t>(size_t(c) + 1, n - i);
            if (out.size() + cnt > max_out) return false;
            out.insert(out.end(), src + i, src + i + cnt);
            i += cnt;
        } else if (c > 128) {
            if (i >= n) break;
            const size_t cnt = 257 - size_t(c);
            if (out.size() + cnt > max_out) return false;
            out.insert(out.end(), cnt, src[i++]);
        }
    }
    return true;
}

// ---------------- Deflate ----------------

Bytes zlib_encode(const uint8_t* src, size_t n, int level) {
    uLongf cap = compressBound(uLong(n));
    Bytes out(cap);
    if (compress2(out.data(), &cap, src, uLong(n), level) != Z_OK) return {};
    out.resize(cap);
    return out;
}

Bytes libdeflate_encode(const uint8_t* src, size_t n, int level) {
    struct Del {
        void operator()(libdeflate_compressor* c) const { libdeflate_free_compressor(c); }
    };
    thread_local std::unique_ptr<libdeflate_compressor, Del> cache[13];
    auto& c = cache[std::clamp(level, 0, 12)];
    if (!c) c.reset(libdeflate_alloc_compressor(level));
    if (!c) return {};
    Bytes out(libdeflate_zlib_compress_bound(c.get(), n));
    const size_t got = libdeflate_zlib_compress(c.get(), src, n, out.data(), out.size());
    out.resize(got);
    return out;
}

bool zlib_decode(const uint8_t* src, size_t n, size_t max_out, Bytes& out) {
    out.clear();
    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) return false;
    zs.next_in = const_cast<Bytef*>(src);
    zs.avail_in = uInt(std::min<size_t>(n, 0xFFFFFFFFu));
    uint8_t buf[65536];
    int st = Z_OK;
    while (st == Z_OK) {
        zs.next_out = buf;
        zs.avail_out = sizeof buf;
        st = inflate(&zs, Z_NO_FLUSH);
        const size_t got = sizeof buf - zs.avail_out;
        if (out.size() + got > max_out) {
            st = Z_BUF_ERROR;
            break;
        }
        out.insert(out.end(), buf, buf + got);
        if (st == Z_BUF_ERROR && zs.avail_in == 0) break;  // 数据不完整
    }
    inflateEnd(&zs);
    return st == Z_STREAM_END;
}

const char* zlib_lib_version() { return zlibVersion(); }
const char* libdeflate_version() { return LIBDEFLATE_VERSION_STRING; }

}  // namespace iis::tiffcodec
