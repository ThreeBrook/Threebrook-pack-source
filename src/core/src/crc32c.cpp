// CRC32C（Castagnoli，反射多项式 0x82F63B78）。用于容器每段的介质损坏检测。
// 软件实现为 slicing-by-8；x86-64 且 CPU 支持 SSE4.2 时改用 crc32 指令，两者结果逐位相同。
#include "internal.hpp"

#if defined(_M_X64) || defined(__x86_64__)
#  define IIS_CRC32C_X86 1
#  include <nmmintrin.h>
#  if defined(_MSC_VER)
#    include <intrin.h>
#  else
#    include <cpuid.h>
#  endif
#endif

namespace {

struct Tables {
    uint32_t t[8][256];
    Tables() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0x82F63B78u & (0u - (c & 1)));
            t[0][i] = c;
        }
        for (uint32_t i = 0; i < 256; ++i)
            for (int s = 1; s < 8; ++s) t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFF];
    }
};

const Tables& tables() {
    static const Tables tb;
    return tb;
}

uint32_t crc_sw(uint32_t c, const uint8_t* p, size_t n) {
    const auto& t = tables().t;
    while (n && (reinterpret_cast<uintptr_t>(p) & 7)) {
        c = (c >> 8) ^ t[0][(c ^ *p++) & 0xFF];
        --n;
    }
    for (; n >= 8; n -= 8, p += 8) {
        // 按小端组装，与平台字节序无关
        const uint32_t lo = c ^ (uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24);
        const uint32_t hi = uint32_t(p[4]) | uint32_t(p[5]) << 8 | uint32_t(p[6]) << 16 | uint32_t(p[7]) << 24;
        c = t[7][lo & 0xFF] ^ t[6][(lo >> 8) & 0xFF] ^ t[5][(lo >> 16) & 0xFF] ^ t[4][lo >> 24] ^
            t[3][hi & 0xFF] ^ t[2][(hi >> 8) & 0xFF] ^ t[1][(hi >> 16) & 0xFF] ^ t[0][hi >> 24];
    }
    while (n--) c = (c >> 8) ^ t[0][(c ^ *p++) & 0xFF];
    return c;
}

#ifdef IIS_CRC32C_X86
#  if !defined(_MSC_VER)
__attribute__((target("sse4.2")))
#  endif
uint32_t crc_hw(uint32_t c, const uint8_t* p, size_t n) {
    while (n && (reinterpret_cast<uintptr_t>(p) & 7)) {
        c = _mm_crc32_u8(c, *p++);
        --n;
    }
    uint64_t c64 = c;
    for (; n >= 8; n -= 8, p += 8) {
        uint64_t v;
        std::memcpy(&v, p, 8);
        c64 = _mm_crc32_u64(c64, v);
    }
    c = uint32_t(c64);
    while (n--) c = _mm_crc32_u8(c, *p++);
    return c;
}

bool cpu_has_sse42() {
#  if defined(_MSC_VER)
    int r[4];
    __cpuid(r, 1);
    return (r[2] >> 20) & 1;
#  else
    unsigned a, b, c, d;
    return __get_cpuid(1, &a, &b, &c, &d) && ((c >> 20) & 1);
#  endif
}

const bool g_hw = cpu_has_sse42();
#else
const bool g_hw = false;
#endif

}  // namespace

namespace iis {

const char* crc32c_impl_name() { return g_hw ? "sse4.2" : "software"; }

// 测试用：强制走软件实现，便于与硬件结果对照
uint32_t crc32c_software(uint32_t crc, const void* data, size_t size) {
    return ~crc_sw(~crc, static_cast<const uint8_t*>(data), size);
}

}  // namespace iis

extern "C" uint32_t iis_crc32c(uint32_t crc, const void* data, size_t size) {
    if (!size) return crc;
    const auto p = static_cast<const uint8_t*>(data);
#ifdef IIS_CRC32C_X86
    if (g_hw) return ~crc_hw(~crc, p, size);
#endif
    return ~crc_sw(~crc, p, size);
}
