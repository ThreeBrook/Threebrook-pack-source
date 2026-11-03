#include "io.hpp"

#ifdef _WIN32
#  define NOMINMAX
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <cerrno>
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

#include <algorithm>

namespace iis::io {

int MemorySink::write(const void* data, size_t size) {
    const auto p = static_cast<const uint8_t*>(data);
    buf_.insert(buf_.end(), p, p + size);
    pos_ += size;
    return IIS_OK;
}

int MemorySource::read_at(uint64_t offset, void* dst, size_t size) {
    if (offset > n_ || size > n_ - offset) return fail(IIS_E_CORRUPT, "读取越过数据末尾");
    std::memcpy(dst, p_ + offset, size);
    return IIS_OK;
}

#ifdef _WIN32

namespace {

std::wstring widen(const char* utf8) {
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, w.data(), n);
    w.resize(size_t(n - 1));
    return w;
}

int io_fail(const char* what, const char* path) {
    const DWORD err = GetLastError();
    return fail(win_status(err), std::string(what) + "（Windows 错误 " + std::to_string(err) + "）：" + path);
}

}  // namespace

std::wstring win_path(const char* utf8) {
    std::wstring w = widen(utf8);
    if (w.empty() || w.rfind(LR"(\\?\)", 0) == 0) return w;
    DWORD n = GetFullPathNameW(w.c_str(), 0, nullptr, nullptr);
    if (!n) return w;
    std::wstring full(n, L'\0');
    n = GetFullPathNameW(w.c_str(), n, full.data(), nullptr);
    full.resize(n);
    if (full.rfind(LR"(\\)", 0) == 0) return LR"(\\?\UNC\)" + full.substr(2);  // \\服务器\共享
    if (full.size() >= 2 && full[1] == L':') return LR"(\\?\)" + full;
    return full;
}

std::string narrow(const wchar_t* w, size_t n) {
    if (!n) return {};
    const int m = WideCharToMultiByte(CP_UTF8, 0, w, int(n), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(m), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, int(n), s.data(), m, nullptr, nullptr);
    return s;
}

int win_status(unsigned long err) {
    switch (err) {
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL: return IIS_E_NO_SPACE;
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
        case ERROR_USER_MAPPED_FILE: return IIS_E_BUSY;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND: return IIS_E_NOT_FOUND;
        default: return IIS_E_IO;
    }
}

namespace {

class FileSink final : public Sink {
public:
    FileSink(HANDLE h, std::wstring path, std::string path_utf8)
        : h_(h), path_(std::move(path)), path_utf8_(std::move(path_utf8)) {}
    ~FileSink() override { abort(); }

    int write(const void* data, size_t size) override {
        auto p = static_cast<const uint8_t*>(data);
        while (size) {
            const DWORD chunk = DWORD(std::min<size_t>(size, 1u << 30));
            DWORD done = 0;
            if (!WriteFile(h_, p, chunk, &done, nullptr) || done != chunk) return io_fail("写入失败", path_utf8_.c_str());
            p += chunk;
            size -= chunk;
            pos_ += chunk;
        }
        return IIS_OK;
    }

    int finish(bool sync) override {
        if (sync && !FlushFileBuffers(h_)) return io_fail("落盘失败", path_utf8_.c_str());
        CloseHandle(h_);
        h_ = INVALID_HANDLE_VALUE;
        return IIS_OK;
    }

    void abort() override {
        if (h_ == INVALID_HANDLE_VALUE) return;
        CloseHandle(h_);
        h_ = INVALID_HANDLE_VALUE;
        DeleteFileW(path_.c_str());  // 文件由本对象以 CREATE_NEW 创建，删除不会影响他人文件
    }

private:
    HANDLE h_;
    std::wstring path_;
    std::string path_utf8_;
};

class FileSource final : public Source {
public:
    FileSource(HANDLE h, uint64_t size) : h_(h), size_(size) {}
    ~FileSource() override { CloseHandle(h_); }
    uint64_t size() const override { return size_; }

    int read_at(uint64_t offset, void* dst, size_t size) override {
        if (offset > size_ || size > size_ - offset) return fail(IIS_E_CORRUPT, "读取越过文件末尾");
        auto p = static_cast<uint8_t*>(dst);
        while (size) {
            const DWORD chunk = DWORD(std::min<size_t>(size, 1u << 30));
            OVERLAPPED ov{};
            ov.Offset = DWORD(offset);
            ov.OffsetHigh = DWORD(offset >> 32);
            DWORD done = 0;
            if (!ReadFile(h_, p, chunk, &done, &ov) || done != chunk)
                return fail(IIS_E_IO, "读取失败（Windows 错误 " + std::to_string(GetLastError()) + "）");
            p += chunk;
            size -= chunk;
            offset += chunk;
        }
        return IIS_OK;
    }

private:
    HANDLE h_;
    uint64_t size_;
};

}  // namespace

int open_file_sink(const char* path_utf8, std::unique_ptr<Sink>& out) {
    if (!path_utf8) return fail(IIS_E_INVALID_ARG, "路径为空");
    const std::wstring w = win_path(path_utf8);
    if (w.empty()) return fail(IIS_E_INVALID_ARG, "路径不是合法 UTF-8");
    HANDLE h = CreateFileW(w.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_EXISTS) return fail(IIS_E_CONFLICT, std::string("目标文件已存在，不覆盖：") + path_utf8);
        return io_fail("无法创建文件", path_utf8);
    }
    out = std::make_unique<FileSink>(h, w, path_utf8);
    return IIS_OK;
}

int open_file_source(const char* path_utf8, std::unique_ptr<Source>& out) {
    if (!path_utf8) return fail(IIS_E_INVALID_ARG, "路径为空");
    const std::wstring w = win_path(path_utf8);
    if (w.empty()) return fail(IIS_E_INVALID_ARG, "路径不是合法 UTF-8");
    // 允许别人同时读、删除或重命名（不妨碍视觉软件与清理策略）
    HANDLE h = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return io_fail("无法打开文件", path_utf8);
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) {
        CloseHandle(h);
        return io_fail("无法取得文件大小", path_utf8);
    }
    out = std::make_unique<FileSource>(h, uint64_t(sz.QuadPart));
    return IIS_OK;
}

#else  // POSIX

int posix_status(int err) {
    switch (err) {
        case ENOSPC:
#ifdef EDQUOT
        case EDQUOT:
#endif
            return IIS_E_NO_SPACE;
        case ENOENT:
        case ENOTDIR: return IIS_E_NOT_FOUND;
        case EBUSY:
        case ETXTBSY: return IIS_E_BUSY;
        default: return IIS_E_IO;
    }
}

namespace {

int io_fail(const char* what, const std::string& path) {
    const int err = errno;
    return fail(posix_status(err), std::string(what) + "（errno " + std::to_string(err) + "）：" + path);
}

class FileSink final : public Sink {
public:
    FileSink(int fd, std::string path) : fd_(fd), path_(std::move(path)) {}
    ~FileSink() override { abort(); }

    int write(const void* data, size_t size) override {
        auto p = static_cast<const uint8_t*>(data);
        while (size) {
            const ssize_t n = ::write(fd_, p, std::min<size_t>(size, size_t(1) << 30));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return io_fail("写入失败", path_);
            p += n;
            size -= size_t(n);
            pos_ += uint64_t(n);
        }
        return IIS_OK;
    }

    int finish(bool sync) override {
        if (sync && ::fsync(fd_) != 0) return io_fail("落盘失败", path_);
        const int fd = fd_;
        fd_ = -1;
        if (::close(fd) != 0) {  // 网络文件系统可能到关闭时才报告写入错误
            const int st = io_fail("关闭文件失败", path_);
            ::unlink(path_.c_str());
            return st;
        }
        return IIS_OK;
    }

    void abort() override {
        if (fd_ < 0) return;
        ::close(fd_);
        fd_ = -1;
        ::unlink(path_.c_str());  // 由本对象以 O_EXCL 创建
    }

private:
    int fd_;
    std::string path_;
};

class FileSource final : public Source {
public:
    FileSource(int fd, uint64_t size) : fd_(fd), size_(size) {}
    ~FileSource() override { ::close(fd_); }
    uint64_t size() const override { return size_; }

    int read_at(uint64_t offset, void* dst, size_t size) override {
        if (offset > size_ || size > size_ - offset) return fail(IIS_E_CORRUPT, "读取越过文件末尾");
        auto p = static_cast<uint8_t*>(dst);
        while (size) {
            const ssize_t n = ::pread(fd_, p, std::min<size_t>(size, size_t(1) << 30), off_t(offset));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return fail(IIS_E_IO, "读取失败（errno " + std::to_string(errno) + "）");
            p += n;
            size -= size_t(n);
            offset += uint64_t(n);
        }
        return IIS_OK;
    }

private:
    int fd_;
    uint64_t size_;
};

}  // namespace

int open_file_sink(const char* path_utf8, std::unique_ptr<Sink>& out) {
    if (!path_utf8) return fail(IIS_E_INVALID_ARG, "路径为空");
    const int fd = ::open(path_utf8, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) {
        if (errno == EEXIST) return fail(IIS_E_CONFLICT, std::string("目标文件已存在，不覆盖：") + path_utf8);
        return io_fail("无法创建文件", path_utf8);
    }
    out = std::make_unique<FileSink>(fd, path_utf8);
    return IIS_OK;
}

int open_file_source(const char* path_utf8, std::unique_ptr<Source>& out) {
    if (!path_utf8) return fail(IIS_E_INVALID_ARG, "路径为空");
    const int fd = ::open(path_utf8, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return io_fail("无法打开文件", path_utf8);
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        return io_fail("无法取得文件大小", path_utf8);
    }
    out = std::make_unique<FileSource>(fd, uint64_t(st.st_size));
    return IIS_OK;
}

#endif

}  // namespace iis::io
