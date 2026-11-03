#include "rawguess.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace iis::rawguess {

namespace {

constexpr uint64_t kMinWidth = 64, kMaxWidth = 65535, kMinHeight = 8;
constexpr uint64_t kRows = 48;     // 每个候选行宽抽样的行数
constexpr uint64_t kCols = 4096;   // 每行最多看这么多像素
constexpr size_t kMaxSize = size_t(1) << 30;

// 常见的已压缩 / 非图像文件：不猜（省时间；猜了也会因为估计不出二维结构而放弃）
bool known_magic(const uint8_t* d, size_t n) {
    static const char* const magics[] = {"\x89PNG", "\xFF\xD8\xFF", "PK\x03\x04", "%PDF", "GIF8", "IISCONT",
                                         "MZ",      "\x1F\x8B",     "7z\xBC\xAF", "Rar!", "\x28\xB5\x2F\xFD"};
    for (const char* m : magics) {
        const size_t k = std::strlen(m);
        if (n >= k && std::memcmp(d, m, k) == 0) return true;
    }
    return false;
}

std::vector<uint64_t> divisors(uint64_t m) {
    std::vector<uint64_t> out;
    for (uint64_t i = 1; i * i <= m; ++i) {
        if (m % i) continue;
        for (const uint64_t w : {i, m / i})
            if (w >= kMinWidth && w <= kMaxWidth && m / w >= kMinHeight) out.push_back(w);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

inline int64_t med(int64_t a, int64_t b, int64_t c) {
    const int64_t mx = std::max(a, b), mn = std::min(a, b);
    return c >= mx ? mn : c <= mn ? mx : a + b - c;
}

double bits_for(double mean_abs) {  // 拉普拉斯分布的熵近似
    return std::max(std::log2(2 * 2.718281828459045 * std::max(mean_abs, 1e-3)), 0.05);
}

struct Cost {
    double bits = 0;      // 估计每像素比特数
    double vertical = 0;  // 与上一行同列之差 / 与左邻之差：行宽对时上一行最像，这个比值最小
    bool bayer = false;   // 用的是同色预测（Bayer）
};

// 把样本看成 w 宽的图来估计
Cost cost(const fmt::Samples& s, uint64_t w) {
    const uint64_t h = s.size() / w, k = std::min(kRows, h - 2), xm = std::min(w, kCols);
    uint64_t en = 0, eb = 0, cnt_n = 0, cnt_b = 0, dv = 0, dh = 0;
    for (uint64_t r = 0; r < k; ++r) {
        const uint64_t y = 2 + (k > 1 ? r * (h - 3) / (k - 1) : 0);
        const uint16_t* r0 = &s[size_t(y * w)];
        const uint16_t* r1 = r0 - w;
        const uint16_t* r2 = r1 - w;
        for (uint64_t x = 1; x < xm; ++x) {
            en += uint64_t(std::llabs(int64_t(r0[x]) - med(r0[x - 1], r1[x], r1[x - 1])));
            dv += uint64_t(std::llabs(int64_t(r0[x]) - r1[x]));
            dh += uint64_t(std::llabs(int64_t(r0[x]) - r0[x - 1]));
            ++cnt_n;
            if (x >= 2) {  // Bayer：同色像素在左边第 2 个、上面第 2 行
                eb += uint64_t(std::llabs(int64_t(r0[x]) - med(r0[x - 2], r2[x], r2[x - 2])));
                ++cnt_b;
            }
        }
    }
    const double mn = double(en) / double(std::max<uint64_t>(cnt_n, 1));
    const double mb = double(eb) / double(std::max<uint64_t>(cnt_b, 1));
    // 同色预测只在明显更好时采用（否则半个行宽会和真实行宽打平）
    const bool bayer = mb < 0.8 * mn;
    return {bits_for(bayer ? mb : mn), double(dv + 1) / double(dh + 1), bayer};
}

// 横向是否像 Bayer：隔一个像素比相邻像素更像（颜色交替）
bool cfa_like(const fmt::Samples& s) {
    const size_t step = std::max<size_t>(1, s.size() / 200000);
    uint64_t d1 = 0, d2 = 0;
    for (size_t i = 0; i + 2 < s.size(); i += step) {
        d1 += uint64_t(std::abs(int(s[i + 1]) - int(s[i])));
        d2 += uint64_t(std::abs(int(s[i + 2]) - int(s[i])));
    }
    return double(d2) < 0.8 * double(d1);
}

struct Interp {
    const char* format;
    int bits;  // 每像素存储位数
    int bpp;   // 每像素字节；打包格式为 0
};

bool decode(const Interp& f, const uint8_t* d, size_t n, fmt::Samples& s) {
    if (f.bpp == 1) {
        s.assign(d, d + n);
        return true;
    }
    if (f.bpp == 2) {
        if (n % 2) return false;
        s.resize(n / 2);
        for (size_t i = 0; i < s.size(); ++i) s[i] = uint16_t(d[2 * i] | d[2 * i + 1] << 8);
        return true;
    }
    // 打包格式：像素数取打包后恰好为 n 字节的最大值
    uint64_t m = uint64_t(n) * 8 / uint64_t(f.bits);
    while (m > 0 && fmt::packed_size(f.format, m) > n) --m;
    if (m == 0 || fmt::packed_size(f.format, m) != n) return false;
    return fmt::unpack(f.format, d, n, m, s);
}

}  // namespace

std::optional<fmt::RawParams> guess(const uint8_t* d, size_t n) {
    if (n < kMinWidth * kMinHeight || n > kMaxSize || known_magic(d, n)) return std::nullopt;
    static const Interp interps[] = {
        {"Mono8", 8, 1}, {"Mono16", 16, 2}, {"Mono10p", 10, 0}, {"Mono12p", 12, 0}, {"Mono12Packed", 12, 0}};
    struct Best {
        double total = 0;
        const Interp* f = nullptr;
        uint64_t w = 0, m = 0;
        double native = 0;
    } best;
    fmt::Samples s;
    for (const Interp& f : interps) {
        if (!decode(f, d, n, s)) continue;
        // 本格式内取估计大小最小的行宽。内容简单的图，半个行宽可能与真实行宽打平（“上一行”其实是同一行的另一半，
        // 预测退化成只看左邻，估计却差不多）：两倍行宽的估计在 2% 以内、且上一行更像时，改用两倍行宽
        std::vector<std::pair<uint64_t, Cost>> cs;
        for (const uint64_t w : divisors(s.size())) cs.emplace_back(w, cost(s, w));
        const std::pair<uint64_t, Cost>* pick = nullptr;
        for (const auto& c : cs)
            if (!pick || c.second.bits < pick->second.bits) pick = &c;
        if (!pick) continue;
        // 真正的 Bayer 图隔一行才同色，两倍行宽总显得“上一行更像”，不能改
        for (bool moved = !(pick->second.bayer && cfa_like(s)); moved;) {
            moved = false;
            for (const auto& c : cs)
                if (c.first == 2 * pick->first && c.second.bits <= pick->second.bits * 1.02 &&
                    c.second.vertical < pick->second.vertical) {
                    pick = &c;
                    moved = true;
                    break;
                }
        }
        const double total = pick->second.bits * double(s.size());
        if (!best.f || total < best.total) best = {total, &f, pick->first, s.size(), double(f.bits) * double(s.size())};
    }
    // 估计看不出二维结构（压不到存储位数的 90%）：不像图像
    if (!best.f || best.total > 0.9 * best.native) return std::nullopt;

    fmt::RawParams p;
    p.width = best.w;
    p.height = best.m / best.w;
    p.offset = 0;
    p.origin = "auto";
    std::string name = best.f->format;
    if (best.f->bpp) {
        decode(*best.f, d, n, s);
        // 行尾补齐：右侧若干列在每一行都是 0，视为行填充（看错了也只影响压缩比）
        uint64_t pad = 0;
        while (pad < 64 / uint64_t(best.f->bpp) && best.w - pad > 16) {
            bool zero = true;
            for (uint64_t y = 0; y < p.height && zero; ++y) zero = s[size_t(y * best.w + best.w - 1 - pad)] == 0;
            if (!zero) break;
            ++pad;
        }
        if (pad) {
            p.width = best.w - pad;
            p.stride = best.w * uint64_t(best.f->bpp);
        }
        if (best.f->bpp == 2) {  // 名称按实际取值范围；版面都是 16 位小端
            const uint16_t mx = *std::max_element(s.begin(), s.end());
            name = mx < 1024 ? "Mono10" : mx < 4096 ? "Mono12" : mx < 16384 ? "Mono14" : "Mono16";
        }
    }
    p.pixel_format = name;
    p.pf = fmt::find_pixel_format(name);
    if (!p.pf) return std::nullopt;
    return p;
}

}  // namespace iis::rawguess
