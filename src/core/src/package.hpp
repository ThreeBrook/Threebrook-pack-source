// 包（.iisz）：把一个文件夹压成一个文件，像 ZIP 一样用（格式见 spec/package-v1.md）。
//
// 包就是标准 ZIP（条目只存不压、UTF-8 文件名、按需 ZIP64），普通解压软件也能打开：
//   - 图像等压得小的文件按单文件规则压缩（iis_compress，已自检），存为“原名.iis”条目；
//   - 压不小的文件（JPEG、PNG、已压缩的数据等）原样存放；
//   - 每个文件条目在中央目录带 IIS 附加字段（0x4949）：原文件长度、SHA-256、修改时间、是否为容器。
// 打包先写临时文件，落盘后读回核对 SHA-256，一致才改名为最终的包；可选再逐个核对原文件未被修改后删除。
// 解包：每个文件先核对 ZIP CRC-32 与原文件 SHA-256，写临时文件再改名；已存在的文件一律不覆盖。
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace iis::io {
class Source;
}

namespace iis::pkg {

constexpr int kCancelled = -1000;  // 用户取消（打包：不留下任何文件）

struct Progress {
    uint64_t files_total = 0, files_done = 0;
    uint64_t bytes_total = 0, bytes_done = 0;  // 按原文件字节计
    std::string current;                       // 刚处理完的相对路径
};
using ProgressFn = std::function<void(const Progress&)>;

struct PackOptions {
    int threads = 0;                // 0：按 CPU 核数（最多 8）
    bool delete_originals = false;  // 包写好并核对后，逐个核对原文件未被修改再删除，再删除变空的文件夹
    double min_saving = 0.01;       // 压缩收益低于此比例的文件原样存放
    int zstd_level = 0;             // 兜底 zstd 级别；0 为默认 19
    bool force_zip64 = false;       // 仅测试：总是写 ZIP64 结构
    const std::atomic<bool>* cancel = nullptr;
};

struct PackResult {
    int status = 0;  // IIS_OK；否则包没有生成（error 为原因），原文件不受任何影响
    std::string error;
    std::string package;  // 生成的包
    uint64_t files = 0, dirs = 0;
    uint64_t as_image = 0, as_zstd = 0, as_stored = 0;  // 按图像压缩 / zstd 兜底 / 原样存放的文件数
    uint64_t orig_bytes = 0, package_bytes = 0;
    uint64_t unreadable = 0;    // 读不了、没有打进包的文件
    uint64_t released = 0;      // 已删除的原文件
    uint64_t not_released = 0;  // 要求删除但保留了的原文件（被占用、只读、压缩后被修改等）
    std::vector<std::string> problems;  // “相对路径：原因”，最多 200 条
    double seconds = 0;
};

// 把文件夹 folder 打成包，包内以文件夹名为顶层目录。out 为空时在 folder 旁边生成“文件夹名.iisz”
// （已存在时为“文件夹名 (2).iisz”…，绝不覆盖）。读不了的文件不打进包、计入 unreadable，其余照常；
// delete_originals 只删除打进了包、且与打包时内容一致的原文件。
PackResult pack_folder(const std::string& folder, const std::string& out, const PackOptions& opt,
                       const ProgressFn& progress = {});

struct Entry {
    std::string name;        // 包内名称（'/' 分隔，目录以 '/' 结尾）
    std::string path;        // 解出后的相对路径（'/' 分隔；容器条目去掉 .iis）
    std::string bad;         // 名称不安全（含 ..、盘符、非法字符等）时为原因，这样的条目不解出
    bool dir = false;
    bool iis = false;        // 内容是 IIS 容器，解出时还原
    bool has_meta = false;   // 带 IIS 附加字段
    uint64_t size = 0;       // 条目数据长度
    uint64_t orig_size = 0;  // 原文件长度（没有附加字段时等于 size）
    uint8_t sha256[32] = {};
    int64_t mtime = 0;       // 修改时间（FILETIME：1601-01-01 起的 100 纳秒数），0 表示未知
    uint32_t crc = 0;
    uint64_t header_offset = 0;
};

class Reader {
public:
    Reader();
    ~Reader();
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    // 读中央目录。不是 ZIP、结构损坏、条目经过压缩或加密时失败，原因见 error()
    int open(const std::string& path);
    // 同上，从任意字节源读（如资源管理器交给缩略图程序的流）；name 只用于 path()
    int open(std::unique_ptr<io::Source> src, const std::string& name);
    const std::vector<Entry>& entries() const { return entries_; }
    const std::string& error() const { return error_; }
    const std::string& path() const { return path_; }
    // 条目的原样数据，核对 CRC-32。可在多个线程中同时调用
    int read(size_t index, std::vector<uint8_t>& data, std::string& why) const;
    // 原文件内容：容器条目还原并核对 SHA-256；原样条目有附加字段时也核对 SHA-256。可在多个线程中同时调用
    int original(size_t index, std::vector<uint8_t>& out, std::string& why) const;

private:
    struct Impl;
    Impl* impl_;
    std::vector<Entry> entries_;
    std::string error_, path_;
};

struct ExtractOptions {
    int threads = 0;
    // true（“解压到此处”）：解到包所在的文件夹。包内只有一个顶层文件夹时直接还原它，否则放进“包名\”；
    // 同名文件夹已存在时改用“名 (2)”…，所以不会与已有文件混在一起。
    bool here = true;
    std::string dest;  // here 为 false 时解到这个文件夹，保持包内层次；已存在的文件跳过
    const std::atomic<bool>* cancel = nullptr;
};

struct ExtractResult {
    int status = 0;  // 包打不开等整体失败时非 0（error 为原因）；个别文件失败计入 failed
    std::string error;
    std::string out_dir;  // 解出的顶层文件夹
    uint64_t files = 0, ok = 0, skipped = 0, failed = 0, bytes = 0;
    std::vector<std::string> problems;  // 最多 200 条
    double seconds = 0;
};

ExtractResult extract(const std::string& package, const ExtractOptions& opt, const ProgressFn& progress = {});

// 校验包：每个条目核对 CRC-32；deep 时再还原并核对原文件 SHA-256。不写任何文件
ExtractResult verify(const std::string& package, bool deep, const ProgressFn& progress = {});

// 不覆盖用的新名字：path 不存在时原样返回，否则“名 (2).扩展名”“名 (3).扩展名”…（dir 为真时不拆扩展名）
std::string unique_path(const std::string& path, bool dir);

// 按扩展名判断是否为包（.iisz）
bool is_package_name(const std::string& name);

// ZIP 用的 CRC-32（IEEE 802.3，多项式 0xEDB88320）
uint32_t crc32(uint32_t crc, const void* data, size_t size);

}  // namespace iis::pkg
