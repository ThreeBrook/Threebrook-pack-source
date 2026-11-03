// IIS 容器 v1 读写（spec/container-v1.md）。C 接口见 container_api.cpp。
#pragma once

#include "io.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace iis::cont {

constexpr uint16_t kMajor = 1, kMinor = 0;
constexpr uint32_t kHeaderSize = 72, kSegHead = 40, kEntrySize = 40, kIndexHead = 16, kTrailer = 32;
constexpr uint32_t kKnownIncompat = 0;  // v1.0 不支持任何不兼容特性（第 0 位 PREDICTION 预留）

// 第 15 节限制值
constexpr uint32_t kMaxSegments = 65536, kMaxEntrySize = 4096, kMaxHeaderSize = 4096;
constexpr uint64_t kMaxMeta = 16ull << 20, kMaxJson = 16ull << 20, kMaxRaw = 8ull << 30;

enum : uint16_t { RECIPE = 1, SKELETON = 2, PLANE = 3, EXTRA = 4, META = 5 };
enum : uint16_t { NONE = 0, ZSTD = 1, JPEGLS = 2, HTJ2K = 3, JPEGXL = 4 };
constexpr uint16_t kCritical = 1;

struct Entry {
    uint64_t offset = 0;
    uint16_t type = 0, codec = 0, flags = 0, id = 0;
    uint32_t meta_len = 0, data_crc = 0;
    uint64_t raw_len = 0, data_len = 0;
};

struct Range {
    uint64_t offset, length;
};

bool known_type(uint16_t type);
bool is_critical(uint16_t type);
const char* type_name(uint16_t type);
const char* codec_name(uint16_t codec);

// SKELETON 区间表（LEB128，第 6.4 节）
int encode_ranges(const Range* r, size_t n, uint64_t orig_size, std::vector<uint8_t>& meta, uint64_t& total);
int decode_ranges(const uint8_t* meta, size_t len, uint64_t orig_size, std::vector<Range>& out, uint64_t& total);

// 自动释放的 iis_buffer
struct Buf {
    iis_buffer b{};
    Buf() = default;
    Buf(const Buf&) = delete;
    Buf& operator=(const Buf&) = delete;
    ~Buf() { iis_buffer_free(&b); }
    iis_buffer release() {
        iis_buffer r = b;
        b = {};
        return r;
    }
};

class Writer {
public:
    int begin(std::unique_ptr<io::Sink> sink, uint64_t orig_size, const uint8_t sha256[32]);
    // RECIPE / PLANE / EXTRA / META；data 为已编码内容，raw_len 为解码后长度
    int add(uint16_t type, uint16_t codec, uint16_t id, uint64_t raw_len, const void* data, size_t data_len);
    int add_skeleton(uint16_t id, uint16_t codec, const Range* ranges, size_t n, const void* data, size_t data_len);
    int finish(bool sync);
    void abort();
    io::Sink* sink() { return sink_.get(); }

private:
    int put_segment(const Entry& e, const void* meta, const void* data);
    int broken(int st);

    std::unique_ptr<io::Sink> sink_;
    uint64_t orig_size_ = 0;
    std::vector<Entry> entries_;
    std::vector<Range> ranges_;
    bool failed_ = false, finished_ = false;
};

class Reader {
public:
    int open(std::unique_ptr<io::Source> src);

    uint16_t major() const { return major_; }
    uint16_t minor() const { return minor_; }
    uint32_t incompat() const { return incompat_; }
    uint32_t compat() const { return compat_; }
    uint64_t orig_size() const { return orig_size_; }
    const uint8_t* orig_sha256() const { return sha_; }
    uint64_t container_size() const { return src_->size(); }
    const std::vector<Entry>& entries() const { return entries_; }
    bool restorable() const { return why_not_.empty(); }
    const std::string& why_not_restorable() const { return why_not_; }
    int find(uint16_t type, uint16_t id) const;  // 段序号，没有返回 -1

    // 校验段头与段表一致、data_crc 后取出 meta 与 data（data 为容器中的原样字节）
    int read_segment(size_t i, std::vector<uint8_t>* meta, Buf& data);
    // 再按 codec 解码；长度必须等于 raw_len
    int decode_segment(size_t i, Buf& out, std::vector<uint8_t>* meta = nullptr);
    int skeleton_ranges(size_t i, std::vector<Range>& out);
    int recipe_json(std::string& out);
    int check_recipe(nlohmann::json* parsed = nullptr);  // 配方外层与 PLANE / EXTRA 段对应关系
    int verify(bool deep);
    int restore(Buf& out);            // 还原并比对 SHA-256

private:
    std::unique_ptr<io::Source> src_;
    uint16_t major_ = 0, minor_ = 0;
    uint32_t header_size_ = 0, incompat_ = 0, compat_ = 0;
    uint64_t orig_size_ = 0;
    uint8_t sha_[32] = {};
    std::vector<Entry> entries_;
    std::string why_not_;
};

// 整文件兜底容器：配方 format=bytes + 覆盖全文件的 SKELETON 段（zstd_level 为 0 时不压缩）
int pack_bytes(const void* data, size_t size, int zstd_level, std::vector<uint8_t>& out);
std::string bytes_recipe_json();

}  // namespace iis::cont
