// 内核内部公用：错误记录、缓冲区分配、异常到状态码的转换。
#pragma once

#include "iis/iis.h"

#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

namespace iis {

// 记录本线程的错误说明并返回状态码，便于 `return fail(IIS_E_..., "...")`
int fail(int status, std::string message);
void clear_error();

// 用 malloc 分配（与 iis_buffer_free 配对）；size 为 0 时也返回有效指针
inline bool buffer_alloc(iis_buffer* out, size_t size) {
    out->data = static_cast<uint8_t*>(std::malloc(size ? size : 1));
    out->size = out->data ? size : 0;
    return out->data != nullptr;
}

// 缩小已分配缓冲区到实际长度（失败时保留原块，不影响正确性）
inline void buffer_shrink(iis_buffer* out, size_t size) {
    if (size < out->size) {
        if (void* p = std::realloc(out->data, size ? size : 1)) out->data = static_cast<uint8_t*>(p);
        out->size = size;
    }
}

// 故障注入（只供内核测试，不经 C 接口导出，产品中钩子恒为空）：管线在各步骤调用 at(步骤名)，
// 钩子返回非 0 时该步骤按这个错误码失败；测试也可在钩子里直接终止进程，模拟任意时刻崩溃或断电。
namespace fault {
using Hook = int (*)(const char* point);
void set_hook(Hook h);
int at(const char* point);
}  // namespace fault

const char* crc32c_impl_name();
uint32_t crc32c_software(uint32_t crc, const void* data, size_t size);  // 测试对照用

}  // namespace iis

// C 接口函数体统一包一层，任何 C++ 异常都不越过 ABI 边界
#define IIS_GUARD_BEGIN try {
#define IIS_GUARD_END                                                         \
    }                                                                         \
    catch (const std::bad_alloc&) {                                           \
        return iis::fail(IIS_E_NO_MEMORY, "内存不足");                        \
    }                                                                         \
    catch (const std::exception& e) {                                         \
        return iis::fail(IIS_E_INTERNAL, std::string("内部错误：") + e.what()); \
    }                                                                         \
    catch (...) {                                                             \
        return iis::fail(IIS_E_INTERNAL, "内部错误：未知异常");               \
    }
