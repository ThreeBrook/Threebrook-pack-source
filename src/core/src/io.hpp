// 容器读写的字节源 / 字节汇：内存或文件。路径一律 UTF-8，Windows 下转宽字符调用 W 版 API。
#pragma once

#include "internal.hpp"

#include <memory>
#include <string>
#include <vector>

namespace iis::io {

class Sink {
public:
    virtual ~Sink() = default;
    virtual int write(const void* data, size_t size) = 0;  // 顺序追加
    virtual int finish(bool sync) = 0;                     // sync：落盘（FlushFileBuffers / fsync）
    virtual void abort() {}                                // 未完成时放弃：文件汇删除自己创建的不完整文件
    uint64_t position() const { return pos_; }

protected:
    uint64_t pos_ = 0;
};

class Source {
public:
    virtual ~Source() = default;
    virtual uint64_t size() const = 0;
    virtual int read_at(uint64_t offset, void* dst, size_t size) = 0;  // 越界或读不满返回 IIS_E_CORRUPT
};

class MemorySink final : public Sink {
public:
    int write(const void* data, size_t size) override;
    int finish(bool) override { return IIS_OK; }
    std::vector<uint8_t>& bytes() { return buf_; }

private:
    std::vector<uint8_t> buf_;
};

// 引用调用方的内存，不复制；调用方须保证读取期间内存有效
class MemorySource final : public Source {
public:
    MemorySource(const void* data, size_t size) : p_(static_cast<const uint8_t*>(data)), n_(size) {}
    uint64_t size() const override { return n_; }
    int read_at(uint64_t offset, void* dst, size_t size) override;

private:
    const uint8_t* p_;
    size_t n_;
};

#ifdef _WIN32
// UTF-8 路径 → 宽字符完整路径，加长路径前缀 \\?\ 以支持超过 260 字符的路径；非法 UTF-8 返回空串
std::wstring win_path(const char* path_utf8);
std::string narrow(const wchar_t* w, size_t n);
// Windows 错误码 → 状态码（磁盘满 → IIS_E_NO_SPACE，占用 → IIS_E_BUSY，不存在 → IIS_E_NOT_FOUND）
int win_status(unsigned long err);
#else
int posix_status(int err);
#endif

// 新建文件（已存在则失败，绝不覆盖）
int open_file_sink(const char* path_utf8, std::unique_ptr<Sink>& out);
int open_file_source(const char* path_utf8, std::unique_ptr<Source>& out);

}  // namespace iis::io
