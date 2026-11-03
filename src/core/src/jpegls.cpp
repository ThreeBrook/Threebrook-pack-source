// JPEG-LS 无损编解码（CharLS）。只产出、只接受 NEAR = 0 的码流。
#include "internal.hpp"

#include <charls/charls.h>

#include <vector>

namespace {

// 损坏的码流头可能声明极大的尺寸；超过此值直接按损坏处理，不尝试分配
constexpr uint64_t kMaxImageBytes = uint64_t(8) << 30;

int check_info(const iis_image_info* info) {
    if (!info) return iis::fail(IIS_E_INVALID_ARG, "图像参数为空");
    if (!info->width || !info->height) return iis::fail(IIS_E_INVALID_ARG, "宽或高为 0");
    if (info->width > 65535 || info->height > 65535)
        return iis::fail(IIS_E_INVALID_ARG, "JPEG-LS 单帧宽高不超过 65535");
    // 分量数与 imagecodecs 一致：1、3、4（阶段 0 不产生其他分量数）
    if (info->components != 1 && info->components != 3 && info->components != 4)
        return iis::fail(IIS_E_INVALID_ARG, "分量数须为 1、3 或 4");
    if (info->bits_per_sample < 2 || info->bits_per_sample > 16)
        return iis::fail(IIS_E_INVALID_ARG, "位深须为 2~16");
    return IIS_OK;
}

// 位深不是 8 / 16 时，样本值可能超出 2^bits-1；CharLS 不检查，这里先拦下
template <typename T>
bool within_range(const void* pixels, size_t count, uint32_t bits) {
    const T maxval = T((1u << bits) - 1);
    const T* p = static_cast<const T*>(pixels);
    T acc = 0;
    for (size_t i = 0; i < count; ++i) acc |= p[i];
    return (acc & ~maxval) == 0;
}

int decode_error(const charls::jpegls_error& e) {
    return iis::fail(IIS_E_CORRUPT, std::string("JPEG-LS 码流无法解码：") + e.what());
}

}  // namespace

extern "C" {

int iis_jpegls_encode(const void* pixels, size_t size, const iis_image_info* info, iis_buffer* out) {
    IIS_GUARD_BEGIN
    if (!out) return iis::fail(IIS_E_INVALID_ARG, "输出缓冲区为空");
    *out = {};
    if (int st = check_info(info)) return st;
    if (!pixels) return iis::fail(IIS_E_INVALID_ARG, "像素指针为空");
    const size_t expect = iis_image_bytes(info);
    if (size != expect)
        return iis::fail(IIS_E_SIZE_MISMATCH,
                         "像素长度 " + std::to_string(size) + " 与图像参数要求的 " + std::to_string(expect) + " 不符");
    const uint32_t bits = info->bits_per_sample;
    if (bits != 8 && bits != 16) {
        const bool ok = bits < 8 ? within_range<uint8_t>(pixels, size, bits)
                                 : within_range<uint16_t>(pixels, size / 2, bits);
        if (!ok) return iis::fail(IIS_E_INVALID_ARG, "样本值超出 " + std::to_string(bits) + " 位范围");
    }

    charls::jpegls_encoder enc;
    enc.frame_info({info->width, info->height, int32_t(bits), int32_t(info->components)});
    // 交织方式与 imagecodecs 相同，保证码流逐字节一致：3 分量按像素交织，4 分量按行交织
    if (info->components == 3) enc.interleave_mode(charls::interleave_mode::sample);
    if (info->components == 4) enc.interleave_mode(charls::interleave_mode::line);
    if (!iis::buffer_alloc(out, enc.estimated_destination_size())) return iis::fail(IIS_E_NO_MEMORY, "内存不足");
    try {
        enc.destination(out->data, out->size);
        const size_t n = enc.encode(pixels, size);
        iis::buffer_shrink(out, n);
    } catch (const charls::jpegls_error& e) {
        iis_buffer_free(out);
        return iis::fail(IIS_E_CODEC, std::string("JPEG-LS 编码失败：") + e.what());
    }
    return IIS_OK;
    IIS_GUARD_END
}

int iis_jpegls_read_info(const void* data, size_t size, iis_image_info* info) {
    IIS_GUARD_BEGIN
    if (!data || !info) return iis::fail(IIS_E_INVALID_ARG, "参数为空");
    try {
        charls::jpegls_decoder dec(data, size, true);
        const auto fi = dec.frame_info();
        *info = {fi.width, fi.height, uint32_t(fi.component_count), uint32_t(fi.bits_per_sample)};
    } catch (const charls::jpegls_error& e) {
        return decode_error(e);
    }
    return IIS_OK;
    IIS_GUARD_END
}

int iis_jpegls_decode(const void* data, size_t size, iis_image_info* info, iis_buffer* out) {
    IIS_GUARD_BEGIN
    if (!out) return iis::fail(IIS_E_INVALID_ARG, "输出缓冲区为空");
    *out = {};
    if (!data) return iis::fail(IIS_E_INVALID_ARG, "码流指针为空");
    try {
        charls::jpegls_decoder dec(data, size, true);
        const auto fi = dec.frame_info();
        const iis_image_info got{fi.width, fi.height, uint32_t(fi.component_count), uint32_t(fi.bits_per_sample)};
        if (check_info(&got)) return iis::fail(IIS_E_UNSUPPORTED, "码流图像参数超出本库支持范围");
        if (dec.near_lossless() != 0) return iis::fail(IIS_E_UNSUPPORTED, "有损 JPEG-LS 码流（NEAR ≠ 0）");
        const size_t bytes = iis_image_bytes(&got);
        if (bytes > kMaxImageBytes) return iis::fail(IIS_E_CORRUPT, "码流声明的图像过大");
        if (!iis::buffer_alloc(out, bytes)) return iis::fail(IIS_E_NO_MEMORY, "内存不足");

        const bool planar = got.components > 1 && dec.interleave_mode() == charls::interleave_mode::none;
        if (!planar) {
            dec.decode(out->data, out->size);
        } else {
            // 非交织码流按平面解出，再转成按像素交织
            std::vector<uint8_t> tmp(bytes);
            dec.decode(tmp.data(), tmp.size());
            const size_t bps = got.bits_per_sample <= 8 ? 1 : 2;
            const size_t plane = size_t(got.width) * got.height;
            for (size_t c = 0; c < got.components; ++c)
                for (size_t i = 0; i < plane; ++i)
                    std::memcpy(out->data + (i * got.components + c) * bps, tmp.data() + (c * plane + i) * bps, bps);
        }
        if (info) *info = got;
    } catch (const charls::jpegls_error& e) {
        iis_buffer_free(out);
        return decode_error(e);
    }
    return IIS_OK;
    IIS_GUARD_END
}

}  // extern "C"
