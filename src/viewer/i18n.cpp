#include "i18n.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace iis::i18n {

namespace {
std::atomic<Lang> g_lang{system_lang()};
}

Lang lang() { return g_lang.load(); }
void set_lang(Lang l) { g_lang = l; }

Lang system_lang() { return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE ? Lang::Zh : Lang::En; }

std::string trf(const char* zh, const char* en, ...) {
    va_list a, b;
    va_start(a, en);
    va_copy(b, a);
    const char* f = tr(zh, en);
    const int n = std::vsnprintf(nullptr, 0, f, a);
    std::string s(n > 0 ? size_t(n) : 0, '\0');
    if (n > 0) std::vsnprintf(s.data(), s.size() + 1, f, b);
    va_end(b);
    va_end(a);
    return s;
}

}  // namespace iis::i18n
