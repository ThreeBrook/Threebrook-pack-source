// 极简测试框架：TEST 注册、CHECK 断言，失败不中止，最后汇总。不引入第三方依赖。
#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <random>
#include <string>
#include <vector>

namespace testing {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}

inline int& failures() {
    static int n = 0;
    return n;
}

struct Register {
    Register(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline void report(const char* file, int line, const std::string& what) {
    ++failures();
    std::printf("  失败 %s:%d  %s\n", file, line, what.c_str());
}

// 固定种子的随机数据，结果可复现
inline std::vector<uint8_t> random_bytes(size_t n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<uint8_t> v(n);
    for (auto& b : v) b = uint8_t(rng());
    return v;
}

}  // namespace testing

#define TEST(name)                                                    \
    static void test_##name();                                        \
    static testing::Register reg_##name(#name, &test_##name);         \
    static void test_##name()

#define CHECK(cond)                                                   \
    do {                                                              \
        if (!(cond)) testing::report(__FILE__, __LINE__, #cond);      \
    } while (0)

#define CHECK_EQ(a, b)                                                                                 \
    do {                                                                                               \
        const auto va_ = (a);                                                                          \
        const auto vb_ = (b);                                                                          \
        if (!(va_ == vb_))                                                                             \
            testing::report(__FILE__, __LINE__,                                                        \
                            std::string(#a " == " #b "  实际 ") + std::to_string(va_) + " vs " +       \
                                std::to_string(vb_));                                                  \
    } while (0)
