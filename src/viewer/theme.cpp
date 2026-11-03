#include "theme.hpp"

#include "i18n.hpp"

#include <algorithm>
#include <cstring>
#include <string>

namespace iis::ui {

namespace {

constexpr ImU32 rgb(uint32_t hex, int a = 255) {
    return IM_COL32((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF, a);
}

// 三溪外观规范 §2.1、§2.2（threebrook-tokens.css）
const Palette kLight = {false,
                        rgb(0xF6FAF6), rgb(0xEAF3EC), rgb(0xFFFFFF), rgb(0xD6E4DB), rgb(0xC5D7CC), rgb(0x1D3331),
                        rgb(0x4E6461), rgb(0x627874), rgb(0x1C8780), rgb(0xFFFFFF), rgb(0xE0F2EE), rgb(0xD4EEE9),
                        rgb(0xE3EEE7), rgb(0xB3C2BC), rgb(0xBFDCD6), rgb(0xB5D9D2), rgb(0x2F7D4F), rgb(0xE6F4EC),
                        rgb(0x1F5C3A), rgb(0xB0334C), rgb(0xFDEFF2), rgb(0xF2B8C4), rgb(0xFFF0C9), rgb(0xF2B33D)};
const Palette kDark = {true,
                       rgb(0x0F1A19), rgb(0x14231F), rgb(0x172826), rgb(0x27403C), rgb(0x31504B), rgb(0xE4F0EC),
                       rgb(0xA8BDB8), rgb(0x8FA59F), rgb(0x4CC0B6), rgb(0x08201E), rgb(0x173A36), rgb(0x1E3834),
                       rgb(0x1F3330), rgb(0x4E625E), rgb(0x2E5450), rgb(0x2B524D), rgb(0x5DBB86), rgb(0x16302A),
                       rgb(0x8FD3AC), rgb(0xF27F95), rgb(0x3A1D25), rgb(0x6E3443), rgb(0x3A3018), rgb(0xF4C25B)};

const Palette* g_pal = &kLight;
Mode g_mode = Mode::System;
Fonts g_fonts;
std::vector<std::vector<uint8_t>> g_font_data;  // 字体字节，与 ImGui 字体图集同寿命

ImVec4 v4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

// 自定义样式的按钮：底色、悬停色、按下色、文字色、描边色、圆角、高度
bool styled_button(const char* label, float width, float height, ImU32 bg, ImU32 hover, ImU32 active, ImU32 text, ImU32 border,
                   float rounding) {
    ImGui::PushStyleColor(ImGuiCol_Button, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, active);
    ImGui::PushStyleColor(ImGuiCol_Text, text);
    ImGui::PushStyleColor(ImGuiCol_Border, border);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, rounding);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, border ? 1.0f : 0.0f);
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const ImVec2 ts = ImGui::CalcTextSize(label, nullptr, true);
    const float w = width > 0 ? width * dpi : ts.x + 28 * dpi;
    const bool r = ImGui::Button(label, ImVec2(w, height * dpi));
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(5);
    return r;
}

}  // namespace

ImU32 mix(ImU32 a, ImU32 b, float t) {
    const ImVec4 x = v4(a), y = v4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t,
                                                 x.w + (y.w - x.w) * t));
}

bool system_dark() {
    DWORD v = 1, n = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &n) != ERROR_SUCCESS)
        return false;
    return v == 0;
}

const Palette& pal() { return *g_pal; }
Mode mode() { return g_mode; }
const Fonts& fonts() { return g_fonts; }

void apply(Mode m, float dpi) {
    g_mode = m;
    g_pal = (m == Mode::Dark || (m == Mode::System && system_dark())) ? &kDark : &kLight;
    const Palette& p = *g_pal;
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowPadding = ImVec2(16, 16);
    s.FramePadding = ImVec2(10, 7);
    s.ItemSpacing = ImVec2(8, 8);
    s.ItemInnerSpacing = ImVec2(8, 6);
    s.ScrollbarSize = 12;
    s.GrabMinSize = 12;
    s.WindowRounding = 0;
    s.ChildRounding = 14;
    s.FrameRounding = 8;
    s.PopupRounding = 14;
    s.ScrollbarRounding = 6;
    s.GrabRounding = 6;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 1;
    s.PopupBorderSize = 1;
    s.FrameBorderSize = 1;
    s.SeparatorTextBorderSize = 1;
    s.ScaleAllSizes(dpi);
    s.FontScaleDpi = dpi;
    s.FontSizeBase = kBody;

    ImVec4* c = s.Colors;
    auto set = [&](ImGuiCol i, ImU32 v) { c[i] = v4(v); };
    set(ImGuiCol_Text, p.text);
    set(ImGuiCol_TextDisabled, p.text_muted);
    set(ImGuiCol_WindowBg, p.bg);
    set(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
    set(ImGuiCol_PopupBg, p.card);
    set(ImGuiCol_Border, p.border);
    set(ImGuiCol_BorderShadow, IM_COL32(0, 0, 0, 0));
    set(ImGuiCol_FrameBg, p.card);
    set(ImGuiCol_FrameBgHovered, p.card);
    set(ImGuiCol_FrameBgActive, p.card);
    set(ImGuiCol_TitleBg, p.bg);
    set(ImGuiCol_TitleBgActive, p.bg);
    set(ImGuiCol_TitleBgCollapsed, p.bg);
    set(ImGuiCol_MenuBarBg, p.bg);
    set(ImGuiCol_ScrollbarBg, IM_COL32(0, 0, 0, 0));
    set(ImGuiCol_ScrollbarGrab, p.border_strong);
    set(ImGuiCol_ScrollbarGrabHovered, p.offline);
    set(ImGuiCol_ScrollbarGrabActive, p.text_muted);
    set(ImGuiCol_CheckMark, p.accent);
    set(ImGuiCol_SliderGrab, p.accent);
    set(ImGuiCol_SliderGrabActive, p.accent);
    set(ImGuiCol_Button, p.card);
    set(ImGuiCol_ButtonHovered, p.accent_soft);
    set(ImGuiCol_ButtonActive, p.nav_active);
    set(ImGuiCol_Header, p.nav_active);
    set(ImGuiCol_HeaderHovered, p.accent_soft);
    set(ImGuiCol_HeaderActive, p.nav_active);
    set(ImGuiCol_Separator, p.border);
    set(ImGuiCol_SeparatorHovered, p.border_strong);
    set(ImGuiCol_SeparatorActive, p.accent);
    set(ImGuiCol_ResizeGrip, IM_COL32(0, 0, 0, 0));
    set(ImGuiCol_ResizeGripHovered, p.accent_soft);
    set(ImGuiCol_ResizeGripActive, p.accent);
    set(ImGuiCol_PlotHistogram, p.accent);
    set(ImGuiCol_PlotHistogramHovered, p.accent);
    set(ImGuiCol_TextSelectedBg, p.accent_soft);
    set(ImGuiCol_NavCursor, p.accent);
    set(ImGuiCol_ModalWindowDimBg, p.dark ? IM_COL32(0, 0, 0, 120) : IM_COL32(29, 51, 49, 60));
    set(ImGuiCol_Tab, p.card);
    set(ImGuiCol_TabHovered, p.accent_soft);
    set(ImGuiCol_TabSelected, p.accent_soft);
}

std::vector<uint8_t> resource(HINSTANCE inst, int id) {
    HRSRC r = FindResourceW(inst, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));  // RT_RCDATA
    if (!r) return {};
    HGLOBAL g = LoadResource(inst, r);
    const DWORD n = SizeofResource(inst, r);
    const void* p = g ? LockResource(g) : nullptr;
    if (!p || !n) return {};
    return std::vector<uint8_t>(static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
}

void load_fonts(HINSTANCE inst) {
    ImGuiIO& io = ImGui::GetIO();
    // 系统字体：微软雅黑 UI（msyh.ttc 第 2 个字形），没有时退到黑体、宋体
    auto system_font = [&](const char* file, int face, ImFontConfig cfg) -> ImFont* {
        const std::string path = std::string("C:\\Windows\\Fonts\\") + file;
        if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) return nullptr;
        cfg.FontNo = face;
        return io.Fonts->AddFontFromFileTTF(path.c_str(), kBody, &cfg);
    };
    auto cjk = [&](ImFontConfig cfg, bool bold) -> ImFont* {
        ImFont* f = bold ? system_font("msyhbd.ttc", 1, cfg) : nullptr;
        if (!f) f = system_font("msyh.ttc", 1, cfg);
        if (!f) f = system_font("simhei.ttf", 0, cfg);
        if (!f) f = system_font("simsun.ttc", 0, cfg);
        return f;
    };
    // 嵌在资源里的字体；merge_cjk：缺的汉字用雅黑补
    auto embedded = [&](int id, bool merge_cjk) -> ImFont* {
        g_font_data.push_back(resource(inst, id));
        auto& d = g_font_data.back();
        if (d.empty()) return nullptr;
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;
        ImFont* f = io.Fonts->AddFontFromMemoryTTF(d.data(), int(d.size()), kBody, &cfg);
        if (f && merge_cjk) {
            ImFontConfig m;
            m.MergeMode = true;
            cjk(m, false);
        }
        return f;
    };
    g_fonts.body = cjk(ImFontConfig(), false);
    g_fonts.bold = cjk(ImFontConfig(), true);
    g_fonts.title = embedded(102, true);
    g_fonts.title_en = embedded(103, true);
    g_fonts.mono = embedded(104, true);
    if (!g_fonts.body) g_fonts.body = io.Fonts->AddFontDefault();
    for (ImFont** f : {&g_fonts.bold, &g_fonts.title, &g_fonts.title_en, &g_fonts.mono})
        if (!*f) *f = g_fonts.body;
    io.FontDefault = g_fonts.body;
}

const char* product() { return i18n::tr(kProduct, kProductEn); }
const wchar_t* product_w() { return i18n::lang() == i18n::Lang::En ? L"Threebrook Pack" : kProductW; }

// 中文标题：站酷小薇、字号 +2；英文界面：Bricolage Grotesque 粗体、字号不变（规范 §3）
void title(const char* text, float size) {
    const bool en = i18n::lang() == i18n::Lang::En;
    ImGui::PushFont(en ? g_fonts.title_en : g_fonts.title, en ? size : size + 2);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}

void title_en(const char* text, float size) {
    ImGui::PushFont(g_fonts.title_en, size);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}

void mono(const char* text, float size, ImU32 color) {
    ImGui::PushFont(g_fonts.mono, size);
    ImGui::PushStyleColor(ImGuiCol_Text, color ? color : g_pal->text_sub);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void text_sub(const char* text, float size) {
    ImGui::PushFont(nullptr, size);
    ImGui::PushStyleColor(ImGuiCol_Text, g_pal->text_sub);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void text_colored(ImU32 color, const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

bool primary_button(const char* label, float width) {
    const Palette& p = *g_pal;
    return styled_button(label, width, 40, p.accent, mix(p.accent, p.dark ? IM_COL32_WHITE : IM_COL32_BLACK, 0.08f),
                         mix(p.accent, p.dark ? IM_COL32_WHITE : IM_COL32_BLACK, 0.16f), p.on_accent, 0, 10);
}

bool outline_button(const char* label, float width, bool small) {
    const Palette& p = *g_pal;
    return styled_button(label, width, small ? 30.0f : 36.0f, p.card, p.accent_soft, p.nav_active, p.text, p.border_strong, 6);
}

bool danger_button(const char* label, float width) {
    const Palette& p = *g_pal;
    return styled_button(label, width, 36, p.danger_bg, mix(p.danger_bg, p.danger_border, 0.4f), p.danger_border, p.danger,
                         p.danger_border, 6);
}

bool segment(const char* label, bool selected) {
    const Palette& p = *g_pal;
    return selected ? styled_button(label, 0, 30, p.accent_soft, p.accent_soft, p.nav_active, p.accent, p.chip_border, 8)
                    : styled_button(label, 0, 30, p.card, p.accent_soft, p.nav_active, p.text_sub, p.border, 8);
}

void begin_card(const char* id, float height) {
    const Palette& p = *g_pal;
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, p.card);
    ImGui::PushStyleColor(ImGuiCol_Border, p.border);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 14 * dpi);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20 * dpi, 18 * dpi));
    ImGui::BeginChild(id, ImVec2(0, height > 0 ? height * dpi : 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding | (height > 0 ? 0 : ImGuiChildFlags_AutoResizeY));
}

void end_card() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    if (!g_pal->dark) {  // 浅色：很轻的阴影（画在父窗口里，在卡片下面）
        const float dpi = ImGui::GetStyle().FontScaleDpi;
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (int i = 1; i <= 4; ++i) {
            const float o = i * 2.0f * dpi;
            dl->AddRect(ImVec2(a.x - o * 0.5f, a.y), ImVec2(b.x + o * 0.5f, b.y + o), IM_COL32(29, 51, 49, 9 - i * 2), 14 * dpi + o,
                        2 * dpi);
        }
    }
}

void hint(const char* text, float width) {
    const Palette& p = *g_pal;
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const float w = width > 0 ? width : ImGui::GetContentRegionAvail().x, pad = 12 * dpi, bar = 3 * dpi;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - pad);
    const ImVec2 ts = ImGui::CalcTextSize(text, nullptr, false, w - 2 * pad - bar);
    ImGui::PopTextWrapPos();
    const ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + w, p0.y + ts.y + 2 * pad * 0.8f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, p.accent_soft, 10 * dpi);
    dl->AddRectFilled(p0, ImVec2(p0.x + bar, p1.y), p.accent, 10 * dpi, ImDrawFlags_RoundCornersLeft);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + pad + bar, p0.y + pad * 0.8f));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - 2 * pad - bar);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
    ImGui::Dummy(ImVec2(w, 0));
}

bool checkbox(const char* label, bool* v) {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4 * ImGui::GetStyle().FontScaleDpi);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 3 * ImGui::GetStyle().FontScaleDpi));
    ImGui::PushStyleColor(ImGuiCol_Border, g_pal->border_strong);
    const bool r = ImGui::Checkbox(label, v);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    return r;
}

void badge(const char* text, Badge kind) {
    const Palette& p = *g_pal;
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    ImU32 bg = p.track, fg = p.text_sub;
    if (kind == Badge::Success) bg = p.success_bg, fg = p.success_text;
    else if (kind == Badge::Danger) bg = p.danger_bg, fg = p.danger;
    else if (kind == Badge::Highlight) bg = p.highlight, fg = p.dark ? IM_COL32(0xF7, 0xDF, 0xA6, 255) : IM_COL32(0x6B, 0x4A, 0x06, 255);
    ImGui::PushFont(nullptr, 12);
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const ImVec2 pad(10 * dpi, 3 * dpi);
    const ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + ts.x + 2 * pad.x, p0.y + ts.y + 2 * pad.y);
    ImGui::GetWindowDrawList()->AddRectFilled(p0, p1, bg, (p1.y - p0.y) / 2);
    ImGui::GetWindowDrawList()->AddText(ImVec2(p0.x + pad.x, p0.y + pad.y), fg, text);
    ImGui::Dummy(ImVec2(p1.x - p0.x, p1.y - p0.y));
    ImGui::PopFont();
}

void progress(float fraction, float height) {
    const Palette& p = *g_pal;
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const float w = ImGui::GetContentRegionAvail().x, h = height * dpi;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), p.track, h / 2);
    const float f = std::clamp(fraction, 0.0f, 1.0f);
    if (f > 0) dl->AddRectFilled(p0, ImVec2(p0.x + std::max(h, w * f), p0.y + h), p.accent, h / 2);
    ImGui::Dummy(ImVec2(w, h));
}

}  // namespace iis::ui
