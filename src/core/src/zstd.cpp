// zstd 压缩与解压：非像素字节（文件头、调色板、标签等）与兜底整文件存储。
#include "internal.hpp"

#include <zstd.h>

extern "C" {

int iis_zstd_compress(const void* data, size_t size, int level, iis_buffer* out) {
    IIS_GUARD_BEGIN
    if (!out) return iis::fail(IIS_E_INVALID_ARG, "输出缓冲区为空");
    *out = {};
    if (!data && size) return iis::fail(IIS_E_INVALID_ARG, "数据指针为空");
    if (level < 1 || level > 19) return iis::fail(IIS_E_INVALID_ARG, "zstd 级别须为 1~19");
    if (!iis::buffer_alloc(out, ZSTD_compressBound(size))) return iis::fail(IIS_E_NO_MEMORY, "内存不足");
    const size_t n = ZSTD_compress(out->data, out->size, data, size, level);
    if (ZSTD_isError(n)) {
        iis_buffer_free(out);
        return iis::fail(IIS_E_CODEC, std::string("zstd 压缩失败：") + ZSTD_getErrorName(n));
    }
    iis::buffer_shrink(out, n);
    return IIS_OK;
    IIS_GUARD_END
}

int iis_zstd_decompress(const void* data, size_t size, uint64_t expected_size, iis_buffer* out) {
    IIS_GUARD_BEGIN
    if (!out) return iis::fail(IIS_E_INVALID_ARG, "输出缓冲区为空");
    *out = {};
    if (!data) return iis::fail(IIS_E_INVALID_ARG, "数据指针为空");
    const size_t frame = ZSTD_findFrameCompressedSize(data, size);
    if (ZSTD_isError(frame)) return iis::fail(IIS_E_CORRUPT, std::string("zstd 帧损坏：") + ZSTD_getErrorName(frame));
    if (frame != size) return iis::fail(IIS_E_CORRUPT, "zstd 帧之后有多余数据");
    const unsigned long long fcs = ZSTD_getFrameContentSize(data, size);
    if (fcs == ZSTD_CONTENTSIZE_ERROR) return iis::fail(IIS_E_CORRUPT, "zstd 帧头损坏");
    if (fcs == ZSTD_CONTENTSIZE_UNKNOWN) return iis::fail(IIS_E_UNSUPPORTED, "zstd 帧头未记录原始长度");
    if (expected_size != IIS_SIZE_UNKNOWN && fcs != expected_size)
        return iis::fail(IIS_E_SIZE_MISMATCH, "zstd 帧记录的长度 " + std::to_string(fcs) + " 与预期 " +
                                                  std::to_string(expected_size) + " 不符");
    // 预期长度未知时，帧头声明的长度可能来自损坏数据；超过上限按损坏处理，不尝试分配
    if (expected_size == IIS_SIZE_UNKNOWN && fcs > (uint64_t(8) << 30))
        return iis::fail(IIS_E_CORRUPT, "zstd 帧头声明的长度过大");
    if (fcs > SIZE_MAX) return iis::fail(IIS_E_UNSUPPORTED, "数据过大");
    if (!iis::buffer_alloc(out, size_t(fcs))) return iis::fail(IIS_E_NO_MEMORY, "内存不足");
    const size_t n = ZSTD_decompress(out->data, out->size, data, size);
    if (ZSTD_isError(n) || n != fcs) {
        iis_buffer_free(out);
        return iis::fail(IIS_E_CORRUPT, std::string("zstd 解压失败：") +
                                            (ZSTD_isError(n) ? ZSTD_getErrorName(n) : "长度不符"));
    }
    return IIS_OK;
    IIS_GUARD_END
}

}  // extern "C"
