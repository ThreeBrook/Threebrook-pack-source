// 单文件压缩管线（内存内）：识别 → 版面拆分 → 像素编码 → 写容器 → 自检（独立读回、还原、SHA-256 比对）。
// 任何一步失败都回退到整文件 zstd；压了更大就不压（返回“保留原文件”）。
// 落盘与原子提交见 commit.cpp，多线程见 pool.cpp。
#include "pipeline.hpp"

#include "container.hpp"
#include "detect.hpp"
#include "layout.hpp"
#include "tiffcodec.hpp"

#include <charls/charls.h>
#include <zstd.h>

#include <chrono>
#include <cmath>

struct iis_raw_rules {
    iis::fmt::RawRules rules;
};

namespace iis {

using nlohmann::json;

namespace {

constexpr int kSideZstd = 19;  // 原样字节、附加字节、配方：数据小，用最高常用级别

json writer_info() {
    return {{"name", "libiis"},
            {"version", IIS_VERSION_STRING},
            {"libs",
             {{"charls", charls_get_version_string()},
              {"zstd", ZSTD_versionString()},
              {"zlib", tiffcodec::zlib_lib_version()},
              {"libdeflate", tiffcodec::libdeflate_version()}}}};
}

// zstd 更小才压，否则原样；返回 codec
uint16_t pack_side(const std::vector<uint8_t>& raw, std::vector<uint8_t>& out) {
    cont::Buf z;
    if (!raw.empty() && iis_zstd_compress(raw.data(), raw.size(), kSideZstd, &z.b) == IIS_OK && z.b.size < raw.size()) {
        out.assign(z.b.data, z.b.data + z.b.size);
        return cont::ZSTD;
    }
    out = raw;
    return cont::NONE;
}

int self_check(const std::vector<uint8_t>& c, const char* fault_point) {
    if (int st = fault::at(fault_point)) return fail(st, "注入故障：自检不一致");
    cont::Reader r;
    if (int st = r.open(std::make_unique<io::MemorySource>(c.data(), c.size()))) return st;
    cont::Buf out;
    return r.restore(out);  // 还原后与头部记录的原文件 SHA-256 比对
}

int build_image_container(const uint8_t* d, size_t n, const uint8_t sha[32], const layout::Split& s,
                          std::vector<uint8_t>& out) {
    if (int st = fault::at("encode.image")) return fail(st, "注入故障：图像编码失败");
    json planes = json::array(), extras = json::array();
    std::vector<std::vector<uint8_t>> streams;
    for (size_t i = 0; i < s.planes.size(); ++i) {
        const layout::Plane& p = s.planes[i];
        // 与阶段 0 一致：有效位 ≤ 8 的平面按 8 位编码；JPEG-LS 位深下限 2
        const uint32_t bits = uint32_t(std::max(2, p.bits));
        const iis_image_info info{uint32_t(p.width), uint32_t(p.height), p.components, bits};
        if (p.width > 65535 || p.height > 65535) return fail(IIS_E_UNSUPPORTED, "像素平面超过 JPEG-LS 单帧尺寸");
        std::vector<uint8_t> px(iis_image_bytes(&info));
        if (bits <= 8)
            for (size_t k = 0; k < p.px.size(); ++k) px[k] = uint8_t(p.px[k]);
        else
            for (size_t k = 0; k < p.px.size(); ++k) px[2 * k] = uint8_t(p.px[k]), px[2 * k + 1] = uint8_t(p.px[k] >> 8);
        cont::Buf enc;
        if (int st = iis_jpegls_encode(px.data(), px.size(), &info, &enc.b)) return st;
        streams.emplace_back(enc.b.data, enc.b.data + enc.b.size);
        json pm = {{"id", i},
                   {"image", p.image},
                   {"role", p.role},
                   {"bayer_phase", p.bayer_dy < 0 ? json(nullptr) : json::array({p.bayer_dy, p.bayer_dx})},
                   {"width", p.width},
                   {"height", p.height},
                   {"components", p.components},
                   {"bits", bits},
                   {"shift", p.shift},
                   {"codec", "jpegls"}};
        if (p.sample >= 0) pm["sample"] = p.sample;
        planes.push_back(pm);
    }
    for (size_t i = 0; i < s.extras.size(); ++i) {
        json em = {{"id", i}, {"image", s.extras[i].image}, {"role", s.extras[i].role}};
        if (s.extras[i].block >= 0) em["block"] = s.extras[i].block;
        extras.push_back(em);
    }
    const json recipe = {{"iis_recipe", 1}, {"format", s.format}, {"writer", writer_info()}, {"planes", planes},
                         {"extras", extras}, {"layout", s.layout}, {"predictor", nullptr}};
    const std::string rtext = recipe.dump();

    auto sink = std::make_unique<io::MemorySink>();
    io::MemorySink* mem = sink.get();
    cont::Writer w;
    if (int st = w.begin(std::move(sink), n, sha)) return st;
    std::vector<uint8_t> buf;
    uint16_t codec = pack_side(std::vector<uint8_t>(rtext.begin(), rtext.end()), buf);
    if (int st = w.add(cont::RECIPE, codec, 0, rtext.size(), buf.data(), buf.size())) return st;
    if (!s.skeleton.empty()) {
        std::vector<uint8_t> raw;
        for (const auto& r : s.skeleton) raw.insert(raw.end(), d + r.offset, d + r.offset + r.length);
        codec = pack_side(raw, buf);
        if (int st = w.add_skeleton(0, codec, s.skeleton.data(), s.skeleton.size(), buf.data(), buf.size())) return st;
    }
    for (size_t i = 0; i < streams.size(); ++i) {
        const auto& p = s.planes[i];
        const uint64_t raw_len = p.width * p.height * p.components * (std::max(2, p.bits) <= 8 ? 1 : 2);
        if (int st = w.add(cont::PLANE, cont::JPEGLS, uint16_t(i), raw_len, streams[i].data(), streams[i].size()))
            return st;
    }
    for (size_t i = 0; i < s.extras.size(); ++i) {
        codec = pack_side(s.extras[i].bytes, buf);
        if (int st = w.add(cont::EXTRA, codec, uint16_t(i), s.extras[i].bytes.size(), buf.data(), buf.size())) return st;
    }
    if (int st = w.finish(false)) return st;
    out = std::move(mem->bytes());
    return IIS_OK;
}

}  // namespace

namespace pipeline {

void set_detail(char (&dst)[512], const std::string& s) {
    size_t n = std::min(s.size(), sizeof dst - 1);
    while (n > 0 && n < s.size() && (uint8_t(s[n]) & 0xC0) == 0x80) --n;  // 不截断在 UTF-8 字符中间
    std::memcpy(dst, s.data(), n);
    dst[n] = 0;
}

int normalize(const iis_compress_options* opt, iis_compress_options& o) {
    o = {};
    o.fallback_zstd_level = 19;
    if (opt) o = *opt;
    if (o.fallback_zstd_level == 0) o.fallback_zstd_level = 19;
    if (o.fallback_zstd_level < 1 || o.fallback_zstd_level > 19) return fail(IIS_E_INVALID_ARG, "zstd 级别须为 1~19");
    if (!(o.min_saving >= 0 && o.min_saving <= 0.9)) return fail(IIS_E_INVALID_ARG, "收益阈值须为 0~0.9");
    return IIS_OK;
}

int compress(const uint8_t* d, size_t size, const uint8_t sha[32], const iis_compress_options& o,
             std::vector<uint8_t>& best, iis_compress_result& result) {
    result = {};
    best.clear();
    // 压缩结果须不大于这个长度才采用（压了更大、或收益低于阈值就不压）
    const double limit_f = std::floor(double(size) * (1.0 - o.min_saving));
    const uint64_t limit = size == 0 ? 0 : std::min<uint64_t>(size - 1, uint64_t(limit_f));
    int method = 0;
    std::string detail;
    using clock = std::chrono::steady_clock;
    auto since = [](clock::time_point t) { return std::chrono::duration<double>(clock::now() - t).count(); };
    double t_analyze = 0, t_encode = 0, t_verify = 0;
    auto t0 = clock::now();
    const auto det = detect::detect(d, size, o.rel_path_utf8 ? o.rel_path_utf8 : "",
                                    o.raw_rules ? &o.raw_rules->rules : nullptr,
                                    o.raw_sidecar_json ? o.raw_sidecar_json : "", o.raw_auto != 0);
    const bool guessed = det.raw && det.raw->origin == "auto";
    const bool splittable = det.processable && (det.cls == "bmp" || det.cls == "bayer" || det.cls == "raw" ||
                                                det.cls == "tiff_uncompressed" || det.cls == "tiff_compressed");
    if (splittable) {
        layout::Split s;
        std::string why;
        std::vector<uint8_t> c;
        const bool split_ok = layout::split(d, size, det, s, why);
        t_analyze = since(t0);
        t0 = clock::now();
        const int enc_st = split_ok ? build_image_container(d, size, sha, s, c) : IIS_OK;
        t_encode = since(t0);
        t0 = clock::now();
        const int chk_st = split_ok && enc_st == IIS_OK ? self_check(c, "verify.image") : IIS_OK;
        t_verify = since(t0);
        if (!split_ok) {
            detail = "版面拆分失败：" + why;
        } else if (enc_st) {
            detail = std::string("图像编码失败：") + iis_last_error();
        } else if (chk_st) {
            detail = std::string("自检失败：") + iis_last_error();  // 不应发生；发生即兜底并上报
        } else if (c.size() > limit) {
            detail = o.min_saving > 0 ? "按图像压缩后收益低于阈值" : "按图像压缩后不小于原文件";
        } else {
            best = std::move(c);
            method = IIS_METHOD_IMAGE;
            detail = det.cls + (guessed ? "（RAW 参数自动识别：" + std::to_string(det.width) + "×" +
                                              std::to_string(det.height) + " " + det.pixel_format + "）"
                                        : "");
        }
    } else {
        detail = "不按图像处理：" + det.cls + (det.error.empty() ? "" : "（" + det.error + "）");
        t_analyze = since(t0);
    }
    if (method == IIS_METHOD_IMAGE && guessed) {
        // 参数是猜的：再试 zstd，取小的（猜错时图像压缩可能不如通用压缩）
        std::vector<uint8_t> z;
        t0 = clock::now();
        const bool packed = cont::pack_bytes(d, size, o.fallback_zstd_level, z) == IIS_OK && z.size() < best.size();
        t_encode += since(t0);
        if (packed && self_check(z, "verify.zstd") == IIS_OK) {
            best = std::move(z);
            method = IIS_METHOD_ZSTD;
            detail += "；zstd 更小，采用 zstd";
        }
    }
    if (!method) {
        std::vector<uint8_t> z;
        t0 = clock::now();
        const bool packed = fault::at("encode.zstd") == 0 && cont::pack_bytes(d, size, o.fallback_zstd_level, z) == IIS_OK;
        t_encode += since(t0);
        t0 = clock::now();
        const bool checked = packed && self_check(z, "verify.zstd") == IIS_OK;
        t_verify += since(t0);
        if (checked && z.size() <= limit) {
            best = std::move(z);
            method = IIS_METHOD_ZSTD;
        } else {
            method = IIS_METHOD_STORE;  // 压了更大（或收益不足、兜底也失败）：保留原文件
            if (!checked) detail += "；zstd 兜底失败，保留原文件";
            else if (z.size() < size) detail += "；zstd 收益也低于阈值，保留原文件";
            else detail += "；zstd 也不能缩小，保留原文件";
        }
    }
    result.method = method;
    result.orig_size = size;
    result.stored_size = method == IIS_METHOD_STORE ? size : best.size();
    result.seconds_analyze = t_analyze;
    result.seconds_encode = t_encode;
    result.seconds_verify = t_verify;
    set_detail(result.detail, detail);
    return IIS_OK;
}

}  // namespace pipeline

}  // namespace iis

using namespace iis;

extern "C" {

int iis_raw_rules_parse(const char* json_utf8, size_t len, iis_raw_rules** out) {
    IIS_GUARD_BEGIN
    if (!json_utf8 || !out) return fail(IIS_E_INVALID_ARG, "参数为空");
    *out = nullptr;
    auto r = std::make_unique<iis_raw_rules>();
    std::string why;
    if (!fmt::parse_raw_rules(std::string(json_utf8, len), r->rules, why)) return fail(IIS_E_INVALID_ARG, why);
    *out = r.release();
    return IIS_OK;
    IIS_GUARD_END
}

void iis_raw_rules_free(iis_raw_rules* r) { delete r; }

int iis_detect(const void* data, size_t size, const iis_compress_options* opt, iis_buffer* out) {
    IIS_GUARD_BEGIN
    if (!out || (!data && size)) return fail(IIS_E_INVALID_ARG, "参数为空");
    *out = {};
    const auto det = detect::detect(static_cast<const uint8_t*>(data), size,
                                    opt && opt->rel_path_utf8 ? opt->rel_path_utf8 : "",
                                    opt && opt->raw_rules ? &opt->raw_rules->rules : nullptr,
                                    opt && opt->raw_sidecar_json ? opt->raw_sidecar_json : "",
                                    opt && opt->raw_auto != 0);
    const std::string j = detect::to_json(det);
    if (!buffer_alloc(out, j.size())) return fail(IIS_E_NO_MEMORY, "内存不足");
    std::memcpy(out->data, j.data(), j.size());
    return IIS_OK;
    IIS_GUARD_END
}

int iis_compress(const void* data, size_t size, const iis_compress_options* opt, iis_buffer* container,
                 iis_compress_result* result) {
    IIS_GUARD_BEGIN
    if (!container || (!data && size)) return fail(IIS_E_INVALID_ARG, "参数为空");
    *container = {};
    if (result) *result = {};
    iis_compress_options o;
    if (int st = pipeline::normalize(opt, o)) return st;
    const auto* d = static_cast<const uint8_t*>(data);
    uint8_t sha[32];
    iis_sha256(d, size, sha);
    std::vector<uint8_t> best;
    iis_compress_result r{};
    if (int st = pipeline::compress(d, size, sha, o, best, r)) return st;
    if (r.method != IIS_METHOD_STORE) {
        if (!buffer_alloc(container, best.size())) return fail(IIS_E_NO_MEMORY, "内存不足");
        std::memcpy(container->data, best.data(), best.size());
    }
    if (result) *result = r;
    return IIS_OK;
    IIS_GUARD_END
}

}  // extern "C"
