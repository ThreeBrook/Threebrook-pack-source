// iis_thumb.dll：资源管理器里 .iis 与 .iisz 文件的缩略图（Windows 缩略图处理程序，IThumbnailProvider）。
//
// 资源管理器把文件内容以流的形式交给本程序，按开头的字节区分：
//   .iis：整个读进内存还原（核对 SHA-256）、按内容识别、缩成缩略图；
//   .iisz 包（ZIP）：只读中央目录和前 4 张能显示的图像的条目，拼成 2×2（见 package_mosaic），不读整个包。
// 与查看器用同一套读取代码（16 位自动窗宽窗位、无文件头 RAW 自动识别）。
// Windows 默认把缩略图处理程序放在单独的隔离进程里运行，本程序出错不会影响资源管理器；
// 每次调用都捕获所有异常，出错时返回失败，资源管理器改为显示图标。
// 注册（安装包写入）：HKCR\CLSID\{6C71C38C-…}\InprocServer32 = 本 DLL（Apartment），
//                     HKCR\.iis\ShellEx\{e357fccd-a995-4576-b01f-234630154e96} = {6C71C38C-…}，.iisz 同样
#include "image.hpp"

#include "iis/iis.h"
#include "io.hpp"
#include "package.hpp"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <thumbcache.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <new>
#include <vector>

namespace {

// {6C71C38C-08F5-40DC-AD84-B8644B008F7A}
const CLSID kClsid = {0x6C71C38C, 0x08F5, 0x40DC, {0xAD, 0x84, 0xB8, 0x64, 0x4B, 0x00, 0x8F, 0x7A}};
constexpr ULONGLONG kMaxBytes = 2ull << 30;  // 更大的文件不做缩略图

std::atomic<long> g_objects{0}, g_locks{0};

// 资源管理器给的流当作随机读取的字节源（包只读用到的部分）
class StreamSource final : public iis::io::Source {
public:
    StreamSource(IStream* s, uint64_t size) : s_(s), size_(size) { s_->AddRef(); }
    ~StreamSource() override { s_->Release(); }
    uint64_t size() const override { return size_; }
    int read_at(uint64_t offset, void* dst, size_t size) override {
        if (offset > size_ || size > size_ - offset) return IIS_E_CORRUPT;
        LARGE_INTEGER at;
        at.QuadPart = LONGLONG(offset);
        if (FAILED(s_->Seek(at, STREAM_SEEK_SET, nullptr))) return IIS_E_IO;
        auto* p = static_cast<uint8_t*>(dst);
        for (size_t got = 0; got < size;) {
            ULONG n = 0;
            const ULONG want = ULONG(std::min<size_t>(size - got, 1u << 30));
            if (FAILED(s_->Read(p + got, want, &n)) || n == 0) return IIS_E_CORRUPT;
            got += n;
        }
        return IIS_OK;
    }

private:
    IStream* s_;
    uint64_t size_;
};

class Provider final : public IInitializeWithStream, public IThumbnailProvider {
public:
    Provider() { ++g_objects; }
    ~Provider() {
        if (stream_) stream_->Release();
        --g_objects;
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IThumbnailProvider) *ppv = static_cast<IThumbnailProvider*>(this);
        else if (riid == IID_IInitializeWithStream) *ppv = static_cast<IInitializeWithStream*>(this);
        else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ULONG(++refs_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        const long n = --refs_;
        if (n == 0) delete this;
        return ULONG(n);
    }

    IFACEMETHODIMP Initialize(IStream* s, DWORD) override {
        if (!s) return E_INVALIDARG;
        if (stream_) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        stream_ = s;
        stream_->AddRef();
        return S_OK;
    }

    IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP* out, WTS_ALPHATYPE* alpha) override {
        if (!out || !alpha) return E_POINTER;
        *out = nullptr;
        *alpha = WTSAT_RGB;
        if (!stream_ || cx == 0) return E_UNEXPECTED;
        try {
            STATSTG st{};
            if (FAILED(stream_->Stat(&st, STATFLAG_NONAME))) return E_FAIL;
            const uint64_t size = st.cbSize.QuadPart;
            auto src = std::make_unique<StreamSource>(stream_, size);
            uint8_t magic[4] = {};
            if (size < 4 || src->read_at(0, magic, 4)) return E_FAIL;
            std::vector<uint8_t> rgba;
            int w = 0, h = 0;
            if (magic[0] == 'P' && magic[1] == 'K') {  // .iisz 包（ZIP）：不限大小，只读目录与几张图
                iis::pkg::Reader pk;
                if (pk.open(std::move(src), "thumbnail.iisz")) return E_FAIL;
                if (!iis::view::package_mosaic(pk, int(cx), rgba, w, h)) return E_FAIL;
                *alpha = WTSAT_ARGB;  // 拼图格子之间的缝透明
            } else {
                if (size > kMaxBytes) return E_FAIL;
                std::vector<uint8_t> d(static_cast<size_t>(size));
                if (src->read_at(0, d.data(), d.size())) return E_FAIL;
                const iis::view::Loaded l = iis::view::load_bytes(std::move(d), "thumbnail.iis");
                if (!l.ok || l.pages.empty()) return E_FAIL;
                iis::view::thumbnail(l.pages[0], int(cx), rgba, w, h);
            }
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof bi.bmiHeader;
            bi.bmiHeader.biWidth = w;
            bi.bmiHeader.biHeight = -h;  // 自上而下
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            void* bits = nullptr;
            HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (!bmp || !bits) return E_OUTOFMEMORY;
            auto* p = static_cast<uint8_t*>(bits);
            for (size_t i = 0; i < size_t(w) * h; ++i) {  // RGBA → BGRA（alpha 只有 0 与 255，透明处 RGB 为 0，即预乘）
                p[i * 4 + 0] = rgba[i * 4 + 2];
                p[i * 4 + 1] = rgba[i * 4 + 1];
                p[i * 4 + 2] = rgba[i * 4 + 0];
                p[i * 4 + 3] = *alpha == WTSAT_ARGB ? rgba[i * 4 + 3] : 255;
            }
            *out = bmp;
            return S_OK;
        } catch (const std::bad_alloc&) {
            return E_OUTOFMEMORY;
        } catch (...) {
            return E_FAIL;
        }
    }

private:
    std::atomic<long> refs_{1};
    IStream* stream_ = nullptr;
};

class Factory final : public IClassFactory {
public:
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return 2; }   // 静态对象
    IFACEMETHODIMP_(ULONG) Release() override { return 1; }
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* p = new (std::nothrow) Provider();
        if (!p) return E_OUTOFMEMORY;
        const HRESULT hr = p->QueryInterface(riid, ppv);
        p->Release();
        return hr;
    }
    IFACEMETHODIMP LockServer(BOOL lock) override {
        lock ? ++g_locks : --g_locks;
        return S_OK;
    }
};

Factory g_factory;

}  // namespace

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (clsid != kClsid) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow() { return g_objects == 0 && g_locks == 0 ? S_OK : S_FALSE; }
