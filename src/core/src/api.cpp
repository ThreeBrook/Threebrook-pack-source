// 版本、状态码、错误信息与缓冲区管理。
#include "internal.hpp"

#include <charls/charls.h>
#include <zstd.h>

#include <atomic>
#include <string>

namespace iis {

namespace fault {
namespace {
std::atomic<Hook> g_hook{nullptr};
}
void set_hook(Hook h) { g_hook.store(h); }
int at(const char* point) {
    const Hook h = g_hook.load(std::memory_order_relaxed);
    return h ? h(point) : 0;
}
}  // namespace fault

namespace {
thread_local std::string g_last_error;

std::string compiler_name() {
#if defined(_MSC_VER)
    return "msvc " + std::to_string(_MSC_VER);
#elif defined(__clang__)
    return "clang " + std::to_string(__clang_major__) + "." + std::to_string(__clang_minor__);
#elif defined(__GNUC__)
    return "gcc " + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__);
#else
    return "unknown";
#endif
}
}  // namespace

int fail(int status, std::string message) {
    g_last_error = std::move(message);
    return status;
}

void clear_error() { g_last_error.clear(); }

}  // namespace iis

extern "C" {

const char* iis_version(void) { return IIS_VERSION_STRING; }

int iis_abi_version(void) { return IIS_ABI_VERSION; }

const char* iis_build_info(void) {
    static const std::string info = std::string("iis ") + IIS_VERSION_STRING + "; charls " +
                                    charls_get_version_string() + "; zstd " + ZSTD_versionString() +
                                    "; crc32c " + iis::crc32c_impl_name() + "; " + iis::compiler_name();
    return info.c_str();
}

const char* iis_status_str(int status) {
    switch (status) {
        case IIS_OK: return "成功";
        case IIS_E_INVALID_ARG: return "参数错误";
        case IIS_E_NO_MEMORY: return "内存不足";
        case IIS_E_CORRUPT: return "数据损坏";
        case IIS_E_UNSUPPORTED: return "不支持";
        case IIS_E_SIZE_MISMATCH: return "长度不一致";
        case IIS_E_CODEC: return "编码器失败";
        case IIS_E_IO: return "文件读写失败";
        case IIS_E_NOT_FOUND: return "不存在";
        case IIS_E_BUSY: return "文件被占用";
        case IIS_E_CONFLICT: return "目标已存在";
        case IIS_E_NO_SPACE: return "磁盘已满";
        case IIS_E_INTERNAL: return "内部错误";
        default: return "未知状态";
    }
}

const char* iis_last_error(void) { return iis::g_last_error.c_str(); }

void iis_buffer_free(iis_buffer* buf) {
    if (!buf) return;
    std::free(buf->data);
    buf->data = nullptr;
    buf->size = 0;
}

size_t iis_image_bytes(const iis_image_info* info) {
    if (!info) return 0;
    const uint64_t bps = info->bits_per_sample <= 8 ? 1 : 2;
    const uint64_t n = uint64_t(info->width) * info->height * info->components * bps;
    return n > SIZE_MAX ? 0 : size_t(n);
}

}  // extern "C"
