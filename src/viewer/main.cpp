// iis-viewer：三溪缩影的查看器与右键压缩 / 解压窗口。单个 exe、不写注册表、不写配置文件；只依赖 Windows 自带的 D3D11（无显卡时用 WARP 软件渲染）。
// 外观照三溪外观规范（theme.hpp）：自绘标题栏，浅色 / 深色跟随系统；中文 / English 跟随 Windows 显示语言（i18n.hpp），
// “视图”菜单里可改，命令行 --lang zh|en 也可以指定（任何窗口都适用）。
//
//   iis-viewer [文件、文件夹或 .iisz 包]
//   iis-viewer --compress <文件夹或文件>…   压缩窗口：文件夹 → 文件夹名.iisz，文件 → 文件名.iis（资源管理器右键“三溪缩影 压缩”）
//   iis-viewer --extract <.iisz、.iis 或文件夹>…   解压窗口：解到所在文件夹，不覆盖（右键“三溪缩影 解压到此处”）
//   iis-viewer --render 截图.bmp [--size 1280x800] [--scale 2] [--light | --dark] [--lang zh|en] [其他参数]    不显示窗口，渲染几帧后把界面存为 BMP 并退出
//                                  （自检、宣传截图用；--scale 按几倍像素密度画，如 2 等于 200% 缩放的屏幕）
//   iis-viewer --restore [目录或 .iis 文件]    打开“批量还原”并填好源与输出目录
//
// 资源管理器选中多项时会为每一项各启动一次本程序；--compress / --extract 的第一个进程收集其余进程的路径，放进同一个窗口。
//
// 查看：.iis、.iisz 包（像文件夹一样浏览）与原始 BMP / TIFF / RAW，PNG / JPEG 等用 Windows 自带解码器预览；缩放与平移；
// 16 位窗宽窗位；Bayer 简单去马赛克预览；目录浏览与缩略图；批量还原；把当前文件另存为原文件。
#include "image.hpp"
#include "tasks.hpp"
#include "theme.hpp"
#include "i18n.hpp"

#include "iis/iis.h"
#include "package.hpp"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "misc/cpp/imgui_stdlib.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "dwmapi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace fs = std::filesystem;
namespace ui = iis::ui;
using iis::i18n::tr;
using iis::i18n::trf;
using namespace iis::view;

namespace {

// ---------------- D3D11 ----------------
ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
ID3D11SamplerState* g_point = nullptr;
UINT g_resize_w = 0, g_resize_h = 0;
std::vector<std::wstring> g_dropped;
float g_dpi = 1;
bool g_theme_changed = false;  // 系统深浅色变了（WM_SETTINGCHANGE）
// 自绘标题栏：高度与其中可点的地方（菜单、窗口按钮，客户区像素）；其余部分按系统标题栏处理（拖动、双击最大化）
std::vector<RECT> g_title_hot;
int g_title_h = 0;

void create_rtv() {
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();
}

void release_rtv() {
    if (g_rtv) g_rtv->Release(), g_rtv = nullptr;
}

bool create_device(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                               &sd, &g_swap, &g_dev, &got, &g_ctx);
    if (FAILED(hr))  // 没有可用显卡（虚拟机、远程桌面）：用 WARP 软件渲染
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
                                           &g_swap, &g_dev, &got, &g_ctx);
    if (FAILED(hr)) return false;
    create_rtv();
    D3D11_SAMPLER_DESC s{};
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    s.MaxLOD = D3D11_FLOAT32_MAX;
    g_dev->CreateSamplerState(&s, &g_point);
    return true;
}

void cleanup_device() {
    release_rtv();
    if (g_point) g_point->Release();
    if (g_swap) g_swap->Release();
    if (g_ctx) g_ctx->Release();
    if (g_dev) g_dev->Release();
}

struct Tex {
    ID3D11ShaderResourceView* srv = nullptr;
    int w = 0, h = 0;
    void release() {
        if (srv) srv->Release();
        srv = nullptr;
    }
};

Tex make_tex(const std::vector<uint8_t>& rgba, int w, int h) {
    Tex t;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = UINT(w);
    d.Height = UINT(h);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA s{rgba.data(), UINT(w * 4), 0};
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&d, &s, &tex))) return t;
    g_dev->CreateShaderResourceView(tex, nullptr, &t.srv);
    tex->Release();
    t.w = w;
    t.h = h;
    return t;
}

ImTextureID tid(const Tex& t) { return (ImTextureID)(intptr_t)t.srv; }

// 把当前后台缓冲区存为 32 位 BMP（--render 自检用）
bool save_backbuffer(const std::wstring& path) {
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    D3D11_TEXTURE2D_DESC d;
    back->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &st))) return back->Release(), false;
    g_ctx->CopyResource(st, back);
    back->Release();
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) return st->Release(), false;
    const uint32_t w = d.Width, h = d.Height, size = 54 + w * h * 4;
    std::vector<uint8_t> b(size, 0);
    auto le = [&](size_t at, uint32_t v, int n) {
        for (int i = 0; i < n; ++i) b[at + i] = uint8_t(v >> (8 * i));
    };
    b[0] = 'B', b[1] = 'M';
    le(2, size, 4), le(10, 54, 4), le(14, 40, 4), le(18, w, 4), le(22, uint32_t(-int32_t(h)), 4), le(26, 1, 2), le(28, 32, 2);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* src = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
        uint8_t* dst = &b[54 + size_t(y) * w * 4];
        for (uint32_t x = 0; x < w; ++x) dst[x * 4] = src[x * 4 + 2], dst[x * 4 + 1] = src[x * 4 + 1], dst[x * 4 + 2] = src[x * 4], dst[x * 4 + 3] = 255;
    }
    g_ctx->Unmap(st, 0);
    st->Release();
    std::ofstream f(fs::path(path), std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
    return bool(f);
}

// 最大化时窗口会超出屏幕一圈边框的宽度，客户区要往里收
int frame_thickness(HWND h) {
    const UINT dpi = GetDpiForWindow(h);
    return GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

LRESULT hit_test(HWND h, LPARAM l) {
    POINT pt{short(LOWORD(l)), short(HIWORD(l))};
    ScreenToClient(h, &pt);
    RECT rc;
    GetClientRect(h, &rc);
    if (!IsZoomed(h)) {  // 四边与四角：调整大小
        const int b = int(6 * g_dpi);
        const bool L = pt.x < b, R = pt.x >= rc.right - b, T = pt.y < b, B = pt.y >= rc.bottom - b;
        if (T && L) return HTTOPLEFT;
        if (T && R) return HTTOPRIGHT;
        if (B && L) return HTBOTTOMLEFT;
        if (B && R) return HTBOTTOMRIGHT;
        if (L) return HTLEFT;
        if (R) return HTRIGHT;
        if (T) return HTTOP;
        if (B) return HTBOTTOM;
    }
    if (pt.y < g_title_h) {
        for (const RECT& r : g_title_hot)
            if (PtInRect(&r, pt)) return HTCLIENT;
        return HTCAPTION;
    }
    return HTCLIENT;
}

LRESULT WINAPI wndproc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {  // 自绘标题栏：去掉系统标题栏，整窗都是客户区
        case WM_NCCALCSIZE:
            if (w) {
                if (IsZoomed(h)) {
                    auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(l);
                    const int f = frame_thickness(h);
                    p->rgrc[0].left += f, p->rgrc[0].top += f, p->rgrc[0].right -= f, p->rgrc[0].bottom -= f;
                }
                return 0;
            }
            break;
        case WM_NCHITTEST:
            return hit_test(h, l);
        case WM_NCACTIVATE:  // 不让系统重画（已经去掉的）标题栏
            return DefWindowProcW(h, msg, w, -1);
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(l);
            mm->ptMinTrackSize = POINT{LONG(520 * g_dpi), LONG(360 * g_dpi)};
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (l && wcscmp(reinterpret_cast<const wchar_t*>(l), L"ImmersiveColorSet") == 0) g_theme_changed = true;
            break;
    }
    if (ImGui_ImplWin32_WndProcHandler(h, msg, w, l)) return true;
    switch (msg) {
        case WM_SIZE:
            if (w != SIZE_MINIMIZED) g_resize_w = LOWORD(l), g_resize_h = HIWORD(l);
            return 0;
        case WM_DROPFILES: {
            HDROP drop = HDROP(w);
            const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; ++i) {
                wchar_t p[32768];
                DragQueryFileW(drop, i, p, 32768);
                g_dropped.push_back(p);
            }
            DragFinish(drop);
            return 0;
        }
        case WM_SYSCOMMAND:
            if ((w & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

std::string u8(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string human(uint64_t b) {
    char buf[32];
    if (b >= 1000000000ull) std::snprintf(buf, sizeof buf, "%.2f GB", b / 1e9);
    else if (b >= 1000000) std::snprintf(buf, sizeof buf, "%.2f MB", b / 1e6);
    else if (b >= 1000) std::snprintf(buf, sizeof buf, "%.1f KB", b / 1e3);
    else std::snprintf(buf, sizeof buf, "%llu B", (unsigned long long)b);
    return buf;
}

// 系统文件 / 文件夹对话框（folder 为真时选文件夹）
std::string pick(bool folder, bool save = false, const std::wstring& name = L"") {
    std::string out;
    IFileDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dlg))))
        return out;
    DWORD opt = 0;
    dlg->GetOptions(&opt);
    dlg->SetOptions(opt | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : 0) | (save ? FOS_OVERWRITEPROMPT : 0));
    if (save && !name.empty()) dlg->SetFileName(name.c_str());
    if (!folder && !save) {
        const COMDLG_FILTERSPEC f[] = {{iis::i18n::lang() == iis::i18n::Lang::En ? L"Images and Threebrook Pack files" : L"图像与三溪缩影压缩文件", L"*.iis;*.iisz;*.bmp;*.dib;*.tif;*.tiff;*.raw;*.bin;*.dat;*.png;*.jpg;*.jpeg"}, {iis::i18n::lang() == iis::i18n::Lang::En ? L"All files" : L"所有文件", L"*.*"}};
        dlg->SetFileTypes(2, f);
    }
    if (SUCCEEDED(dlg->Show(nullptr))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                out = u8(fs::path(p));
                CoTaskMemFree(p);
            }
            item->Release();
        }
    }
    dlg->Release();
    return out;
}

// 用本程序打开压缩 / 解压窗口（各自一个进程，不影响查看器）
std::wstring quote_arg(const std::string& u8path) {
    std::wstring w = fs::u8path(u8path).wstring();
    if (!w.empty() && w.back() == L'\\') w += L'\\';  // "D:\" 的反斜杠不能转义引号
    return L"\"" + w + L"\"";
}

void spawn(const std::wstring& args) {
    wchar_t exe[32768];
    GetModuleFileNameW(nullptr, exe, 32768);
    ShellExecuteW(nullptr, L"open", exe, args.c_str(), nullptr, SW_SHOWNORMAL);
}

// ---------------- 合并资源管理器的多次调用 ----------------
// 资源管理器对选中的每一项各启动一次本程序。第一个进程建一个收集窗口，其余进程把路径发给它后退出。
constexpr wchar_t kCollectClass[] = L"IisViewerCollect";
std::vector<std::wstring>* g_collect = nullptr;
DWORD g_collect_last = 0;

LRESULT WINAPI collect_proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_COPYDATA) {
        const auto* cd = reinterpret_cast<const COPYDATASTRUCT*>(l);
        if (g_collect && cd->dwData == 1 && cd->cbData % sizeof(wchar_t) == 0) {
            g_collect->emplace_back(static_cast<const wchar_t*>(cd->lpData), cd->cbData / sizeof(wchar_t));
            g_collect_last = GetTickCount();
        }
        return TRUE;
    }
    return DefWindowProcW(h, msg, w, l);
}

// 返回 false：路径已交给先启动的进程，本进程直接退出
bool collect(HINSTANCE inst, const std::wstring& mode, std::vector<std::wstring>& paths) {
    const std::wstring title = L"IIS-" + mode;
    HANDLE mtx = CreateMutexW(nullptr, FALSE, (L"Local\\IisViewerCollect-" + mode).c_str());
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        for (int i = 0; i < 60; ++i) {  // 等先启动的进程建好收集窗口（最多 3 秒）
            if (HWND w = FindWindowExW(HWND_MESSAGE, nullptr, kCollectClass, title.c_str())) {
                bool sent = true;
                for (const auto& p : paths) {
                    COPYDATASTRUCT cd{1, DWORD(p.size() * sizeof(wchar_t)), const_cast<wchar_t*>(p.data())};
                    DWORD_PTR res = 0;
                    sent = sent && SendMessageTimeoutW(w, WM_COPYDATA, 0, LPARAM(&cd), SMTO_ABORTIFHUNG, 2000, &res) && res;
                }
                if (sent) {
                    if (mtx) CloseHandle(mtx);
                    return false;
                }
                break;  // 对方已不再收集：自己处理
            }
            Sleep(50);
        }
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = collect_proc;
    wc.hInstance = inst;
    wc.lpszClassName = kCollectClass;
    RegisterClassExW(&wc);
    HWND w = CreateWindowExW(0, kCollectClass, title.c_str(), 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
    g_collect = &paths;
    const DWORD t0 = g_collect_last = GetTickCount();
    while (GetTickCount() - g_collect_last < 600 && GetTickCount() - t0 < 5000) {  // 0.6 秒没有新路径为止
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
    }
    DestroyWindow(w);
    g_collect = nullptr;
    if (mtx) CloseHandle(mtx);
    std::vector<std::wstring> uniq;  // 去重，保持顺序
    for (const auto& p : paths)
        if (std::none_of(uniq.begin(), uniq.end(), [&](const std::wstring& q) { return _wcsicmp(p.c_str(), q.c_str()) == 0; }))
            uniq.push_back(p);
    paths = uniq;
    return true;
}

// ---------------- 自绘标题栏（三溪规范：高 36，左上图标 + 产品名，右边最小化 / 最大化 / 关闭） ----------------
// 标题栏里可点的地方（菜单、窗口按钮），其余部分是“标题”：拖动窗口、双击最大化（WM_NCHITTEST 用）
void hot(const ImVec2& a, const ImVec2& b) { g_title_hot.push_back(RECT{LONG(a.x), LONG(a.y), LONG(b.x), LONG(b.y)}); }

// 标题栏上的文字按钮（菜单）：悬停时浅底
bool title_button(const char* label, float h) {
    const ui::Palette& p = ui::pal();
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const ImVec2 ts = ImGui::CalcTextSize(label, nullptr, true);
    const ImVec2 p0 = ImGui::GetCursorScreenPos(), size(ts.x + 20 * dpi, h);
    const bool r = ImGui::InvisibleButton(label, size);
    const bool on = ImGui::IsItemHovered() || ImGui::IsPopupOpen(label);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (on) dl->AddRectFilled(ImVec2(p0.x, p0.y + 5 * dpi), ImVec2(p0.x + size.x, p0.y + h - 5 * dpi), p.accent_soft, 6 * dpi);
    dl->AddText(ImVec2(p0.x + 10 * dpi, p0.y + (h - ts.y) / 2), p.text_sub, label);
    hot(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    return r;
}

// 窗口按钮：0 最小化，1 最大化 / 还原，2 关闭
bool window_button(int kind, float h, bool maximized) {
    const ui::Palette& p = ui::pal();
    const float dpi = ImGui::GetStyle().FontScaleDpi, w = 46 * dpi;
    ImGui::PushID(kind);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const bool r = ImGui::InvisibleButton("wb", ImVec2(w, h));
    const bool hov = ImGui::IsItemHovered();
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 fg = p.text_sub;
    if (hov) {
        const ImU32 bg = kind == 2 ? IM_COL32(0xC4, 0x2B, 0x1C, 255) : p.accent_soft;  // 关闭悬停与 Windows 一致
        dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), bg);
        if (kind == 2) fg = IM_COL32_WHITE;
    }
    const float cx = p0.x + w / 2, cy = p0.y + h / 2, s = 5 * dpi, t = 1.0f * dpi;
    if (kind == 0) dl->AddLine(ImVec2(cx - s, cy), ImVec2(cx + s, cy), fg, t);
    else if (kind == 1 && !maximized) dl->AddRect(ImVec2(cx - s, cy - s), ImVec2(cx + s, cy + s), fg, 1 * dpi, t);
    else if (kind == 1) {
        dl->AddRect(ImVec2(cx - s, cy - s + 2 * dpi), ImVec2(cx + s - 2 * dpi, cy + s), fg, 1 * dpi, t);
        dl->AddLine(ImVec2(cx - s + 2 * dpi, cy - s), ImVec2(cx + s, cy - s), fg, t);
        dl->AddLine(ImVec2(cx + s, cy - s), ImVec2(cx + s, cy + s - 2 * dpi), fg, t);
    } else {
        dl->AddLine(ImVec2(cx - s, cy - s), ImVec2(cx + s, cy + s), fg, t);
        dl->AddLine(ImVec2(cx - s, cy + s), ImVec2(cx + s, cy - s), fg, t);
    }
    hot(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    return r;
}

// 画标题栏；menus 在产品名之后画菜单按钮（可空）。返回标题栏高度
float draw_titlebar(HWND hwnd, const Tex& icon, const char* subtitle, bool resizable, const std::function<void(float)>& menus) {
    const ui::Palette& p = ui::pal();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float dpi = ImGui::GetStyle().FontScaleDpi, h = 36 * dpi;
    g_title_hot.clear();
    g_title_h = int(h);
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, p.bg);
    ImGui::Begin("##titlebar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetWindowPos();
    float x = 12 * dpi;
    if (icon.srv) dl->AddImage(tid(icon), ImVec2(o.x + x, o.y + 9 * dpi), ImVec2(o.x + x + 18 * dpi, o.y + 27 * dpi));
    x += 26 * dpi;
    const bool en = iis::i18n::lang() == iis::i18n::Lang::En;
    ImGui::PushFont(en ? ui::fonts().title_en : ui::fonts().title, en ? 14.0f : 15.0f);
    const ImVec2 ns = ImGui::CalcTextSize(ui::product());
    dl->AddText(ImVec2(o.x + x, o.y + (h - ns.y) / 2), p.text, ui::product());
    ImGui::PopFont();
    x += ns.x + 8 * dpi;
    if (subtitle && *subtitle) {
        const ImVec2 ss = ImGui::CalcTextSize(subtitle);
        dl->AddText(ImVec2(o.x + x, o.y + (h - ss.y) / 2), p.text_muted, subtitle);
        x += ss.x + 8 * dpi;
    }
    if (menus) {
        ImGui::SetCursorPos(ImVec2(x + 8 * dpi, 0));
        menus(h);
    }
    const bool maxed = IsZoomed(hwnd) != 0;
    ImGui::SetCursorPos(ImVec2(vp->Size.x - 46 * dpi * (resizable ? 3 : 2), 0));
    if (window_button(0, h, maxed)) ShowWindow(hwnd, SW_MINIMIZE);
    if (resizable) {
        ImGui::SameLine();
        if (window_button(1, h, maxed)) ShowWindow(hwnd, maxed ? SW_RESTORE : SW_MAXIMIZE);
    }
    ImGui::SameLine();
    if (window_button(2, h, maxed)) PostMessageW(hwnd, WM_CLOSE, 0, 0);
    dl->AddLine(ImVec2(o.x, o.y + h - 1), ImVec2(o.x + vp->Size.x, o.y + h - 1), p.border);
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    return h;
}

// ---------------- 压缩 / 解压窗口 ----------------
struct Task {
    bool compress = true;
    std::vector<std::string> paths, preview;
    bool delete_orig = false, started = false;
    std::atomic<bool> running{false}, finished{false}, cancel{false};
    TaskState st;
    std::thread th;
    std::chrono::steady_clock::time_point t0;
    double seconds = 0;
    void start() {
        started = true;
        running = true;
        t0 = std::chrono::steady_clock::now();
        th = std::thread([this] {
            if (compress) run_compress(paths, delete_orig, cancel, st);
            else run_extract(paths, cancel, st);
            seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            finished = true;
            running = false;
        });
    }
};

// 右对齐的一排按钮的起点（widths 为逻辑像素）
void align_right(std::initializer_list<float> widths) {
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    float w = 0;
    for (float x : widths) w += x * dpi;
    w += ImGui::GetStyle().ItemSpacing.x * float(widths.size() - 1);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - w);
}

void draw_task(Task& t, float dpi, bool& done) {
    const ui::Palette& p = ui::pal();
    const float bottom = 40 * dpi + ImGui::GetStyle().WindowPadding.y;  // 底部按钮行
    if (!t.started) {  // 压缩：开始前列出要做的事，可选删除原文件
        ui::title(trf("压缩 %d 项", t.paths.size() == 1 ? "Compress %d item" : "Compress %d items", int(t.paths.size())).c_str(), 20);
        ui::text_sub(tr("文件夹打成一个 .iisz 包，单个图像压成 .iis。图像无损压缩：解压出的文件与原文件逐字节相同；"
                        "压不小的文件（如 JPG、PNG）原样存放。",
                        "A folder becomes one .iisz package and a single image becomes an .iis. Lossless: extracted files are "
                        "byte-for-byte identical to the originals; files that would not get smaller (such as JPG and PNG) are stored as they are."));
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, p.card);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10 * dpi);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * dpi, 10 * dpi));
        const float list_h = ImGui::GetContentRegionAvail().y - bottom - ImGui::GetFrameHeightWithSpacing() * 2.2f;
        ImGui::BeginChild("items", ImVec2(0, std::max(60 * dpi, list_h)), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PushFont(ui::fonts().mono, 12.5f);
        for (const auto& l : t.preview) ImGui::TextUnformatted(l.c_str());
        ImGui::PopFont();
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        ui::checkbox(tr("压缩并核对后删除原文件", "Delete the originals after compressing and checking"), &t.delete_orig);
        ui::text_sub(t.delete_orig ? tr("包写好并逐个核对后，删除内容未变的原文件；被占用、只读或已修改的原文件保留。", "After the package is written and every file checked, originals that have not changed are deleted; files that are in use, read-only or modified are kept.")
                                   : tr("默认保留原文件（和 ZIP 一样）。", "Originals are kept by default, just like ZIP."));
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - bottom);
        align_right({90, 120});
        if (ui::outline_button(tr("取消", "Cancel"), 90)) done = true;
        ImGui::SameLine();
        if (ui::primary_button(tr("开始压缩", "Compress"), 120)) t.start();
        return;
    }
    std::lock_guard<std::mutex> lk(t.st.mu);
    const TaskState& s = t.st;
    const float frac = t.finished ? 1.0f : s.items ? float((double(s.item) + s.fraction) / double(s.items)) : 0.0f;
    if (!t.finished) {
        ui::title(t.compress ? tr("正在压缩…", "Compressing…") : tr("正在解压…", "Extracting…"), 20);
        const std::string label = s.items > 1 ? trf("第 %d / %d 项", "Item %d of %d", int(std::min(s.item + 1, s.items)), int(s.items)) : "";
        if (!label.empty()) ui::text_sub(label.c_str());
        ImGui::PushFont(ui::fonts().mono, 12);
        ImGui::PushStyleColor(ImGuiCol_Text, p.text_muted);
        ImGui::TextUnformatted(s.current.empty() ? " " : s.current.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
    } else {
        ui::title(t.compress ? tr("压缩完成", "Compression finished") : tr("解压完成", "Extraction finished"), 20);
        if (s.cancelled) ui::badge(tr("已取消", "Cancelled"), ui::Badge::Highlight);
        else if (s.failed) ui::badge(trf("有 %d 个失败，见下", "%d failed, see below", s.failed).c_str(), ui::Badge::Danger);
        else ui::badge(tr("全部核对无误", "All files checked OK"), ui::Badge::Success);
        char buf[200];
        if (t.compress && s.out_bytes)
            std::snprintf(buf, sizeof buf, tr("压缩前 %s → 压缩后 %s（%.2f 倍），用时 %.1f 秒", "%s before → %s after (%.2f×), took %.1f s"), human(s.in_bytes).c_str(),
                          human(s.out_bytes).c_str(), double(s.in_bytes) / double(s.out_bytes), t.seconds);
        else
            std::snprintf(buf, sizeof buf, tr("用时 %.1f 秒", "Took %.1f s"), t.seconds);
        ImGui::TextUnformatted(buf);
    }
    ui::progress(frac);
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, p.card);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10 * dpi);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * dpi, 10 * dpi));
    ImGui::BeginChild("log", ImVec2(0, ImGui::GetContentRegionAvail().y - bottom), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    for (const auto& l : s.lines) ImGui::TextWrapped("%s", l.c_str());
    for (const auto& l : s.problems) ui::text_colored(p.danger, l.c_str());
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - bottom);
    if (!t.finished) {
        align_right({110});
        ImGui::BeginDisabled(t.cancel);
        if (ui::outline_button(t.cancel ? tr("正在取消…", "Cancelling…") : tr("取消", "Cancel"), 110)) t.cancel = true;
        ImGui::EndDisabled();
    } else {
        align_right({120, 100});
        ImGui::BeginDisabled(s.open_dir.empty());
        if (ui::outline_button(tr("打开文件夹", "Open folder"), 120))
            ShellExecuteW(nullptr, L"open", fs::u8path(s.open_dir).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ui::primary_button(tr("完成", "Done"), 100)) done = true;
    }
}

// ---------------- 应用状态 ----------------
struct Entry {
    std::string name, path;
    std::string vdir;  // 包内的文件夹（浏览 .iisz 时）
    bool dir = false;
    uint64_t size = 0;
};

struct Thumbs {  // 后台生成缩略图（.iisz 包为前 4 张图的拼图）
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::string> todo;
    std::set<std::string> queued;
    std::vector<std::tuple<std::string, std::vector<uint8_t>, int, int>> done;
    bool stop = false;
    std::atomic<int> pending{0};  // 已请求、结果还没上传为纹理的数量
    std::thread th;
    void start() {
        th = std::thread([this] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // PNG、JPEG 用 WIC 解码
            for (;;) {
                std::string p;
                {
                    std::unique_lock<std::mutex> lk(mu);
                    cv.wait(lk, [&] { return stop || !todo.empty(); });
                    if (stop) break;
                    p = todo.front();
                    todo.pop_front();
                }
                std::vector<uint8_t> rgba;
                int w = 0, h = 0;
                if (p.find('|') == std::string::npos && iis::pkg::is_package_name(p)) {
                    if (const auto pk = open_package(p)) package_mosaic(*pk, 160, rgba, w, h);
                } else {
                    const Loaded l = load_any(p);
                    if (l.ok && !l.pages.empty()) thumbnail(l.pages[0], 160, rgba, w, h);
                }
                std::lock_guard<std::mutex> lk(mu);
                done.emplace_back(p, std::move(rgba), w, h);
            }
            CoUninitialize();
        });
    }
    void request(const std::string& p) {
        std::lock_guard<std::mutex> lk(mu);
        if (!queued.insert(p).second) return;
        ++pending;
        todo.push_back(p);
        cv.notify_one();
    }
    void clear_pending() {
        std::lock_guard<std::mutex> lk(mu);
        for (const auto& p : todo) queued.erase(p);
        pending -= int(todo.size());
        todo.clear();
    }
    void shutdown() {
        {
            std::lock_guard<std::mutex> lk(mu);
            stop = true;
        }
        cv.notify_all();
        if (th.joinable()) th.join();
    }
};

struct Batch {  // 批量还原
    std::string src, out;
    bool recursive = true;
    std::atomic<bool> running{false};
    std::atomic<int> total{0}, ok{0}, skipped{0}, failed{0};
    std::mutex mu;
    std::vector<std::string> errors;
    std::thread th;
    bool finished = false;   // 本次打开对话框后完成过一次
    std::string out_auto;    // 自动给出的输出目录（用户没改时随源目录变化）
    std::string watch_warn;  // 输出目录在后台服务（0.1 版）的监控目录内时的提醒
    void run() {
        if (th.joinable()) th.join();
        total = ok = skipped = failed = 0;
        errors.clear();
        running = true;
        finished = false;
        th = std::thread([this] {
            const RestoreResult r = restore_tree(src, out, recursive, [this](const RestoreResult& p) {
                total = p.total, ok = p.ok, skipped = p.skipped, failed = p.failed;
                std::lock_guard<std::mutex> lk(mu);
                errors = p.errors;
            });
            finished = true;
            running = false;
        });
    }
    // 默认输出：桌面\还原的图像\源目录名
    void suggest_out() {
        if (src.empty() || (!out.empty() && out != out_auto)) return;  // 用户改过就不动
        PWSTR desk = nullptr;
        fs::path base;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk))) base = desk;
        CoTaskMemFree(desk);
        fs::path s = fs::u8path(src);
        std::error_code ec;
        if (fs::is_regular_file(s, ec)) s = s.parent_path();
        std::wstring name = s.filename().wstring();
        if (name.empty()) name = s.root_name().wstring().substr(0, 1) + (iis::i18n::lang() == iis::i18n::Lang::En ? L" drive" : L"盘");  // D:\ → D盘
        out_auto = out = base.empty() ? "" : u8(base / (iis::i18n::lang() == iis::i18n::Lang::En ? L"Restored images" : L"还原的图像") / name);
    }
    void check_out() { watch_warn = out.empty() ? "" : watched_root(out); }
};

struct App {
    std::string folder;
    std::vector<Entry> entries;
    std::string path;  // 当前文件
    Loaded img;
    std::future<Loaded> loading;
    bool is_loading = false;
    int page = 0;
    Window win;
    bool demosaic = true, white_balance = true;
    int bayer_sel = 0;  // 0 自动，1 不去马赛克，2.. 指定排列
    Tex tex;
    int tex_scale = 1;  // 超过 16384 时抽稀显示
    bool tex_dirty = false;
    double last_tex = 0;
    ImU32 ambient = 0;  // 画面的平均颜色（看图区的底色由它与页面底色混合，不用纯黑）
    float zoom = 1, pan_x = 0, pan_y = 0;
    bool fit = true;
    RawParams raw;
    bool want_raw_dialog = false, want_batch = false, want_about = false;
    bool thumbs_view = true;
    bool show_bayer = false;  // 没检测到 Bayer 的灰度图也显示 Bayer 选项
    bool editing_path = false;  // 路径框处于编辑状态（否则显示为面包屑）
    std::map<std::string, Tex> thumb_tex;
    Thumbs thumbs;
    Batch batch;
    std::string message;

    const char* bayer_names[6] = {"自动", "不去马赛克", "RGGB", "GRBG", "GBRG", "BGGR"};

    // 浏览 .iisz 包：像文件夹一样，pkg_dir 为包内当前文件夹（以 '/' 结尾，顶层为空）
    std::shared_ptr<const iis::pkg::Reader> pkg;
    std::string pkg_path, pkg_dir;

    void open_package_view(const std::string& p) {
        std::string why;
        auto r = open_package(p, &why);
        if (!r) {
            message = tr("无法打开包：", "Cannot open the package: ") + why;
            return;
        }
        pkg = r;
        pkg_path = p;
        pkg_dir.clear();
        std::set<std::string> tops;  // 只有一个顶层文件夹时直接进入
        bool loose = false;
        for (const auto& e : r->entries()) {
            if (!e.bad.empty()) continue;
            const size_t k = e.path.find('/');
            tops.insert(e.path.substr(0, k));
            loose = loose || (!e.dir && k == std::string::npos);
        }
        if (tops.size() == 1 && !loose) pkg_dir = *tops.begin() + "/";
        list_package();
    }

    void list_package() {
        folder = pkg_path + (pkg_dir.empty() ? "" : " › " + pkg_dir.substr(0, pkg_dir.size() - 1));
        entries.clear();
        thumbs.clear_pending();
        std::set<std::string> dirs;
        const auto& es = pkg->entries();
        for (size_t i = 0; i < es.size(); ++i) {
            const auto& e = es[i];
            if (!e.bad.empty()) continue;
            const std::string p = e.dir ? e.path + "/" : e.path;
            if (p.size() <= pkg_dir.size() || p.compare(0, pkg_dir.size(), pkg_dir) != 0) continue;
            const std::string rest = p.substr(pkg_dir.size());
            const size_t k = rest.find('/');
            if (k != std::string::npos) {
                dirs.insert(rest.substr(0, k));
                continue;
            }
            Entry en;
            en.name = rest;
            en.path = package_item(pkg_path, i);
            en.size = e.orig_size;
            entries.push_back(en);
        }
        for (const auto& d : dirs) {
            Entry en;
            en.name = d;
            en.dir = true;
            en.vdir = pkg_dir + d + "/";
            entries.push_back(en);
        }
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            return a.dir != b.dir ? a.dir : a.name < b.name;
        });
    }

    void enter(const Entry& e) {
        if (!e.vdir.empty()) pkg_dir = e.vdir, list_package();
        else if (e.dir) open_folder(e.path);
        else open(e.path);
    }

    void up() {
        if (!pkg) {
            if (!folder.empty()) open_folder(u8(fs::u8path(folder).parent_path()));
        } else if (!pkg_dir.empty()) {
            pkg_dir.pop_back();
            const size_t k = pkg_dir.rfind('/');
            pkg_dir = k == std::string::npos ? "" : pkg_dir.substr(0, k + 1);
            list_package();
        } else {
            open_folder(u8(fs::u8path(pkg_path).parent_path()));
        }
    }

    void open_folder(const std::string& f) {
        pkg.reset();
        pkg_path.clear();
        pkg_dir.clear();
        std::error_code aec;
        folder = u8(fs::absolute(fs::u8path(f), aec));
        entries.clear();
        thumbs.clear_pending();
        std::error_code ec;
        const fs::path p = fs::u8path(f);
        for (const auto& e : fs::directory_iterator(p, fs::directory_options::skip_permission_denied, ec)) {
            Entry en;
            en.name = u8(e.path().filename());
            en.path = u8(e.path());
            en.dir = e.is_directory(ec);
            if (!en.dir) {
                if (!listable(en.name)) continue;
                en.size = e.file_size(ec);
            }
            entries.push_back(en);
        }
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            return a.dir != b.dir ? a.dir : a.name < b.name;
        });
    }

    void open(const std::string& p, const RawParams* rp = nullptr) {
        std::error_code ec;
        const bool virt = p.find('|') != std::string::npos;  // 包内文件
        if (!virt && fs::is_directory(fs::u8path(p), ec)) {
            open_folder(p);
            return;
        }
        if (!virt && iis::pkg::is_package_name(p)) {
            open_package_view(p);
            return;
        }
        if (virt && (!pkg || pkg_path != p.substr(0, p.rfind('|')))) open_package_view(p.substr(0, p.rfind('|')));  // 命令行直接给包内文件
        path = p;
        if (folder.empty() && !virt) open_folder(u8(fs::u8path(p).parent_path()));
        is_loading = true;
        const RawParams copy = rp ? *rp : RawParams{};
        const bool has = rp != nullptr;
        loading = std::async(std::launch::async, [p, copy, has] { return load_any(p, has ? &copy : nullptr); });
    }

    void poll_loading() {
        if (!is_loading || loading.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        is_loading = false;
        img = loading.get();
        page = 0;
        fit = true;
        bayer_sel = 0;
        show_bayer = false;
        if (img.ok) {
            win = auto_window(img.pages[0]);
            tex_dirty = true;
            message.clear();
        } else {
            tex.release();
            message = img.error;
            if (img.need_raw_params) want_raw_dialog = true;
        }
    }

    std::string bayer() const {
        if (!img.ok) return "";
        if (bayer_sel == 0) return img.pages[size_t(page)].bayer;
        if (bayer_sel == 1) return "";
        return bayer_names[bayer_sel];
    }

    void rebuild_texture(double now) {
        if (!tex_dirty || !img.ok || now - last_tex < 0.08) return;
        tex_dirty = false;
        last_tex = now;
        const Image& src = img.pages[size_t(page)];
        // 纹理最大 16384：超过时按整数倍抽稀，仅影响显示
        tex_scale = int(std::max<uint32_t>(1, (std::max(src.width, src.height) + 16383) / 16384));
        std::vector<uint8_t> rgba;
        if (tex_scale == 1) {
            to_rgba(src, win, demosaic, bayer(), rgba, white_balance);
        } else {
            std::vector<uint8_t> full;
            to_rgba(src, win, demosaic, bayer(), full, white_balance);
            const int w = int(src.width) / tex_scale, h = int(src.height) / tex_scale;
            rgba.resize(size_t(w) * h * 4);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    std::memcpy(&rgba[(size_t(y) * w + x) * 4], &full[(size_t(y) * tex_scale * src.width + size_t(x) * tex_scale) * 4], 4);
        }
        uint64_t sum[3] = {0, 0, 0}, n = 0;  // 平均颜色（稀疏取样）
        for (size_t i = 0; i < rgba.size(); i += 4 * 97, ++n)
            for (int k = 0; k < 3; ++k) sum[k] += rgba[i + k];
        ambient = n ? IM_COL32(int(sum[0] / n), int(sum[1] / n), int(sum[2] / n), 255) : 0;
        tex.release();
        tex = make_tex(rgba, int(src.width) / tex_scale, int(src.height) / tex_scale);
    }
};

// 文件夹图标（缩略图格子里）
void folder_glyph(ImDrawList* dl, ImVec2 c, float s, ImU32 col, ImU32 tab) {
    const ImVec2 a(c.x - s * 0.5f, c.y - s * 0.32f), b(c.x + s * 0.5f, c.y + s * 0.38f);
    dl->AddRectFilled(ImVec2(a.x, a.y - s * 0.1f), ImVec2(a.x + s * 0.42f, a.y + s * 0.1f), tab, s * 0.06f);
    dl->AddRectFilled(a, b, col, s * 0.08f);
}

// 输入框：规范的 inputBorder 描边、圆角 10
bool input(const char* id, std::string* s, ImGuiInputTextFlags flags = 0) {
    ImGui::PushStyleColor(ImGuiCol_Border, ui::pal().input_border);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10 * ImGui::GetStyle().FontScaleDpi);
    const bool r = ImGui::InputText(id, s, flags);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    return r;
}

// 当前路径显示为“上级 › 当前”面包屑，放不下时从前面省略；点击后改为可编辑的输入框
bool path_crumbs(const std::string& path, float dpi) {
    const ui::Palette& p = ui::pal();
    std::vector<std::string> parts;
    for (const auto& seg : fs::u8path(path)) {
        const std::string s = u8(seg);
        if (s.empty() || s == "\\" || s == "/") continue;
        for (size_t at = 0;;) {  // 包内路径用“ › ”连接
            const size_t k = s.find(" › ", at);
            parts.push_back(s.substr(at, k == std::string::npos ? std::string::npos : k - at));
            if (k == std::string::npos) break;
            at = k + std::strlen(" › ");
        }
    }
    const float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetFrameHeight();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##crumbs", ImVec2(w, h));
    const bool hov = ImGui::IsItemHovered();
    if (hov) ImGui::SetTooltip(tr("%s\n点击可编辑路径", "%s\nClick to edit the path"), path.c_str());
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), p.card, 10 * dpi);
    dl->AddRect(p0, ImVec2(p0.x + w, p0.y + h), hov ? p.accent : p.input_border, 10 * dpi);
    const float pad = 12 * dpi, maxw = w - 2 * pad;
    std::string text;
    for (size_t k = parts.size(); k-- > 0;) {  // 从最后一级往前加，放不下就停
        const std::string t = parts[k] + (text.empty() ? "" : "  ›  " + text);
        if (!text.empty() && ImGui::CalcTextSize(("…  ›  " + t).c_str()).x > maxw) {
            text = "…  ›  " + text;
            break;
        }
        text = t;
    }
    dl->PushClipRect(p0, ImVec2(p0.x + w - pad / 2, p0.y + h), true);
    dl->AddText(ImVec2(p0.x + pad, p0.y + (h - ImGui::GetFontSize()) / 2), p.text, text.c_str());
    dl->PopClipRect();
    return clicked;
}

void draw_folder(App& a, float dpi) {
    const ui::Palette& p = ui::pal();
    ImGui::PushFont(ui::fonts().bold, 13);
    ImGui::PushStyleColor(ImGuiCol_Text, p.text_sub);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(a.pkg ? tr("包内文件", "In package") : tr("文件夹", "Folder"));
    ImGui::PopStyleColor();
    ImGui::PopFont();
    // 同一行右边：网格 / 列表
    ImGui::SameLine();
    const float seg_w = ImGui::CalcTextSize(tr("网格", "Grid")).x + ImGui::CalcTextSize(tr("列表", "List")).x + 58 * dpi;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - seg_w));
    if (ui::segment(tr("网格", "Grid"), a.thumbs_view)) a.thumbs_view = true;
    ImGui::SameLine(0, 2 * dpi);
    if (ui::segment(tr("列表", "List"), !a.thumbs_view)) a.thumbs_view = false;
    ImGui::SetNextItemWidth(-FLT_MIN);
    std::string f = a.folder;
    if (a.editing_path || a.folder.empty()) {
        if (a.editing_path && !ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
        if (input("##folder", &f, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) a.open(f), a.editing_path = false;
        if (a.editing_path && ImGui::IsItemDeactivated()) a.editing_path = false;
    } else if (path_crumbs(a.folder, dpi)) {
        a.editing_path = true;
    }
    if (ui::outline_button(tr("上一级", "Up"), 0, true)) a.up();
    ImGui::SameLine();
    if (ui::outline_button(tr("选择文件夹…", "Choose folder…"), 0, true)) {
        const std::string pth = pick(true);
        if (!pth.empty()) a.open_folder(pth);
    }
    if (a.pkg && ui::outline_button(tr("全部解压到此处", "Extract all here"), 0, true)) spawn(L"--extract " + quote_arg(a.pkg_path));
    ImGui::Spacing();
    ImGui::BeginChild("files");
    if (a.entries.empty()) ui::text_sub(a.folder.empty() ? tr("还没有打开文件夹。", "No folder open yet.") : tr("这里没有图像。", "No images here."));
    if (a.thumbs_view) {
        const float cell = 124 * dpi, gap = 8 * dpi;
        const int cols = std::max(1, int((ImGui::GetContentRegionAvail().x + gap) / (cell + gap)));
        const float cw = (ImGui::GetContentRegionAvail().x - gap * (cols - 1)) / cols;
        int col = 0;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
        for (const auto& e : a.entries) {
            ImGui::PushID(e.path.empty() ? e.vdir.c_str() : e.path.c_str());
            const ImVec2 p0 = ImGui::GetCursorScreenPos(), box(cw, cw * 0.78f + 30 * dpi);
            const bool sel = !e.dir && e.path == a.path;
            if (ImGui::InvisibleButton("t", box)) a.enter(e);
            const bool hov = ImGui::IsItemHovered();
            if (hov) ImGui::SetTooltip("%s%s", e.name.c_str(), e.dir ? "" : ("\n" + human(e.size)).c_str());
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 p1(p0.x + box.x, p0.y + box.y);
            dl->AddRectFilled(p0, p1, sel ? p.accent_soft : hov ? ui::mix(p.card, p.accent_soft, 0.5f) : p.card, 10 * dpi);
            dl->AddRect(p0, p1, sel ? p.accent : p.border, 10 * dpi, sel ? 2 * dpi : 1.0f);
            const ImVec2 i0(p0.x + 8 * dpi, p0.y + 8 * dpi), i1(p1.x - 8 * dpi, p0.y + cw * 0.78f - 2 * dpi);
            const ImVec2 ic((i0.x + i1.x) / 2, (i0.y + i1.y) / 2);
            if (e.dir) {
                folder_glyph(dl, ic, (i1.y - i0.y) * 0.62f, ui::mix(p.accent, p.card, 0.45f), p.accent);
            } else if (ImGui::IsItemVisible()) {
                auto it = a.thumb_tex.find(e.path);
                if (it == a.thumb_tex.end()) {
                    a.thumbs.request(e.path);
                } else if (it->second.srv) {
                    const float s = std::min((i1.x - i0.x) / it->second.w, (i1.y - i0.y) / it->second.h);
                    const float w = it->second.w * s, h = it->second.h * s;
                    const ImVec2 q0(ic.x - w / 2, ic.y - h / 2);
                    dl->AddImageRounded(tid(it->second), q0, ImVec2(q0.x + w, q0.y + h), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 4 * dpi);
                } else {
                    const char* t = iis::pkg::is_package_name(e.name) ? tr("包里没有图像", "No images in package") : tr("无预览", "No preview");
                    const ImVec2 ts = ImGui::CalcTextSize(t);
                    dl->AddText(ImVec2(ic.x - ts.x / 2, ic.y - ts.y / 2), p.text_muted, t);
                }
            }
            // 名字：一行，放不下用省略号
            ImGui::PushFont(nullptr, 12);
            const float maxw = box.x - 16 * dpi;
            std::string n = e.name;
            if (ImGui::CalcTextSize(n.c_str()).x > maxw) {
                while (!n.empty() && ImGui::CalcTextSize((n + "…").c_str()).x > maxw) {
                    n.pop_back();
                    while (!n.empty() && (uint8_t(n.back()) & 0xC0) == 0x80) n.pop_back();  // 不切断 UTF-8 字符
                }
                n += "…";
            }
            const ImVec2 ts = ImGui::CalcTextSize(n.c_str());
            dl->AddText(ImVec2(p0.x + (box.x - ts.x) / 2, p1.y - 22 * dpi), sel ? p.accent : p.text, n.c_str());
            ImGui::PopFont();
            ImGui::PopID();
            if (++col < cols) ImGui::SameLine();
            else col = 0;
        }
        ImGui::PopStyleVar();
    } else {
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0, 0.5f));
        for (const auto& e : a.entries) {
            ImGui::PushID(e.path.empty() ? e.vdir.c_str() : e.path.c_str());
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##row", !e.dir && e.path == a.path, 0, ImVec2(0, 28 * dpi))) a.enter(e);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (e.dir) folder_glyph(dl, ImVec2(p0.x + 12 * dpi, p0.y + 14 * dpi), 16 * dpi, ui::mix(p.accent, p.card, 0.45f), p.accent);
            const float ty = p0.y + (28 * dpi - ImGui::GetFontSize()) / 2;
            dl->AddText(ImVec2(p0.x + (e.dir ? 28 : 6) * dpi, ty), p.text, e.name.c_str());
            if (!e.dir) {
                const std::string sz = human(e.size);
                const float sw = ImGui::CalcTextSize(sz.c_str()).x;
                dl->AddText(ImVec2(p0.x + ImGui::GetContentRegionAvail().x - sw - 6 * dpi, ty), p.text_muted, sz.c_str());
            }
            ImGui::PopID();
        }
        ImGui::PopStyleVar();
    }
    ImGui::EndChild();
}

void draw_empty(App& a, const Tex& icon, ImVec2 origin, ImVec2 avail, float dpi) {
    const ui::Palette& p = ui::pal();
    const float w = std::min(avail.x - 48 * dpi, 420 * dpi);
    ImGui::SetCursorScreenPos(ImVec2(origin.x + (avail.x - w) / 2, origin.y + avail.y * 0.28f));
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
    if (icon.srv) {
        ImGui::Image(tid(icon), ImVec2(56 * dpi, 56 * dpi));
        ImGui::Spacing();
    }
    if (a.is_loading) {
        ui::title(tr("正在读取…", "Reading…"), 24);
    } else if (!a.message.empty()) {
        ui::title(tr("打不开这个文件", "Cannot open this file"), 24);
        ui::text_colored(p.danger, a.message.c_str());
        if (a.img.need_raw_params && ui::outline_button(tr("输入 RAW 参数…", "Enter RAW parameters…"))) a.want_raw_dialog = true;
    } else {
        ui::title(tr("看图、压缩、解压", "View, compress, extract"), 24);
        ui::text_sub(tr("打开 .iis、.iisz 包或 BMP / TIFF / RAW / PNG / JPEG。也可以把文件或文件夹拖进窗口。", "Open .iis files, .iisz packages or BMP / TIFF / RAW / PNG / JPEG. You can also drag files or folders into the window."), 13);
        ImGui::Spacing();
        if (ui::primary_button(tr("打开文件…", "Open file…"))) {
            const std::string pth = pick(false);
            if (!pth.empty()) a.open(pth);
        }
        ImGui::SameLine();
        if (ui::outline_button(tr("打开文件夹…", "Open folder…"))) {
            const std::string pth = pick(true);
            if (!pth.empty()) a.open_folder(pth);
        }
        ImGui::Spacing();
        ui::hint(tr("压缩：在资源管理器里右键文件夹 →“三溪缩影 压缩”，或用菜单“文件 → 压缩文件夹…”。", "To compress: right-click a folder in File Explorer → “Threebrook Pack: Compress”, or use File → Compress folder…."), w);
    }
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
}

void draw_image(App& a, const Tex& icon, float dpi) {
    const ui::Palette& p = ui::pal();
    const ImVec2 origin = ImGui::GetCursorScreenPos(), avail = ImGui::GetContentRegionAvail();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool showing = a.img.ok && a.tex.srv && !a.is_loading;
    // 看图时底色取画面平均颜色与页面底色的混合（规范 §6：不用纯黑）
    dl->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + avail.y),
                      showing && a.ambient ? ui::mix(p.bg, a.ambient, p.dark ? 0.22f : 0.16f) : p.bg);
    if (!showing) {
        draw_empty(a, icon, origin, avail, dpi);
        return;
    }
    const Image& im = a.img.pages[size_t(a.page)];
    ImGui::InvisibleButton("canvas", avail, ImGuiButtonFlags_MouseButtonLeft);
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) a.fit = !a.fit, a.zoom = a.fit ? a.zoom : 1;
    ImGuiIO& io = ImGui::GetIO();
    const float margin = 16 * dpi;
    if (a.fit) {
        a.zoom = std::min((avail.x - 2 * margin) / im.width, (avail.y - 2 * margin) / im.height);
        a.pan_x = (avail.x - im.width * a.zoom) / 2;
        a.pan_y = (avail.y - im.height * a.zoom) / 2;
    }
    if (hovered && io.MouseWheel != 0) {  // 以鼠标为中心缩放
        const float old = a.zoom;
        a.zoom = std::clamp(a.zoom * (io.MouseWheel > 0 ? 1.25f : 0.8f), 0.01f, 64.0f);
        const float mx = io.MousePos.x - origin.x, my = io.MousePos.y - origin.y;
        a.pan_x = mx - (mx - a.pan_x) * a.zoom / old;
        a.pan_y = my - (my - a.pan_y) * a.zoom / old;
        a.fit = false;
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        a.pan_x += io.MouseDelta.x;
        a.pan_y += io.MouseDelta.y;
        a.fit = false;
    }
    const ImVec2 p0(origin.x + a.pan_x, origin.y + a.pan_y), p1(p0.x + im.width * a.zoom, p0.y + im.height * a.zoom);
    dl->PushClipRect(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), true);
    if (a.zoom >= 2) {  // 放大时按像素显示（最近邻）
        dl->AddCallback([](const ImDrawList*, const ImDrawCmd*) { g_ctx->PSSetSamplers(0, 1, &g_point); }, nullptr);
        dl->AddImage(tid(a.tex), p0, p1);
        dl->AddCallback(ImGui::GetPlatformIO().DrawCallback_ResetRenderState, nullptr);
    } else {
        dl->AddImage(tid(a.tex), p0, p1);
    }
    // 左下角：缩放比例胶囊
    char z[64];
    std::snprintf(z, sizeof z, "%.0f%%%s", a.zoom * 100, a.tex_scale > 1 ? tr("（大图已抽稀显示）", " (large image shown downsampled)") : "");
    ImGui::PushFont(ui::fonts().mono, 12);
    const ImVec2 zs = ImGui::CalcTextSize(z);
    const ImVec2 c0(origin.x + 12 * dpi, origin.y + avail.y - zs.y - 20 * dpi), c1(c0.x + zs.x + 20 * dpi, c0.y + zs.y + 8 * dpi);
    dl->AddRectFilled(c0, c1, ui::mix(p.card, IM_COL32(0, 0, 0, 0), 0.12f), (c1.y - c0.y) / 2);
    dl->AddRect(c0, c1, p.border, (c1.y - c0.y) / 2);
    dl->AddText(ImVec2(c0.x + 10 * dpi, c0.y + 4 * dpi), p.text_sub, z);
    ImGui::PopFont();
    dl->PopClipRect();
    if (hovered) {  // 鼠标处像素值
        const int x = int((io.MousePos.x - p0.x) / a.zoom), y = int((io.MousePos.y - p0.y) / a.zoom);
        if (x >= 0 && y >= 0 && x < int(im.width) && y < int(im.height)) {
            const uint16_t* v = &im.px[(size_t(y) * im.width + x) * im.channels];
            std::string s = "(" + std::to_string(x) + ", " + std::to_string(y) + ")  ";
            for (int c = 0; c < im.channels; ++c) s += (c ? " / " : "") + std::to_string(v[c]);
            if (!im.bayer.empty() && im.channels == 1) s += std::string("  [") + im.bayer[(y % 2) * 2 + x % 2] + "]";
            ImGui::PushFont(ui::fonts().mono, 12);
            ImGui::SetTooltip("%s", s.c_str());
            ImGui::PopFont();
        }
    }
}

// 卡片里的小节标题（正文粗体 13，次要文字色）
void card_label(const char* t) {
    ImGui::PushFont(ui::fonts().bold, 13);
    ImGui::PushStyleColor(ImGuiCol_Text, ui::pal().text_sub);
    ImGui::TextUnformatted(t);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void draw_info(App& a, float dpi) {
    const ui::Palette& p = ui::pal();
    if (!a.img.ok) {
        ui::title(tr("详情", "Details"), 18);
        ui::text_sub(a.is_loading ? tr("正在读取…", "Reading…") : tr("打开一张图后，这里显示格式、大小、校验结果和显示设置。", "Open an image to see its format, size, check result and display settings here."));
        return;
    }
    const Image& im = a.img.pages[size_t(a.page)];
    ui::begin_card("file");
    ImGui::PushFont(ui::fonts().bold, 15);
    ImGui::TextWrapped("%s", a.img.name.c_str());
    ImGui::PopFont();
    ui::text_sub(a.img.kind.c_str());
    if (a.img.raw_auto && ui::outline_button(tr("宽高或格式不对？手动输入…", "Wrong size or format? Enter it…"), 0, true)) a.want_raw_dialog = true;
    ImGui::Spacing();
    char buf[160];
    if (a.img.in_package || a.img.from_iis) {
        ui::badge(a.img.in_package ? tr("已从包中取出并核对 SHA-256", "Taken from the package, SHA-256 checked") : tr("已还原并核对 SHA-256", "Restored, SHA-256 checked"), ui::Badge::Success);
        // 压缩比用标题字体的大号数字（规范：大号数字用标题字体）
        std::snprintf(buf, sizeof buf, tr("%.2f 倍", "%.2f×"), a.img.file_size ? double(a.img.orig_size) / double(a.img.file_size) : 0.0);
        ui::title(buf, 22);
        std::snprintf(buf, sizeof buf, tr("%s %s，原文件 %s", "%s %s, original %s"), a.img.in_package ? tr("包内存放", "Stored in package") : tr("压缩文件", "Compressed file"), human(a.img.file_size).c_str(),
                      human(a.img.orig_size).c_str());
        ui::text_sub(buf, 13);
    } else {
        ImGui::Text(tr("文件 %s", "File %s"), human(a.img.file_size).c_str());
    }
    ui::text_sub(a.img.desc.c_str());
    ImGui::Spacing();
    card_label("SHA-256");
    ui::mono(a.img.sha256.c_str(), 11.5f, p.text_muted);
    ui::end_card();
    ImGui::Spacing();

    ui::begin_card("display");
    if (a.img.pages.size() > 1) {
        card_label(tr("页", "Page"));
        int pg = a.page + 1;
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderInt("##page", &pg, 1, int(a.img.pages.size()), tr("第 %d 页", "Page %d"))) {
            a.page = pg - 1;
            a.win = auto_window(a.img.pages[size_t(a.page)]);
            a.tex_dirty = true;
            a.fit = true;
        }
    }
    card_label(tr("窗宽窗位", "Window / level"));
    float lo = float(a.win.lo), hi = float(a.win.hi);
    const float maxv = float(im.bits >= 16 ? 65535 : (1 << im.bits) - 1);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::DragFloatRange2("##win", &lo, &hi, std::max(1.0f, maxv / 500), 0, maxv, tr("下限 %.0f", "Low %.0f"), tr("上限 %.0f", "High %.0f"))) {
        a.win = {lo, std::max(hi, lo + 1)};
        a.tex_dirty = true;
    }
    if (ui::outline_button(tr("自动", "Auto"), 0, true)) a.win = auto_window(im), a.tex_dirty = true;
    ImGui::SameLine();
    if (ui::outline_button(tr("全范围", "Full range"), 0, true)) a.win = {0, maxv}, a.tex_dirty = true;
    if (im.channels == 1 && im.bayer.empty() && !a.show_bayer) {  // 普通灰度图：Bayer 选项收起
        ImGui::Spacing();
        if (ui::outline_button(tr("按 Bayer 排列预览…", "Preview as Bayer…"), 0, true)) a.show_bayer = true;
    } else if (im.channels == 1) {
        ImGui::Spacing();
        std::snprintf(buf, sizeof buf, "Bayer：%s", im.bayer.empty() ? tr("未检测到", "not detected") : im.bayer.c_str());
        card_label(buf);
        ImGui::SetNextItemWidth(-FLT_MIN);
        const char* names[6] = {tr("自动", "Auto"), tr("不去马赛克", "No demosaic"), "RGGB", "GRBG", "GBRG", "BGGR"};
        if (ImGui::Combo("##bayer", &a.bayer_sel, names, 6)) a.tex_dirty = true;
        if (ui::checkbox(tr("去马赛克预览（双线性）", "Demosaic preview (bilinear)"), &a.demosaic)) a.tex_dirty = true;
        if (a.demosaic && ui::checkbox(tr("自动白平衡（仅预览）", "Auto white balance (preview only)"), &a.white_balance)) a.tex_dirty = true;
    }
    ImGui::Spacing();
    card_label(tr("缩放", "Zoom"));
    if (ui::outline_button(tr("适合窗口", "Fit"), 0, true)) a.fit = true;
    ImGui::SameLine();
    if (ui::outline_button("1:1", 0, true)) a.fit = false, a.zoom = 1;
    ui::end_card();
    ImGui::Spacing();
    if (a.img.from_iis || a.img.in_package) {
        if (ui::primary_button(tr("另存为原文件…", "Save original as…"), ImGui::GetContentRegionAvail().x / dpi)) {
            fs::path n = fs::u8path(a.img.name);
            if (a.img.from_iis) n.replace_extension();
            const std::string out = pick(false, true, n.wstring());
            if (!out.empty()) {
                std::ofstream f(fs::u8path(out), std::ios::binary);
                f.write(reinterpret_cast<const char*>(a.img.original.data()), std::streamsize(a.img.original.size()));
                a.message = f ? tr("已保存：", "Saved: ") + out : tr("保存失败：", "Could not save: ") + out;
            }
        }
        ui::text_sub(tr("与原文件逐字节相同。", "Byte-for-byte identical to the original."));
    }
    if (!a.message.empty()) {
        ImGui::Spacing();
        ui::hint(a.message.c_str());
    }
}

// 对话框：自绘标题（标题字体），圆角 16、内距 24
bool begin_dialog(const char* id, const char* heading, float dpi) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24 * dpi, 22 * dpi));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16 * dpi);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::pal().card);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x / 2, vp->Pos.y + vp->Size.y / 2), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    const bool open = ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    if (open && heading) {
        ui::title(heading, 20);
        ImGui::Spacing();
    }
    return open;
}

void draw_dialogs(App& a, const Tex& icon, float dpi) {
    const ui::Palette& p = ui::pal();
    if (a.want_raw_dialog) ImGui::OpenPopup("raw"), a.want_raw_dialog = false;
    if (begin_dialog("raw", tr("RAW 参数", "RAW parameters"), dpi)) {
        static const char* fmts[] = {"Mono8", "Mono10", "Mono12", "Mono14", "Mono16", "Mono10p", "Mono12p", "Mono12Packed",
                                     "BayerRG8", "BayerGB8", "BayerGR8", "BayerBG8", "BayerRG12", "BayerGB12", "BayerGR12",
                                     "BayerBG12", "BayerRG16", "BayerGB16", "BayerGR16", "BayerBG16", "BayerRG12p",
                                     "BayerGB12p", "BayerGR12p", "BayerBG12p"};
        static int fi = 0, w = 0, h = 0, off = 0, stride = 0;
        ui::text_sub(tr("无文件头的 RAW 需要宽、高与像素格式（自动识别不对时在这里改）。", "Headerless RAW needs width, height and pixel format (correct them here if automatic detection got it wrong)."));
        ImGui::PushItemWidth(220 * dpi);
        ImGui::InputInt(tr("宽", "Width"), &w);
        ImGui::InputInt(tr("高", "Height"), &h);
        ImGui::Combo(tr("像素格式", "Pixel format"), &fi, fmts, IM_ARRAYSIZE(fmts));
        ImGui::InputInt(tr("文件头字节（偏移）", "Header bytes (offset)"), &off);
        ImGui::InputInt(tr("行字节（0 = 紧密）", "Row bytes (0 = packed)"), &stride);
        ImGui::PopItemWidth();
        ImGui::Spacing();
        align_right({90, 100});
        if (ui::outline_button(tr("取消", "Cancel"), 90)) ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (ui::primary_button(tr("打开", "Open"), 100) && w > 0 && h > 0) {
            a.raw = {uint32_t(w), uint32_t(h), fmts[fi], uint64_t(std::max(0, off)), uint64_t(std::max(0, stride))};
            a.open(a.path, &a.raw);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (a.want_batch) ImGui::OpenPopup("batch"), a.want_batch = false;
    if (begin_dialog("batch", tr("批量还原 .iis", "Restore .iis files"), dpi)) {
        ui::text_sub(tr("把文件夹里的 .iis 还原为原文件：逐个核对 SHA-256，与原文件逐字节相同；已存在的不覆盖，.iis 保留不动。", "Restore the .iis files in a folder to the original files: each is checked with SHA-256 and is byte-for-byte identical; existing files are never overwritten and the .iis files are kept."));
        ImGui::Spacing();
        card_label(tr("源文件夹", "Source folder"));
        ImGui::SetNextItemWidth(460 * dpi);
        if (input("##src", &a.batch.src)) a.batch.suggest_out(), a.batch.check_out();
        ImGui::SameLine();
        if (ui::outline_button(tr("选择…##src", "Choose…##src"), 0, true)) {
            const std::string pth = pick(true);
            if (!pth.empty()) a.batch.src = pth, a.batch.suggest_out(), a.batch.check_out();
        }
        card_label(tr("还原到", "Restore to"));
        ImGui::SetNextItemWidth(460 * dpi);
        if (input("##out", &a.batch.out)) a.batch.check_out();
        ImGui::SameLine();
        if (ui::outline_button(tr("选择…##out", "Choose…##out"), 0, true)) {
            const std::string pth = pick(true);
            if (!pth.empty()) a.batch.out = pth, a.batch.check_out();
        }
        if (!a.batch.watch_warn.empty())
            ui::text_colored(p.danger, trf("注意：输出文件夹在旧版后台服务的监控目录（%s）里，还原出的文件过一段时间会被重新压缩。",
                                         "Note: the output folder is inside a folder watched by the old background service (%s); restored files will be compressed again after a while.",
                                         a.batch.watch_warn.c_str()).c_str());
        ui::checkbox(tr("含子文件夹", "Include subfolders"), &a.batch.recursive);
        const int total = a.batch.total, done = a.batch.ok + a.batch.skipped + a.batch.failed;
        if (a.batch.running || done) {
            ui::progress(total ? float(done) / total : 0.0f);
            ImGui::Text(tr("共 %d：还原 %d，跳过（已存在）%d，失败 %d", "%d in total: %d restored, %d skipped (already exist), %d failed"), total, int(a.batch.ok), int(a.batch.skipped), int(a.batch.failed));
            std::lock_guard<std::mutex> lk(a.batch.mu);
            for (const auto& e : a.batch.errors) ui::text_colored(p.danger, e.c_str());
        }
        if (a.batch.finished && !a.batch.running)
            ui::badge(a.batch.total ? tr("完成", "Done") : tr("源文件夹里没有 .iis 文件", "No .iis files in the source folder"), a.batch.total ? ui::Badge::Success : ui::Badge::Highlight);
        ImGui::Spacing();
        align_right({90, 140, 100});
        ImGui::BeginDisabled(a.batch.running);
        if (ui::outline_button(tr("关闭", "Close"), 90)) ImGui::CloseCurrentPopup();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(a.batch.running || !(a.batch.finished && a.batch.ok + a.batch.skipped > 0));
        if (ui::outline_button(tr("打开输出文件夹", "Open output folder"), 140))
            ShellExecuteW(nullptr, L"open", fs::u8path(a.batch.out).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(a.batch.running || a.batch.src.empty() || a.batch.out.empty());
        if (ui::primary_button(tr("开始", "Start"), 100)) a.batch.run();
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
    if (a.want_about) ImGui::OpenPopup("about"), a.want_about = false;
    if (begin_dialog("about", nullptr, dpi)) {
        if (icon.srv) ImGui::Image(tid(icon), ImVec2(64 * dpi, 64 * dpi));
        ImGui::SameLine(0, 16 * dpi);
        ImGui::BeginGroup();
        // 主名称用当前语言，另一种语言的名称小一号放在下面
        const bool en = iis::i18n::lang() == iis::i18n::Lang::En;
        ui::title(ui::product(), 26);
        ImGui::PushStyleColor(ImGuiCol_Text, p.text_sub);
        ImGui::PushFont(en ? ui::fonts().title : ui::fonts().title_en, 16);
        ImGui::TextUnformatted(en ? ui::kProduct : ui::kProductEn);
        ImGui::PopFont();
        ImGui::PopStyleColor();
        ImGui::EndGroup();
        ImGui::Spacing();
        ImGui::Text(tr("版本 %s", "Version %s"), iis_version());
        ui::mono(iis_build_info(), 11.5f, p.text_muted);
        ImGui::Spacing();
        ui::text_sub(tr("工业图像无损压缩：解压出的文件与原文件逐字节相同。.iisz 是标准 ZIP 包，.iis 可用开源解码器 iis-decode 还原。", "Lossless compression for industrial images: extracted files are byte-for-byte identical to the originals. .iisz is a standard ZIP package; .iis files can be restored with the open-source decoder iis-decode."));
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, p.text_muted);
        ImGui::Text("%s · %s", ui::kBrand, ui::kBrandEn);
        ImGui::PopStyleColor();
        align_right({90});
        if (ui::outline_button(tr("关闭", "Close"), 90)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// 标题栏里的菜单
void draw_menus(App& a, HWND hwnd, float h, bool& done, float dpi) {
    auto menu = [&](const char* label, const std::function<void()>& body) {
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        if (title_button(label, h)) ImGui::OpenPopup(label);
        ImGui::SameLine();
        ImGui::SetNextWindowPos(ImVec2(p0.x, p0.y + h));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6 * dpi, 6 * dpi));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * dpi, 4 * dpi));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 10 * dpi);
        if (ImGui::BeginPopup(label)) {
            body();
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar(3);
    };
    menu(tr("文件", "File"), [&] {
        if (ImGui::MenuItem(tr("打开文件…", "Open file…"))) {
            const std::string p = pick(false);
            if (!p.empty()) a.open(p);
        }
        if (ImGui::MenuItem(tr("打开文件夹…", "Open folder…"))) {
            const std::string p = pick(true);
            if (!p.empty()) a.open_folder(p);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(tr("压缩文件夹…", "Compress folder…"))) {
            const std::string p = pick(true);
            if (!p.empty()) spawn(L"--compress " + quote_arg(p));
        }
        if (ImGui::MenuItem(tr("解压 .iisz / .iis…", "Extract .iisz / .iis…"))) {
            const std::string p = pick(false);
            if (!p.empty()) spawn(L"--extract " + quote_arg(p));
        }
        if (ImGui::MenuItem(tr("批量还原 .iis…", "Restore .iis files…"))) {
            if (a.batch.src.empty()) a.batch.src = a.folder;
            a.batch.suggest_out(), a.batch.check_out();
            a.want_batch = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem(tr("退出", "Exit"))) done = true;
    });
    menu(tr("视图", "View"), [&] {
        if (ImGui::MenuItem(tr("适合窗口", "Fit"))) a.fit = true;
        if (ImGui::MenuItem(tr("原始大小 1:1", "Actual size 1:1"))) a.fit = false, a.zoom = 1;
        ImGui::MenuItem(tr("网格", "Grid"), nullptr, &a.thumbs_view);
        ImGui::Separator();
        const ui::Mode m = ui::mode();
        if (ImGui::MenuItem(tr("外观：跟随系统", "Appearance: follow Windows"), nullptr, m == ui::Mode::System)) ui::apply(ui::Mode::System, dpi);
        if (ImGui::MenuItem(tr("外观：浅色", "Appearance: light"), nullptr, m == ui::Mode::Light)) ui::apply(ui::Mode::Light, dpi);
        if (ImGui::MenuItem(tr("外观：深色", "Appearance: dark"), nullptr, m == ui::Mode::Dark)) ui::apply(ui::Mode::Dark, dpi);
        ImGui::Separator();
        // 语言名称各用各自的文字写，不随界面语言变
        const iis::i18n::Lang l = iis::i18n::lang();
        auto set_lang = [&](iis::i18n::Lang to) {
            if (to == l) return;
            iis::i18n::set_lang(to);
            SetWindowTextW(hwnd, ui::product_w());
            if (!a.path.empty() && !a.is_loading) a.open(a.path, a.img.raw_auto || a.raw.width == 0 ? nullptr : &a.raw);  // 详情文字按新语言重新生成
        };
        if (ImGui::MenuItem("语言：中文", nullptr, l == iis::i18n::Lang::Zh)) set_lang(iis::i18n::Lang::Zh);
        if (ImGui::MenuItem("Language: English", nullptr, l == iis::i18n::Lang::En)) set_lang(iis::i18n::Lang::En);
    });
    menu(tr("帮助", "Help"), [&] {
        if (ImGui::MenuItem(tr("关于三溪缩影", "About Threebrook Pack"))) a.want_about = true;
    });
}

// 标题栏图标（资源里的 PNG）
Tex load_icon(HINSTANCE inst) {
    std::vector<uint8_t> png = ui::resource(inst, 101);
    if (png.empty()) return {};
    const Loaded l = load_bytes(std::move(png), "app.png");
    if (!l.ok || l.pages.empty() || l.pages[0].channels != 4) return {};
    const Image& im = l.pages[0];
    std::vector<uint8_t> rgba(im.px.size());
    std::transform(im.px.begin(), im.px.end(), rgba.begin(), [](uint16_t v) { return uint8_t(v); });  // WIC 解出的是 8 位
    return make_tex(rgba, int(im.width), int(im.height));
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring render_path, open_path, restore_path;
    std::vector<std::wstring> task_paths;
    int task_mode = 0;  // 1 压缩，2 解压
    bool restore = false, size_given = false;
    float render_scale = 1;
    ui::Mode theme = ui::Mode::System;
    int rw = 1280, rh = 800;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--render" && i + 1 < argc) render_path = argv[++i];
        else if (a == L"--size" && i + 1 < argc) swscanf_s(argv[++i], L"%dx%d", &rw, &rh), size_given = true;
        else if (a == L"--scale" && i + 1 < argc) render_scale = std::clamp(float(_wtof(argv[++i])), 1.0f, 4.0f);
        else if (a == L"--light") theme = ui::Mode::Light;
        else if (a == L"--dark") theme = ui::Mode::Dark;
        else if (a == L"--lang" && i + 1 < argc) {  // zh / en；不给时跟随 Windows 的显示语言
            const std::wstring v = argv[++i];
            iis::i18n::set_lang(v.rfind(L"en", 0) == 0 ? iis::i18n::Lang::En : iis::i18n::Lang::Zh);
        }
        else if (a == L"--compress") task_mode = 1;
        else if (a == L"--extract") task_mode = 2;
        else if (a == L"--restore") {
            restore = true;
            if (i + 1 < argc && std::wstring(argv[i + 1]).rfind(L"--", 0) != 0) restore_path = argv[++i];
        } else if (task_mode) task_paths.push_back(a);
        else open_path = a;
    }
    LocalFree(argv);
    const bool headless = !render_path.empty();

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::unique_ptr<Task> task;
    if (task_mode) {
        if (!headless && !task_paths.empty() && !collect(inst, task_mode == 1 ? L"compress" : L"extract", task_paths)) {
            CoUninitialize();
            return 0;  // 已交给先启动的窗口
        }
        if (task_paths.empty()) {  // 没给路径：先选
            const std::string p = pick(task_mode == 1);
            if (p.empty()) {
                CoUninitialize();
                return 0;
            }
            task_paths.push_back(fs::u8path(p).wstring());
        }
        task = std::make_unique<Task>();
        task->compress = task_mode == 1;
        for (const auto& p : task_paths) {
            task->paths.push_back(u8(fs::path(p)));
            if (task->compress) task->preview.push_back(compress_preview(task->paths.back()));
        }
        if (!size_given) rw = 640, rh = 460;
    }
    ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW wc{sizeof wc, CS_CLASSDC, wndproc, 0, 0, inst, LoadIconW(inst, MAKEINTRESOURCEW(1)), LoadCursor(nullptr, IDC_ARROW),
                   nullptr, nullptr, L"IisViewer", nullptr};
    wc.hIconSm = HICON(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                  GetSystemMetrics(SM_CYSMICON), 0));
    RegisterClassExW(&wc);
    const float dpi = headless ? render_scale : ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
    g_dpi = dpi;
    const bool en = iis::i18n::lang() == iis::i18n::Lang::En;
    const std::wstring product = ui::product_w();
    const std::wstring title = !task ? product : product + (task->compress ? (en ? L" - Compress" : L" - 压缩") : (en ? L" - Extract" : L" - 解压"));
    const DWORD wstyle = task ? WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME : WS_OVERLAPPEDWINDOW;
    const int ww = int(rw * dpi), wh = int(rh * dpi);
    const int wx = task ? (GetSystemMetrics(SM_CXSCREEN) - ww) / 2 : 100, wy = task ? (GetSystemMetrics(SM_CYSCREEN) - wh) / 3 : 100;
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(), wstyle, wx, wy, ww, wh, nullptr, nullptr, inst, nullptr);
    // 去掉系统标题栏后让窗口重新计算客户区（WM_NCCALCSIZE）
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    if (!create_device(hwnd)) {
        MessageBoxW(nullptr, iis::i18n::lang() == iis::i18n::Lang::En ? L"Cannot start Direct3D 11 (WARP is not available either)" : L"无法初始化 Direct3D 11（WARP 也不可用）",
                    ui::product_w(), MB_ICONERROR);
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // 不写任何配置文件
    io.LogFilename = nullptr;
    if (headless) io.ConfigFlags |= ImGuiConfigFlags_NoMouse;  // 截图：不受真实鼠标位置影响（不出悬停提示）
    ui::load_fonts(inst);
    ui::apply(theme, dpi);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);
    if (!headless) {
        ShowWindow(hwnd, show == SW_HIDE ? SW_SHOWDEFAULT : show);
        UpdateWindow(hwnd);
        DragAcceptFiles(hwnd, TRUE);
    }
    Tex icon = load_icon(inst);

    App app;
    app.thumbs.start();
    if (task && !task->compress) task->start();  // 解压不需要选项，直接开始
    if (!open_path.empty()) app.open(u8(fs::path(open_path)));
    if (restore) {  // 右键“还原”：同时浏览该目录（或打开该文件），弹出填好的批量还原
        std::error_code ec;
        const fs::path rp(restore_path);
        if (!restore_path.empty()) {
            if (fs::is_directory(rp, ec)) app.open_folder(u8(rp));
            else app.open(u8(rp));
        }
        app.batch.src = restore_path.empty() ? app.folder : u8(rp);
        app.batch.suggest_out(), app.batch.check_out();
        app.want_batch = true;
    }

    int frames = 0;
    bool done = false;
    const auto t0 = std::chrono::steady_clock::now();
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;
        if (g_theme_changed) {  // 系统切换了深浅色
            g_theme_changed = false;
            if (ui::mode() == ui::Mode::System) ui::apply(ui::Mode::System, dpi);
        }
        if (g_resize_w && g_resize_h) {
            release_rtv();
            g_swap->ResizeBuffers(0, g_resize_w, g_resize_h, DXGI_FORMAT_UNKNOWN, 0);
            g_resize_w = g_resize_h = 0;
            create_rtv();
        }
        for (const auto& d : g_dropped) app.open(u8(fs::path(d)));
        g_dropped.clear();
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        app.poll_loading();
        app.rebuild_texture(now);
        {  // 缩略图结果上传为纹理（每帧最多 8 个）
            std::lock_guard<std::mutex> lk(app.thumbs.mu);
            for (int k = 0; k < 8 && !app.thumbs.done.empty(); ++k) {
                auto& [p, rgba, w, h] = app.thumbs.done.back();
                app.thumb_tex[p] = rgba.empty() ? Tex{} : make_tex(rgba, w, h);
                app.thumbs.done.pop_back();
                --app.thumbs.pending;
            }
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        const ImGuiWindowFlags fixed = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings;
        if (task) {  // 压缩 / 解压窗口：标题栏 + 一块内容
            const float top = draw_titlebar(hwnd, icon, task->compress ? tr("压缩", "Compress") : tr("解压", "Extract"), false, nullptr);
            ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + top));
            ImGui::SetNextWindowSize(ImVec2(vp->Size.x, vp->Size.y - top));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24 * dpi, 20 * dpi));
            ImGui::Begin("任务", nullptr, fixed | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::PopStyleVar();
            draw_task(*task, dpi, done);
            ImGui::End();
        } else {
            const float top = draw_titlebar(hwnd, icon, nullptr, true, [&](float h) { draw_menus(app, hwnd, h, done, dpi); });
            const float left = 300 * dpi, right = 320 * dpi, H = vp->Size.y - top;
            const ui::Palette& p = ui::pal();
            ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + top));
            ImGui::SetNextWindowSize(ImVec2(left, H));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, p.sidebar);
            ImGui::Begin("左栏", nullptr, fixed);
            draw_folder(app, dpi);
            ImGui::End();
            ImGui::PopStyleColor();
            ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + left, vp->Pos.y + top));
            ImGui::SetNextWindowSize(ImVec2(vp->Size.x - left - right, H));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::Begin("图像", nullptr, fixed | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::PopStyleVar();
            draw_image(app, icon, dpi);
            ImGui::End();
            ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - right, vp->Pos.y + top));
            ImGui::SetNextWindowSize(ImVec2(right, H));
            ImGui::Begin("信息", nullptr, fixed);
            draw_info(app, dpi);
            ImGui::End();
            // 左右栏与看图区之间的分隔线
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            fg->AddLine(ImVec2(vp->Pos.x + left, vp->Pos.y + top), ImVec2(vp->Pos.x + left, vp->Pos.y + vp->Size.y), p.border);
            fg->AddLine(ImVec2(vp->Pos.x + vp->Size.x - right, vp->Pos.y + top), ImVec2(vp->Pos.x + vp->Size.x - right, vp->Pos.y + vp->Size.y),
                        p.border);
            draw_dialogs(app, icon, dpi);
        }

        ImGui::Render();
        const ImVec4 bg = ImGui::ColorConvertU32ToFloat4(ui::pal().bg);
        const float clear[4] = {bg.x, bg.y, bg.z, 1};
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        // --render：等读取与缩略图完成（最多 15 秒），再渲染几帧后截图退出
        if (headless) {
            const bool settled = task ? (task->compress || task->finished) : !app.is_loading && !app.tex_dirty && app.thumbs.pending == 0;
            if ((settled && ++frames > 10) || now > 15) {
                save_backbuffer(render_path);
                done = true;
            }
        }
        g_swap->Present(headless ? 0 : 1, 0);
        if (headless) Sleep(20);
    }
    app.thumbs.shutdown();
    if (app.batch.th.joinable()) app.batch.th.join();
    if (task && task->th.joinable()) {  // 关窗口时还在做：取消（打包不会留下半个包）
        task->cancel = true;
        task->th.join();
    }
    app.tex.release();
    icon.release();
    for (auto& [p, t] : app.thumb_tex) t.release();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanup_device();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, inst);
    CoUninitialize();
    return 0;
}
