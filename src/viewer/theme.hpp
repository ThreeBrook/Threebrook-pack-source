// 三溪外观（三溪外观规范，与三溪寻影、官网同一套）：颜色、字体、按钮与卡片。
//
// 颜色全部来自规范的变量（浅色 / 深色两套，默认跟随系统）；标题用站酷小薇（英文 Bricolage Grotesque），
// 正文用系统的微软雅黑 UI，路径与校验值用 JetBrains Mono。字体嵌在 exe 的资源里（SIL OFL 1.1）。
#pragma once

#include "imgui.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <vector>

namespace iis::ui {

// 产品名（界面上的名字只从这里取）
inline constexpr char kProduct[] = "三溪缩影";
inline constexpr char kProductEn[] = "Threebrook Pack";
inline constexpr wchar_t kProductW[] = L"三溪缩影";
inline constexpr char kBrand[] = "三溪";
inline constexpr char kBrandEn[] = "Threebrook";
// 当前语言的产品名
const char* product();
const wchar_t* product_w();

struct Palette {
    bool dark = false;
    ImU32 bg, sidebar, card, border, border_strong, text, text_sub, text_muted, accent, on_accent, accent_soft, nav_active,
        track, offline, input_border, chip_border, success, success_bg, success_text, danger, danger_bg, danger_border,
        highlight, highlight_border;
};

enum class Mode { System, Light, Dark };

bool system_dark();  // Windows“应用模式”是否为深色
// 按模式选浅色 / 深色并设置 ImGui 的颜色与尺寸（切换立即生效）
void apply(Mode mode, float dpi);
const Palette& pal();
Mode mode();

// 字体（load_fonts 之后可用）：正文、正文粗体、标题（中文）、标题（英文）、等宽
struct Fonts {
    ImFont* body = nullptr;
    ImFont* bold = nullptr;
    ImFont* title = nullptr;
    ImFont* title_en = nullptr;
    ImFont* mono = nullptr;
};
const Fonts& fonts();
void load_fonts(HINSTANCE inst);
// 资源里的原始字节（RCDATA）；没有时为空
std::vector<uint8_t> resource(HINSTANCE inst, int id);

constexpr float kBody = 14.0f;  // 正文字号（逻辑像素）

// 标题：中文站酷小薇字号 +2、字间距 +0.5；英文 Bricolage Grotesque 粗体
void title(const char* text, float size);
void title_en(const char* text, float size);
void mono(const char* text, float size = 12.5f, ImU32 color = 0);
void text_sub(const char* text, float size = 12.5f);  // 次要说明（自动换行）
void text_colored(ImU32 color, const char* text);

// 按钮：主按钮（溪青实心，只给主要动作）、描边按钮、危险按钮。width 为 0 时按文字宽度
bool primary_button(const char* label, float width = 0);
bool outline_button(const char* label, float width = 0, bool small = false);
bool danger_button(const char* label, float width = 0);
// 分段按钮里的一段（选中时溪青浅底）
bool segment(const char* label, bool selected);

// 卡片：底色 card、描边 border、圆角 14、内距 20（浅色带阴影）。height 0 为按内容自动
void begin_card(const char* id, float height = 0);
void end_card();
// 提示条（溪青浅底）与状态徽章（成功 / 危险 / 高亮）
void hint(const char* text, float width = 0);  // width：像素，0 为占满
// 勾选框（圆角小一些，选中溪青）
bool checkbox(const char* label, bool* v);
enum class Badge { Success, Danger, Highlight, Neutral };
void badge(const char* text, Badge kind);

// 进度条：轨道 track、进度 accent、全圆角
void progress(float fraction, float height = 8);

ImU32 mix(ImU32 a, ImU32 b, float t);

}  // namespace iis::ui
