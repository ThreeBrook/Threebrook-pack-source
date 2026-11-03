// 容器的 C 接口：把 iis::cont::Writer / Reader 包成不透明句柄。
#include "container.hpp"

using namespace iis;
using namespace iis::cont;

struct iis_writer {
    Writer w;
    io::MemorySink* mem = nullptr;  // 内存写入方时指向 sink（由 Writer 持有）
};

struct iis_reader {
    Reader r;
};

namespace {

int out_buffer(iis_buffer* out, std::vector<uint8_t>& v) {
    if (!buffer_alloc(out, v.size())) return fail(IIS_E_NO_MEMORY, "内存不足");
    if (!v.empty()) std::memcpy(out->data, v.data(), v.size());
    return IIS_OK;
}

int new_writer(std::unique_ptr<io::Sink> sink, uint64_t orig_size, const uint8_t* sha, iis_writer** out) {
    auto w = std::make_unique<iis_writer>();
    w->mem = dynamic_cast<io::MemorySink*>(sink.get());
    if (int st = w->w.begin(std::move(sink), orig_size, sha)) return st;
    *out = w.release();
    return IIS_OK;
}

int new_reader(std::unique_ptr<io::Source> src, iis_reader** out) {
    auto r = std::make_unique<iis_reader>();
    if (int st = r->r.open(std::move(src))) return st;
    *out = r.release();
    return IIS_OK;
}

}  // namespace

extern "C" {

int iis_writer_create_file(const char* path, uint64_t orig_size, const uint8_t sha[32], iis_writer** out) {
    IIS_GUARD_BEGIN
    if (!out || !sha) return fail(IIS_E_INVALID_ARG, "参数为空");
    *out = nullptr;
    std::unique_ptr<io::Sink> sink;
    if (int st = io::open_file_sink(path, sink)) return st;
    return new_writer(std::move(sink), orig_size, sha, out);
    IIS_GUARD_END
}

int iis_writer_create_memory(uint64_t orig_size, const uint8_t sha[32], iis_writer** out) {
    IIS_GUARD_BEGIN
    if (!out || !sha) return fail(IIS_E_INVALID_ARG, "参数为空");
    *out = nullptr;
    return new_writer(std::make_unique<io::MemorySink>(), orig_size, sha, out);
    IIS_GUARD_END
}

int iis_writer_add(iis_writer* w, uint16_t type, uint16_t codec, uint16_t id, uint64_t raw_len, const void* data,
                   size_t data_len) {
    IIS_GUARD_BEGIN
    if (!w) return fail(IIS_E_INVALID_ARG, "写入器为空");
    return w->w.add(type, codec, id, raw_len, data, data_len);
    IIS_GUARD_END
}

int iis_writer_add_skeleton(iis_writer* w, uint16_t id, uint16_t codec, const iis_range* ranges, size_t count,
                            const void* data, size_t data_len) {
    IIS_GUARD_BEGIN
    if (!w) return fail(IIS_E_INVALID_ARG, "写入器为空");
    static_assert(sizeof(iis_range) == sizeof(Range), "iis_range 与内部 Range 布局须一致");
    return w->w.add_skeleton(id, codec, reinterpret_cast<const Range*>(ranges), count, data, data_len);
    IIS_GUARD_END
}

int iis_writer_finish(iis_writer* w, int sync, iis_buffer* out) {
    IIS_GUARD_BEGIN
    if (!w) return fail(IIS_E_INVALID_ARG, "写入器为空");
    if (out) *out = {};
    if (w->mem && !out) return fail(IIS_E_INVALID_ARG, "内存写入方需要输出缓冲区");
    if (int st = w->w.finish(sync != 0)) return st;
    return w->mem ? out_buffer(out, w->mem->bytes()) : IIS_OK;
    IIS_GUARD_END
}

void iis_writer_free(iis_writer* w) {
    if (!w) return;
    w->w.abort();
    delete w;
}

int iis_reader_open_file(const char* path, iis_reader** out) {
    IIS_GUARD_BEGIN
    if (!out) return fail(IIS_E_INVALID_ARG, "参数为空");
    *out = nullptr;
    std::unique_ptr<io::Source> src;
    if (int st = io::open_file_source(path, src)) return st;
    return new_reader(std::move(src), out);
    IIS_GUARD_END
}

int iis_reader_open_memory(const void* data, size_t size, iis_reader** out) {
    IIS_GUARD_BEGIN
    if (!out || (!data && size)) return fail(IIS_E_INVALID_ARG, "参数为空");
    *out = nullptr;
    return new_reader(std::make_unique<io::MemorySource>(data, size), out);
    IIS_GUARD_END
}

void iis_reader_close(iis_reader* r) { delete r; }

int iis_reader_info(const iis_reader* r, iis_container_info* info) {
    if (!r || !info) return fail(IIS_E_INVALID_ARG, "参数为空");
    const Reader& x = r->r;
    *info = {};
    info->version_major = x.major();
    info->version_minor = x.minor();
    info->incompat_flags = x.incompat();
    info->compat_flags = x.compat();
    info->segment_count = uint32_t(x.entries().size());
    info->orig_size = x.orig_size();
    std::memcpy(info->orig_sha256, x.orig_sha256(), 32);
    info->container_size = x.container_size();
    info->restorable = x.restorable() ? 1 : 0;
    return IIS_OK;
}

int iis_reader_segment(const iis_reader* r, uint32_t index, iis_segment_info* info) {
    if (!r || !info) return fail(IIS_E_INVALID_ARG, "参数为空");
    if (index >= r->r.entries().size()) return fail(IIS_E_INVALID_ARG, "段序号越界");
    const Entry& e = r->r.entries()[index];
    *info = {e.offset, e.raw_len, e.data_len, e.meta_len, e.data_crc, e.type, e.codec, e.flags, e.id};
    return IIS_OK;
}

int iis_reader_find(const iis_reader* r, uint16_t type, uint16_t id, uint32_t* index) {
    if (!r || !index) return fail(IIS_E_INVALID_ARG, "参数为空");
    const int i = r->r.find(type, id);
    if (i < 0) return fail(IIS_E_NOT_FOUND, "没有该段");
    *index = uint32_t(i);
    return IIS_OK;
}

int iis_reader_read(iis_reader* r, uint32_t index, iis_buffer* meta, iis_buffer* data) {
    IIS_GUARD_BEGIN
    if (!r) return fail(IIS_E_INVALID_ARG, "读取器为空");
    if (meta) *meta = {};
    if (data) *data = {};
    std::vector<uint8_t> m;
    Buf d;
    if (int st = r->r.read_segment(index, &m, d)) return st;
    if (meta)
        if (int st = out_buffer(meta, m)) return st;
    if (data) *data = d.release();
    return IIS_OK;
    IIS_GUARD_END
}

int iis_reader_decode(iis_reader* r, uint32_t index, iis_buffer* out) {
    IIS_GUARD_BEGIN
    if (!r || !out) return fail(IIS_E_INVALID_ARG, "参数为空");
    *out = {};
    Buf b;
    if (int st = r->r.decode_segment(index, b)) return st;
    *out = b.release();
    return IIS_OK;
    IIS_GUARD_END
}

int iis_reader_ranges(iis_reader* r, uint32_t index, iis_buffer* out) {
    IIS_GUARD_BEGIN
    if (!r || !out) return fail(IIS_E_INVALID_ARG, "参数为空");
    *out = {};
    std::vector<Range> v;
    if (int st = r->r.skeleton_ranges(index, v)) return st;
    if (!buffer_alloc(out, v.size() * sizeof(iis_range))) return fail(IIS_E_NO_MEMORY, "内存不足");
    std::memcpy(out->data, v.data(), out->size);
    return IIS_OK;
    IIS_GUARD_END
}

int iis_reader_recipe(iis_reader* r, iis_buffer* out) {
    IIS_GUARD_BEGIN
    if (!r || !out) return fail(IIS_E_INVALID_ARG, "参数为空");
    *out = {};
    std::string s;
    if (int st = r->r.recipe_json(s)) return st;
    std::vector<uint8_t> v(s.begin(), s.end());
    return out_buffer(out, v);
    IIS_GUARD_END
}

int iis_reader_verify(iis_reader* r, int deep) {
    IIS_GUARD_BEGIN
    if (!r) return fail(IIS_E_INVALID_ARG, "读取器为空");
    return r->r.verify(deep != 0);
    IIS_GUARD_END
}

int iis_reader_restore(iis_reader* r, iis_buffer* out) {
    IIS_GUARD_BEGIN
    if (!r || !out) return fail(IIS_E_INVALID_ARG, "参数为空");
    *out = {};
    Buf b;
    if (int st = r->r.restore(b)) return st;
    *out = b.release();
    return IIS_OK;
    IIS_GUARD_END
}

int iis_reader_restore_file(iis_reader* r, const char* out_path, int sync) {
    IIS_GUARD_BEGIN
    if (!r) return fail(IIS_E_INVALID_ARG, "读取器为空");
    Buf b;
    if (int st = r->r.restore(b)) return st;  // 先在内存中还原并比对 SHA-256，通过后才建文件
    std::unique_ptr<io::Sink> sink;
    if (int st = io::open_file_sink(out_path, sink)) return st;
    if (int st = sink->write(b.b.data, b.b.size)) return st;  // 失败时 sink 析构删除不完整文件
    return sink->finish(sync != 0);
    IIS_GUARD_END
}

int iis_pack_bytes(const void* data, size_t size, int zstd_level, iis_buffer* out) {
    IIS_GUARD_BEGIN
    if (!out) return fail(IIS_E_INVALID_ARG, "输出缓冲区为空");
    *out = {};
    if (zstd_level < 0 || zstd_level > 19) return fail(IIS_E_INVALID_ARG, "zstd 级别须为 0~19");
    std::vector<uint8_t> v;
    if (int st = pack_bytes(data, size, zstd_level, v)) return st;
    return out_buffer(out, v);
    IIS_GUARD_END
}

}  // extern "C"
