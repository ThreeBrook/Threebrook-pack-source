// IIS 容器 v1 读写实现。字段布局、校验顺序与错误判定严格按 spec/container-v1.md。
#include "container.hpp"

#include "layout.hpp"

#include <charls/charls.h>
#include <nlohmann/json.hpp>
#include <zstd.h>

#include <algorithm>
#include <set>
#include <utility>

namespace iis::cont {

namespace {

constexpr uint8_t kMagic[8] = {'I', 'I', 'S', 'C', 'O', 'N', 'T', 0};
constexpr uint8_t kSegMagic[4] = {'I', 'S', 'E', 'G'};
constexpr uint8_t kIdxMagic[4] = {'I', 'I', 'D', 'X'};
constexpr uint8_t kEndMagic[4] = {'I', 'E', 'N', 'D'};

void put16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}
void put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}
void put64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (8 * i));
}
uint16_t get16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t get32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint64_t get64(const uint8_t* p) { return uint64_t(get32(p)) | uint64_t(get32(p + 4)) << 32; }

uint32_t crc(const void* p, size_t n) { return iis_crc32c(0, p, n); }

int corrupt(const std::string& msg) { return fail(IIS_E_CORRUPT, msg); }

void pack_entry(uint8_t* p, const Entry& e) {
    put64(p, e.offset);
    put16(p + 8, e.type);
    put16(p + 10, e.codec);
    put16(p + 12, e.flags);
    put16(p + 14, e.id);
    put32(p + 16, e.meta_len);
    put32(p + 20, e.data_crc);
    put64(p + 24, e.raw_len);
    put64(p + 32, e.data_len);
}

Entry unpack_entry(const uint8_t* p) {
    Entry e;
    e.offset = get64(p);
    e.type = get16(p + 8);
    e.codec = get16(p + 10);
    e.flags = get16(p + 12);
    e.id = get16(p + 14);
    e.meta_len = get32(p + 16);
    e.data_crc = get32(p + 20);
    e.raw_len = get64(p + 24);
    e.data_len = get64(p + 32);
    return e;
}

void pack_seg_head(uint8_t* p, const Entry& e) {
    std::memcpy(p, kSegMagic, 4);
    put16(p + 4, e.type);
    put16(p + 6, e.codec);
    put16(p + 8, e.flags);
    put16(p + 10, e.id);
    put32(p + 12, e.meta_len);
    put64(p + 16, e.raw_len);
    put64(p + 24, e.data_len);
    put32(p + 32, e.data_crc);
    put32(p + 36, crc(p, 36));
}

std::string seg_label(size_t i, const Entry& e) {
    return "第 " + std::to_string(i) + " 段（" + type_name(e.type) + " #" + std::to_string(e.id) + "）";
}

// 已知类型允许的 codec；htj2k / jpegxl 为 v1 预留，可读但本版本不能解码
bool codec_allowed(uint16_t type, uint16_t codec) {
    if (type == PLANE) return codec <= JPEGXL;
    return codec == NONE || codec == ZSTD;
}

bool codec_decodable(uint16_t codec) { return codec == NONE || codec == ZSTD || codec == JPEGLS; }

}  // namespace

bool known_type(uint16_t t) { return t >= RECIPE && t <= META; }
bool is_critical(uint16_t t) { return t != META; }

const char* type_name(uint16_t t) {
    switch (t) {
        case RECIPE: return "配方";
        case SKELETON: return "原样字节";
        case PLANE: return "像素平面";
        case EXTRA: return "附加字节";
        case META: return "描述信息";
        default: return "未知类型";
    }
}

const char* codec_name(uint16_t c) {
    switch (c) {
        case NONE: return "none";
        case ZSTD: return "zstd";
        case JPEGLS: return "jpegls";
        case HTJ2K: return "htj2k";
        case JPEGXL: return "jpegxl";
        default: return "unknown";
    }
}

// ---------------- 区间表 ----------------

namespace {

void leb_put(std::vector<uint8_t>& out, uint64_t v) {
    do {
        uint8_t b = v & 0x7F;
        v >>= 7;
        out.push_back(uint8_t(b | (v ? 0x80 : 0)));
    } while (v);
}

// 最短编码、不超过 64 位
bool leb_get(const uint8_t*& p, const uint8_t* end, uint64_t& v) {
    v = 0;
    for (int shift = 0; p < end; shift += 7) {
        const uint8_t b = *p++;
        if (shift == 63 && (b & 0x7E)) return false;  // 溢出
        v |= uint64_t(b & 0x7F) << shift;
        if (!(b & 0x80)) return !(b == 0 && shift > 0);  // 末字节为 0 即多余补零
        if (shift >= 63) return false;
    }
    return false;
}

}  // namespace

int encode_ranges(const Range* r, size_t n, uint64_t orig_size, std::vector<uint8_t>& meta, uint64_t& total) {
    meta.clear();
    total = 0;
    if (!n) return fail(IIS_E_INVALID_ARG, "区间表为空");
    leb_put(meta, n);
    uint64_t end = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!r[i].length) return fail(IIS_E_INVALID_ARG, "区间长度为 0");
        if (r[i].offset < end) return fail(IIS_E_INVALID_ARG, "区间未按升序或相互重叠");
        if (r[i].offset > orig_size || r[i].length > orig_size - r[i].offset)
            return fail(IIS_E_INVALID_ARG, "区间超出原文件长度");
        leb_put(meta, r[i].offset - end);
        leb_put(meta, r[i].length);
        end = r[i].offset + r[i].length;
        total += r[i].length;
    }
    return IIS_OK;
}

int decode_ranges(const uint8_t* meta, size_t len, uint64_t orig_size, std::vector<Range>& out, uint64_t& total) {
    out.clear();
    total = 0;
    const uint8_t* p = meta;
    const uint8_t* end = meta + len;
    uint64_t n = 0;
    if (!leb_get(p, end, n) || n == 0) return corrupt("区间表计数无效");
    if (n > len) return corrupt("区间表计数超出数据长度");  // 每个区间至少 2 字节，防止超大预分配
    out.reserve(size_t(n));
    uint64_t pos = 0;
    for (uint64_t i = 0; i < n; ++i) {
        uint64_t gap = 0, length = 0;
        if (!leb_get(p, end, gap) || !leb_get(p, end, length)) return corrupt("区间表编码损坏");
        if (!length) return corrupt("区间长度为 0");
        if (gap > orig_size - pos || length > orig_size - pos - gap) return corrupt("区间超出原文件长度");
        out.push_back({pos + gap, length});
        pos += gap + length;
        total += length;
    }
    if (p != end) return corrupt("区间表后有多余字节");
    return IIS_OK;
}

// ---------------- 写入 ----------------

int Writer::broken(int st) {
    failed_ = true;
    return st;
}

int Writer::begin(std::unique_ptr<io::Sink> sink, uint64_t orig_size, const uint8_t sha256[32]) {
    sink_ = std::move(sink);
    orig_size_ = orig_size;
    uint8_t h[kHeaderSize] = {};
    std::memcpy(h, kMagic, 8);
    put16(h + 8, kMajor);
    put16(h + 10, kMinor);
    put32(h + 12, kHeaderSize);
    put64(h + 24, orig_size);
    std::memcpy(h + 32, sha256, 32);
    put32(h + 68, crc(h, 68));
    if (int st = sink_->write(h, sizeof h)) return broken(st);
    return IIS_OK;
}

int Writer::put_segment(const Entry& e0, const void* meta, const void* data) {
    Entry e = e0;
    e.offset = sink_->position();
    e.flags = is_critical(e.type) ? kCritical : 0;
    e.data_crc = iis_crc32c(iis_crc32c(0, meta, e.meta_len), data, size_t(e.data_len));
    uint8_t head[kSegHead];
    pack_seg_head(head, e);
    if (int st = sink_->write(head, sizeof head)) return broken(st);
    if (e.meta_len)
        if (int st = sink_->write(meta, e.meta_len)) return broken(st);
    if (e.data_len)
        if (int st = sink_->write(data, size_t(e.data_len))) return broken(st);
    entries_.push_back(e);
    return IIS_OK;
}

int Writer::add(uint16_t type, uint16_t codec, uint16_t id, uint64_t raw_len, const void* data, size_t data_len) {
    if (failed_ || finished_) return fail(IIS_E_INVALID_ARG, "写入器已失败或已结束");
    if (!known_type(type) || type == SKELETON) return fail(IIS_E_INVALID_ARG, "段类型无效（原样字节段请用 add_skeleton）");
    if (!codec_allowed(type, codec) || !codec_decodable(codec))
        return fail(IIS_E_INVALID_ARG, std::string(type_name(type)) + "段不能使用编码 " + codec_name(codec));
    if (!data && data_len) return fail(IIS_E_INVALID_ARG, "数据指针为空");
    if (codec == NONE && raw_len != data_len) return fail(IIS_E_INVALID_ARG, "不编码时 raw_len 必须等于数据长度");
    if (raw_len > kMaxRaw) return fail(IIS_E_INVALID_ARG, "段过大");
    if ((type == RECIPE || type == META) && raw_len > kMaxJson) return fail(IIS_E_INVALID_ARG, "JSON 段超过 16 MiB");
    if (type != PLANE && type != EXTRA && id != 0) return fail(IIS_E_INVALID_ARG, "该类型段的 id 必须为 0");
    for (const auto& e : entries_)
        if (e.type == type && e.id == id) return fail(IIS_E_INVALID_ARG, "段类型与 id 重复");
    if (entries_.size() >= kMaxSegments) return fail(IIS_E_INVALID_ARG, "段数超过上限");
    Entry e;
    e.type = type;
    e.codec = codec;
    e.id = id;
    e.raw_len = raw_len;
    e.data_len = data_len;
    return put_segment(e, nullptr, data);
}

int Writer::add_skeleton(uint16_t id, uint16_t codec, const Range* r, size_t n, const void* data, size_t data_len) {
    if (failed_ || finished_) return fail(IIS_E_INVALID_ARG, "写入器已失败或已结束");
    if (codec != NONE && codec != ZSTD) return fail(IIS_E_INVALID_ARG, "原样字节段只能不编码或 zstd");
    if (!r || (!data && data_len)) return fail(IIS_E_INVALID_ARG, "参数为空");
    std::vector<uint8_t> meta;
    uint64_t total = 0;
    if (int st = encode_ranges(r, n, orig_size_, meta, total)) return st;
    if (meta.size() > kMaxMeta) return fail(IIS_E_INVALID_ARG, "区间表过大");
    if (codec == NONE && total != data_len) return fail(IIS_E_INVALID_ARG, "数据长度与区间总长不符");
    // 不同 SKELETON 段之间也不得重叠
    std::vector<Range> all = ranges_;
    all.insert(all.end(), r, r + n);
    std::sort(all.begin(), all.end(), [](const Range& a, const Range& b) { return a.offset < b.offset; });
    for (size_t i = 1; i < all.size(); ++i)
        if (all[i].offset < all[i - 1].offset + all[i - 1].length)
            return fail(IIS_E_INVALID_ARG, "与已写入的原样字节段区间重叠");
    for (const auto& e : entries_)
        if (e.type == SKELETON && e.id == id) return fail(IIS_E_INVALID_ARG, "段类型与 id 重复");
    Entry e;
    e.type = SKELETON;
    e.codec = codec;
    e.id = id;
    e.meta_len = uint32_t(meta.size());
    e.raw_len = total;
    e.data_len = data_len;
    if (int st = put_segment(e, meta.data(), data)) return st;
    ranges_ = std::move(all);
    return IIS_OK;
}

int Writer::finish(bool sync) {
    if (failed_ || finished_) return fail(IIS_E_INVALID_ARG, "写入器已失败或已结束");
    const auto recipes = std::count_if(entries_.begin(), entries_.end(), [](const Entry& e) { return e.type == RECIPE; });
    if (recipes != 1) return fail(IIS_E_INVALID_ARG, "容器必须恰好有一个配方段");

    const uint64_t index_offset = sink_->position();
    std::vector<uint8_t> idx(kIndexHead + kEntrySize * entries_.size() + 4);
    std::memcpy(idx.data(), kIdxMagic, 4);
    put32(&idx[4], uint32_t(entries_.size()));
    put32(&idx[8], kEntrySize);
    for (size_t i = 0; i < entries_.size(); ++i) pack_entry(&idx[kIndexHead + kEntrySize * i], entries_[i]);
    put32(&idx[idx.size() - 4], crc(idx.data(), idx.size() - 4));

    uint8_t t[kTrailer] = {};
    put64(t, index_offset);
    put64(t + 8, idx.size());
    put64(t + 16, index_offset + idx.size() + kTrailer);
    put32(t + 24, crc(t, 24));
    std::memcpy(t + 28, kEndMagic, 4);

    if (int st = sink_->write(idx.data(), idx.size())) return broken(st);
    if (int st = sink_->write(t, sizeof t)) return broken(st);
    if (int st = sink_->finish(sync)) return broken(st);
    finished_ = true;
    return IIS_OK;
}

void Writer::abort() {
    if (sink_ && !finished_) sink_->abort();
    failed_ = true;
}

// ---------------- 读取 ----------------

int Reader::open(std::unique_ptr<io::Source> src) {
    src_ = std::move(src);
    const uint64_t size = src_->size();
    if (size < kHeaderSize + kIndexHead + 4 + kTrailer) return corrupt("文件过短，不是完整的 IIS 容器");

    // 1. 文件头
    uint8_t h[kHeaderSize];
    if (int st = src_->read_at(0, h, sizeof h)) return st;
    if (std::memcmp(h, kMagic, 8) != 0) return corrupt("不是 IIS 容器（魔数不符）");
    major_ = get16(h + 8);
    minor_ = get16(h + 10);
    header_size_ = get32(h + 12);
    if (header_size_ < kHeaderSize || header_size_ > kMaxHeaderSize || header_size_ > size)
        return corrupt("文件头长度无效");
    std::vector<uint8_t> hdr(header_size_);
    if (int st = src_->read_at(0, hdr.data(), hdr.size())) return st;
    if (get32(&hdr[header_size_ - 4]) != crc(hdr.data(), header_size_ - 4)) return corrupt("文件头校验失败");
    if (major_ == 0 || major_ > kMajor)
        return fail(IIS_E_UNSUPPORTED, "容器主版本 " + std::to_string(major_) + " 不受支持（本库支持 1）");
    incompat_ = get32(h + 16);
    compat_ = get32(h + 20);
    if (incompat_ & ~kKnownIncompat)
        return fail(IIS_E_UNSUPPORTED, "容器使用了本库不支持的特性（不兼容特性位 " + std::to_string(incompat_) + "）");
    orig_size_ = get64(h + 24);
    std::memcpy(sha_, h + 32, 32);

    // 2. 文件尾
    uint8_t t[kTrailer];
    if (int st = src_->read_at(size - kTrailer, t, sizeof t)) return st;
    if (std::memcmp(t + 28, kEndMagic, 4) != 0) return corrupt("文件尾缺失或损坏（文件可能没有写完或被截断）");
    if (get32(t + 24) != crc(t, 24)) return corrupt("文件尾校验失败");
    const uint64_t index_offset = get64(t), index_size = get64(t + 8), declared = get64(t + 16);
    if (declared != size)
        return corrupt("容器长度不符：记录 " + std::to_string(declared) + " 字节，实际 " + std::to_string(size) + " 字节");

    // 3. 段表位置
    if (index_offset < header_size_ || index_offset > size || index_size > size - index_offset ||
        index_offset + index_size + kTrailer != size || index_size < kIndexHead + 4)
        return corrupt("段表位置无效");

    // 4. 段表
    std::vector<uint8_t> idx(static_cast<size_t>(index_size));
    if (int st = src_->read_at(index_offset, idx.data(), idx.size())) return st;
    if (std::memcmp(idx.data(), kIdxMagic, 4) != 0) return corrupt("段表魔数不符");
    if (get32(&idx[idx.size() - 4]) != crc(idx.data(), idx.size() - 4)) return corrupt("段表校验失败");
    const uint32_t count = get32(&idx[4]), entry_size = get32(&idx[8]);
    if (entry_size < kEntrySize || entry_size > kMaxEntrySize) return corrupt("段表项长度无效");
    if (count > kMaxSegments) return corrupt("段数超过上限");
    if (index_size != uint64_t(kIndexHead) + uint64_t(entry_size) * count + 4) return corrupt("段表长度与段数不符");

    // 5. 段表一致性
    entries_.clear();
    entries_.reserve(count);
    uint64_t pos = header_size_;
    std::set<std::pair<uint16_t, uint16_t>> seen;
    int recipes = 0, metas = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const Entry e = unpack_entry(&idx[kIndexHead + size_t(entry_size) * i]);
        const std::string where = seg_label(i, e);
        if (e.offset != pos) return corrupt(where + "位置不连续");
        if (e.meta_len > kMaxMeta) return corrupt(where + "meta 过长");
        if (e.raw_len > kMaxRaw) return corrupt(where + "声明的长度过大");
        const uint64_t span = uint64_t(kSegHead) + e.meta_len;
        if (e.data_len > index_offset || span > index_offset - e.data_len || pos > index_offset - span - e.data_len)
            return corrupt(where + "越过段表起点");
        pos += span + e.data_len;
        if (!seen.insert({e.type, e.id}).second) return corrupt(where + "类型与 id 重复");
        if (known_type(e.type)) {
            if ((e.flags & kCritical) != (is_critical(e.type) ? kCritical : 0)) return corrupt(where + "关键标志不符");
            if (e.type != SKELETON && e.meta_len != 0) return corrupt(where + "不应带 meta");
            if (e.type == SKELETON && e.meta_len == 0) return corrupt(where + "缺少区间表");
            if ((e.type == RECIPE || e.type == META) && e.id != 0) return corrupt(where + "id 必须为 0");
            if ((e.type == RECIPE || e.type == META) && e.raw_len > kMaxJson) return corrupt(where + "超过 16 MiB");
            if (e.codec == NONE && e.raw_len != e.data_len) return corrupt(where + "不编码但长度不一致");
            if (e.codec <= JPEGXL && !codec_allowed(e.type, e.codec)) return corrupt(where + "编码与段类型不符");
            if (!codec_decodable(e.codec) && why_not_.empty())
                why_not_ = where + "使用本版本不能解码的编码 " + codec_name(e.codec);
            recipes += e.type == RECIPE;
            metas += e.type == META;
        } else if ((e.flags & kCritical) && why_not_.empty()) {
            why_not_ = where + "是本版本不认识的关键段（类型 " + std::to_string(e.type) + "）";
        }
        entries_.push_back(e);
    }
    if (pos != index_offset) return corrupt("最后一段的终点与段表起点不符");
    if (recipes != 1) return corrupt("配方段数量为 " + std::to_string(recipes) + "，应恰好 1 个");
    if (metas > 1) return corrupt("描述信息段多于 1 个");
    return IIS_OK;
}

int Reader::find(uint16_t type, uint16_t id) const {
    for (size_t i = 0; i < entries_.size(); ++i)
        if (entries_[i].type == type && entries_[i].id == id) return int(i);
    return -1;
}

int Reader::read_segment(size_t i, std::vector<uint8_t>* meta, Buf& data) {
    if (i >= entries_.size()) return fail(IIS_E_INVALID_ARG, "段序号越界");
    const Entry& e = entries_[i];
    const std::string where = seg_label(i, e);
    uint8_t head[kSegHead];
    if (int st = src_->read_at(e.offset, head, sizeof head)) return st;
    if (std::memcmp(head, kSegMagic, 4) != 0) return corrupt(where + "段头魔数不符");
    if (get32(head + 36) != crc(head, 36)) return corrupt(where + "段头校验失败");
    if (get16(head + 4) != e.type || get16(head + 6) != e.codec || get16(head + 8) != e.flags ||
        get16(head + 10) != e.id || get32(head + 12) != e.meta_len || get64(head + 16) != e.raw_len ||
        get64(head + 24) != e.data_len || get32(head + 32) != e.data_crc)
        return corrupt(where + "段头与段表不一致");

    std::vector<uint8_t> m(e.meta_len);
    if (e.meta_len)
        if (int st = src_->read_at(e.offset + kSegHead, m.data(), m.size())) return st;
    iis_buffer_free(&data.b);
    if (e.data_len > SIZE_MAX || !buffer_alloc(&data.b, size_t(e.data_len))) return fail(IIS_E_NO_MEMORY, "内存不足");
    if (e.data_len)
        if (int st = src_->read_at(e.offset + kSegHead + e.meta_len, data.b.data, data.b.size)) return st;
    if (iis_crc32c(iis_crc32c(0, m.data(), m.size()), data.b.data, data.b.size) != e.data_crc)
        return corrupt(where + "数据校验失败（CRC32C 不符）");
    if (meta) *meta = std::move(m);
    return IIS_OK;
}

int Reader::decode_segment(size_t i, Buf& out, std::vector<uint8_t>* meta) {
    if (i >= entries_.size()) return fail(IIS_E_INVALID_ARG, "段序号越界");
    const Entry& e = entries_[i];
    const std::string where = seg_label(i, e);
    Buf data;
    if (int st = read_segment(i, meta, data)) return st;
    iis_buffer_free(&out.b);
    switch (e.codec) {
        case NONE:
            out.b = data.release();
            return IIS_OK;
        case ZSTD: {
            const int st = iis_zstd_decompress(data.b.data, data.b.size, e.raw_len, &out.b);
            if (st) return fail(st == IIS_E_NO_MEMORY ? st : IIS_E_CORRUPT, where + "：" + iis_last_error());
            return IIS_OK;
        }
        case JPEGLS: {
            const int st = iis_jpegls_decode(data.b.data, data.b.size, nullptr, &out.b);
            if (st) return fail(st == IIS_E_NO_MEMORY ? st : IIS_E_CORRUPT, where + "：" + iis_last_error());
            if (out.b.size != e.raw_len) return corrupt(where + "解码长度与记录不符");
            return IIS_OK;
        }
        default:
            return fail(IIS_E_UNSUPPORTED, where + "使用本版本不能解码的编码 " + codec_name(e.codec));
    }
}

int Reader::skeleton_ranges(size_t i, std::vector<Range>& out) {
    if (i >= entries_.size() || entries_[i].type != SKELETON) return fail(IIS_E_INVALID_ARG, "不是原样字节段");
    std::vector<uint8_t> meta;
    Buf data;
    if (int st = read_segment(i, &meta, data)) return st;
    uint64_t total = 0;
    if (int st = decode_ranges(meta.data(), meta.size(), orig_size_, out, total))
        return fail(st, seg_label(i, entries_[i]) + iis_last_error());
    if (total != entries_[i].raw_len) return corrupt(seg_label(i, entries_[i]) + "区间总长与记录不符");
    return IIS_OK;
}

int Reader::recipe_json(std::string& out) {
    const int i = find(RECIPE, 0);
    if (i < 0) return corrupt("缺少配方段");
    Buf b;
    if (int st = decode_segment(size_t(i), b)) return st;
    out.assign(reinterpret_cast<const char*>(b.b.data), b.b.size);
    return IIS_OK;
}

int Reader::check_recipe(nlohmann::json* parsed) {
    std::string text;
    if (int st = recipe_json(text)) return st;
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(text);
    } catch (const nlohmann::json::exception& e) {
        return corrupt(std::string("配方不是合法 JSON：") + e.what());
    }
    if (!j.is_object() || !j.contains("iis_recipe") || !j["iis_recipe"].is_number_unsigned())
        return corrupt("配方缺少 iis_recipe 版本号");
    const auto ver = j["iis_recipe"].get<uint64_t>();
    if (ver != 1) return fail(IIS_E_UNSUPPORTED, "配方版本 " + std::to_string(ver) + " 不受支持（本库支持 1）");
    for (const char* k : {"format", "writer", "planes", "extras", "layout", "predictor"})
        if (!j.contains(k)) return corrupt(std::string("配方缺少字段 ") + k);
    if (!j["format"].is_string() || !j["planes"].is_array() || !j["extras"].is_array() || !j["layout"].is_object())
        return corrupt("配方字段类型不符");
    if (!j["predictor"].is_null()) return fail(IIS_E_UNSUPPORTED, "配方使用了领域预测层（本版本不支持）");

    // planes[] / extras[] 与 PLANE / EXTRA 段一一对应
    auto match = [&](const char* key, uint16_t type) -> int {
        std::set<uint16_t> ids;
        for (const auto& p : j[key]) {
            if (!p.is_object() || !p.contains("id") || !p["id"].is_number_unsigned() || p["id"].get<uint64_t>() > 65535)
                return corrupt(std::string("配方 ") + key + " 中的 id 无效");
            const auto id = uint16_t(p["id"].get<uint64_t>());
            if (!ids.insert(id).second) return corrupt(std::string("配方 ") + key + " 中 id 重复");
            const int s = find(type, id);
            if (s < 0) return corrupt(std::string("配方 ") + key + " 的 id " + std::to_string(id) + " 没有对应的段");
            if (type == PLANE) {
                if (!p.contains("codec") || !p["codec"].is_string() ||
                    p["codec"].get<std::string>() != codec_name(entries_[size_t(s)].codec))
                    return corrupt("配方 planes 的 id " + std::to_string(id) + " 编码与段不符");
            }
        }
        for (const auto& e : entries_)
            if (e.type == type && !ids.count(e.id))
                return corrupt(std::string(type_name(type)) + "段 #" + std::to_string(e.id) + " 在配方中没有描述");
        return IIS_OK;
    };
    if (int st = match("planes", PLANE)) return st;
    if (int st = match("extras", EXTRA)) return st;
    if (parsed) *parsed = std::move(j);
    return IIS_OK;
}

int Reader::verify(bool deep) {
    // 逐段：段头与段表一致、CRC；原样字节段的区间表合法且互不重叠
    std::vector<Range> all;
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].type == SKELETON) {
            std::vector<Range> r;
            if (int st = skeleton_ranges(i, r)) return st;
            all.insert(all.end(), r.begin(), r.end());
        } else {
            Buf data;
            if (int st = read_segment(i, nullptr, data)) return st;
        }
    }
    std::sort(all.begin(), all.end(), [](const Range& a, const Range& b) { return a.offset < b.offset; });
    for (size_t i = 1; i < all.size(); ++i)
        if (all[i].offset < all[i - 1].offset + all[i - 1].length) return corrupt("原样字节段之间区间重叠");
    if (int st = check_recipe()) return st;
    if (!deep) return IIS_OK;
    Buf out;
    return restore(out);
}

int Reader::restore(Buf& out) {
    if (!restorable()) return fail(IIS_E_UNSUPPORTED, "不能还原：" + why_not_);
    nlohmann::json recipe;
    if (int st = check_recipe(&recipe)) return st;

    iis_buffer_free(&out.b);
    if (orig_size_ > kMaxRaw || orig_size_ > SIZE_MAX) return corrupt("原文件长度过大");
    if (!buffer_alloc(&out.b, size_t(orig_size_))) return fail(IIS_E_NO_MEMORY, "内存不足");
    std::vector<Range> all;
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].type != SKELETON) continue;
        std::vector<uint8_t> meta;
        Buf bytes;
        if (int st = decode_segment(i, bytes, &meta)) return st;
        std::vector<Range> r;
        uint64_t total = 0;
        if (int st = decode_ranges(meta.data(), meta.size(), orig_size_, r, total))
            return fail(st, seg_label(i, entries_[i]) + iis_last_error());
        if (total != entries_[i].raw_len || bytes.b.size != total) return corrupt(seg_label(i, entries_[i]) + "区间总长与记录不符");
        size_t pos = 0;
        for (const auto& x : r) {
            std::memcpy(out.b.data + x.offset, bytes.b.data + pos, size_t(x.length));
            pos += size_t(x.length);
        }
        all.insert(all.end(), r.begin(), r.end());
    }
    // 像素平面与附加字节：交给格式处理器按配方版面写回
    if (recipe["format"] != "bytes") {
        const layout::Fetch fetch = [this](uint16_t type, uint16_t id, std::vector<uint8_t>& bytes) -> int {
            const int i = find(type, id);
            if (i < 0) return corrupt(std::string("缺少") + type_name(type) + "段 #" + std::to_string(id));
            Buf b;
            if (int st = decode_segment(size_t(i), b)) return st;
            bytes.assign(b.b.data, b.b.data + b.b.size);
            return IIS_OK;
        };
        if (int st = layout::rebuild(recipe, fetch, out.b.data, orig_size_, all)) return st;
    }
    // 每个字节恰好填一次：原样字节区间与像素区互不重叠，且总长等于原文件长度
    std::sort(all.begin(), all.end(), [](const Range& a, const Range& b) { return a.offset < b.offset; });
    uint64_t covered = 0;
    for (size_t i = 0; i < all.size(); ++i) {
        if (i && all[i].offset < all[i - 1].offset + all[i - 1].length) return corrupt("还原区间重叠");
        covered += all[i].length;
    }
    if (covered != orig_size_) return corrupt("还原区间没有恰好覆盖整个原文件");
    uint8_t d[32];
    iis_sha256(out.b.data, out.b.size, d);
    if (std::memcmp(d, sha_, 32) != 0) return corrupt("还原结果的 SHA-256 与原文件不符");
    return IIS_OK;
}

// ---------------- 兜底容器 ----------------

std::string bytes_recipe_json() {
    // nlohmann::json 的对象按键名排序，输出紧凑——与 Python json.dumps(sort_keys=True, separators=(",", ":")) 一致
    nlohmann::json j = {
        {"iis_recipe", 1},
        {"format", "bytes"},
        {"writer", {{"name", "libiis"}, {"version", IIS_VERSION_STRING},
                    {"libs", {{"charls", charls_get_version_string()}, {"zstd", ZSTD_versionString()}}}}},
        {"planes", nlohmann::json::array()},
        {"extras", nlohmann::json::array()},
        {"layout", nlohmann::json::object()},
        {"predictor", nullptr},
    };
    return j.dump();
}

int pack_bytes(const void* data, size_t size, int zstd_level, std::vector<uint8_t>& out) {
    if (!data && size) return fail(IIS_E_INVALID_ARG, "数据指针为空");
    uint8_t sha[32];
    iis_sha256(data, size, sha);
    auto sink = std::make_unique<io::MemorySink>();
    io::MemorySink* mem = sink.get();
    Writer w;
    if (int st = w.begin(std::move(sink), size, sha)) return st;
    const std::string recipe = bytes_recipe_json();
    if (int st = w.add(RECIPE, NONE, 0, recipe.size(), recipe.data(), recipe.size())) return st;
    if (size) {
        const Range r{0, size};
        if (zstd_level > 0) {
            Buf z;
            if (int st = iis_zstd_compress(data, size, zstd_level, &z.b)) return st;
            if (int st = w.add_skeleton(0, ZSTD, &r, 1, z.b.data, z.b.size)) return st;
        } else if (int st = w.add_skeleton(0, NONE, &r, 1, data, size)) {
            return st;
        }
    }
    if (int st = w.finish(false)) return st;
    out = std::move(mem->bytes());
    return IIS_OK;
}

}  // namespace iis::cont
