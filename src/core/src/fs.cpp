#include "fs.hpp"

#include "io.hpp"

#ifdef _WIN32
#  define NOMINMAX
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <cerrno>
#  include <cstdio>
#  include <dirent.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

#include <algorithm>
#include <cstring>

namespace iis::fs {

namespace {
constexpr size_t kChunk = 1u << 20;

#ifdef _WIN32
constexpr const char* kSeps = "\\/";
#else
constexpr const char* kSeps = "/";
#endif
}  // namespace

std::string join(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    if (std::strchr(kSeps, dir.back())) return dir + name;
    return dir + kSep + name;
}

std::string parent(const std::string& path) {
    const size_t i = path.find_last_of(kSeps);
    if (i == std::string::npos) return ".";
    if (i == 0) return path.substr(0, 1);
    return path.substr(0, i);
}

std::string base_name(const std::string& path) {
    const size_t i = path.find_last_of(kSeps);
    return i == std::string::npos ? path : path.substr(i + 1);
}

#ifdef _WIN32

namespace {

int win_fail(const char* what, const std::string& path, DWORD err) {
    return fail(io::win_status(err), std::string(what) + "（Windows 错误 " + std::to_string(err) + "）：" + path);
}

bool is_readonly(const std::wstring& w) {
    const DWORD a = GetFileAttributesW(w.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY);
}

struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~Handle() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
};

// 读满 size 字节（文件在读取中变短返回 false）
bool read_exact(HANDLE h, uint8_t* p, uint64_t size) {
    while (size) {
        const DWORD want = DWORD(std::min<uint64_t>(size, kChunk));
        DWORD got = 0;
        if (!ReadFile(h, p, want, &got, nullptr) || got != want) return false;
        p += got;
        size -= got;
    }
    return true;
}

}  // namespace

Kind kind(const std::string& path) {
    const std::wstring w = io::win_path(path.c_str());
    if (w.empty()) return Kind::Other;
    const DWORD a = GetFileAttributesW(w.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) {
        const DWORD e = GetLastError();
        return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? Kind::Missing : Kind::Other;
    }
    if (a & FILE_ATTRIBUTE_REPARSE_POINT) return Kind::Other;
    return a & FILE_ATTRIBUTE_DIRECTORY ? Kind::Dir : Kind::File;
}

int read_file(const std::string& path, std::vector<uint8_t>& out, uint64_t max_size) {
    const std::wstring w = io::win_path(path.c_str());
    if (w.empty()) return fail(IIS_E_INVALID_ARG, "路径不是合法 UTF-8");
    Handle f;
    f.h = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                      OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f.h == INVALID_HANDLE_VALUE) return win_fail("无法打开文件", path, GetLastError());
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f.h, &sz)) return win_fail("无法取得文件大小", path, GetLastError());
    if (uint64_t(sz.QuadPart) > max_size) return fail(IIS_E_UNSUPPORTED, "文件过大：" + path);
    out.resize(size_t(sz.QuadPart));
    if (!read_exact(f.h, out.data(), out.size())) {
        const DWORD e = GetLastError();
        return e ? win_fail("读取失败", path, e) : fail(IIS_E_BUSY, "文件在读取过程中变短：" + path);
    }
    return IIS_OK;
}

int rename_file(const std::string& from, const std::string& to, bool replace) {
    const std::wstring a = io::win_path(from.c_str()), b = io::win_path(to.c_str());
    if (a.empty() || b.empty()) return fail(IIS_E_INVALID_ARG, "路径不是合法 UTF-8");
    const DWORD flags = MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0);
    for (int attempt = 0;; ++attempt) {
        if (MoveFileExW(a.c_str(), b.c_str(), flags)) return IIS_OK;
        const DWORD e = GetLastError();
        if (e == ERROR_ALREADY_EXISTS || e == ERROR_FILE_EXISTS)
            return fail(IIS_E_CONFLICT, "目标文件已存在，不覆盖：" + to);
        // 杀毒软件、索引服务短暂打开新文件很常见：稍等重试
        if ((e == ERROR_SHARING_VIOLATION || e == ERROR_ACCESS_DENIED) && attempt < 5) {
            Sleep(DWORD(20 * (attempt + 1)));
            continue;
        }
        return win_fail("改名失败", from + " → " + to, e);
    }
}

int remove_file(const std::string& path) {
    const std::wstring w = io::win_path(path.c_str());
    if (w.empty()) return fail(IIS_E_INVALID_ARG, "路径不是合法 UTF-8");
    if (DeleteFileW(w.c_str())) return IIS_OK;
    return win_fail("删除失败", path, GetLastError());
}

int list_dir(const std::string& dir, std::vector<DirEntry>& out) {
    out.clear();
    std::wstring w = io::win_path(dir.c_str());
    if (w.empty()) return fail(IIS_E_INVALID_ARG, "路径不是合法 UTF-8");
    if (w.back() != L'\\') w += L'\\';
    w += L'*';
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(w.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND) return IIS_OK;
        return win_fail("无法读取目录", dir, e);
    }
    do {
        const std::wstring_view n(fd.cFileName);
        if (n == L"." || n == L"..") continue;
        DirEntry d;
        d.name = io::narrow(n.data(), n.size());
        d.dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        d.link = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        out.push_back(std::move(d));
    } while (FindNextFileW(h, &fd));
    const DWORD e = GetLastError();
    FindClose(h);
    if (e != ERROR_NO_MORE_FILES) return win_fail("读取目录中断", dir, e);
    return IIS_OK;
}

Release release_if_unchanged(const std::string& path, uint64_t size, const uint8_t sha[32], std::string& why) {
    const std::wstring w = io::win_path(path.c_str());
    if (w.empty()) {
        why = "路径不是合法 UTF-8";
        return Release::Failed;
    }
    // 只共享读：持有期间别人不能写入、改名或删除，复核与删除之间内容不会变
    Handle f;
    f.h = CreateFileW(w.c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                      FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (f.h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return Release::Gone;
        if (e == ERROR_ACCESS_DENIED && is_readonly(w)) {
            why = "原文件为只读，不删除";
            return Release::Failed;
        }
        why = "原文件被占用（Windows 错误 " + std::to_string(e) + "）";
        return e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION || e == ERROR_ACCESS_DENIED ? Release::Busy
                                                                                                      : Release::Failed;
    }
    if (int st = fault::at("release.locked")) {
        why = "注入故障（" + std::to_string(st) + "）";
        return Release::Failed;
    }
    FILE_STANDARD_INFO si{};
    if (!GetFileInformationByHandleEx(f.h, FileStandardInfo, &si, sizeof si)) {
        why = "无法取得文件信息（Windows 错误 " + std::to_string(GetLastError()) + "）";
        return Release::Failed;
    }
    if (si.Directory) {
        why = "原路径已变成目录";
        return Release::Changed;
    }
    if (uint64_t(si.EndOfFile.QuadPart) != size) {
        why = "原文件长度已变化";
        return Release::Changed;
    }
    iis_sha256_ctx ctx;
    iis_sha256_init(&ctx);
    std::vector<uint8_t> buf(kChunk);
    for (uint64_t left = size; left;) {
        const DWORD want = DWORD(std::min<uint64_t>(left, kChunk));
        DWORD got = 0;
        if (!ReadFile(f.h, buf.data(), want, &got, nullptr) || got != want) {
            why = "复核时读取失败（Windows 错误 " + std::to_string(GetLastError()) + "）";
            return Release::Failed;
        }
        iis_sha256_update(&ctx, buf.data(), got);
        left -= got;
    }
    uint8_t now[32];
    iis_sha256_final(&ctx, now);
    if (std::memcmp(now, sha, 32) != 0) {
        why = "原文件内容已变化";
        return Release::Changed;
    }
    if (int st = fault::at("release.hashed")) {
        why = "注入故障（" + std::to_string(st) + "）";
        return Release::Failed;
    }
    // 优先用 POSIX 语义删除（Windows 10 1709 起）：名字立即消失，别的程序随即可以新建同名文件；
    // 文件系统不支持时退回经典删除（关闭句柄时删除）
    struct {
        DWORD flags;
    } dix{0x1 | 0x2};  // FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS
    if (!SetFileInformationByHandle(f.h, FILE_INFO_BY_HANDLE_CLASS(21) /* FileDispositionInfoEx */, &dix,
                                    sizeof dix)) {
        FILE_DISPOSITION_INFO di{};
        di.DeleteFile = TRUE;
        if (!SetFileInformationByHandle(f.h, FileDispositionInfo, &di, sizeof di)) {
            const DWORD e = GetLastError();
            why = e == ERROR_ACCESS_DENIED && is_readonly(w) ? "原文件为只读，不删除"
                                                             : "删除失败（Windows 错误 " + std::to_string(e) + "）";
            return Release::Failed;
        }
    }
    return Release::Released;  // 句柄关闭时完成删除
}

#else  // POSIX

namespace {

int px_fail(const char* what, const std::string& path, int err) {
    return fail(io::posix_status(err), std::string(what) + "（errno " + std::to_string(err) + "）：" + path);
}

void sync_dir(const std::string& dir) {
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return;
    ::fsync(fd);  // 部分文件系统不支持目录 fsync（EINVAL），忽略
    ::close(fd);
}

struct Fd {
    int fd = -1;
    ~Fd() {
        if (fd >= 0) ::close(fd);
    }
};

bool read_exact(int fd, uint8_t* p, uint64_t size) {
    while (size) {
        const ssize_t n = ::read(fd, p, size_t(std::min<uint64_t>(size, kChunk)));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n;
        size -= uint64_t(n);
    }
    return true;
}

}  // namespace

Kind kind(const std::string& path) {
    struct stat st {};
    if (::lstat(path.c_str(), &st) != 0) return errno == ENOENT || errno == ENOTDIR ? Kind::Missing : Kind::Other;
    if (S_ISREG(st.st_mode)) return Kind::File;
    if (S_ISDIR(st.st_mode)) return Kind::Dir;
    return Kind::Other;
}

int read_file(const std::string& path, std::vector<uint8_t>& out, uint64_t max_size) {
    Fd f;
    f.fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (f.fd < 0) return px_fail("无法打开文件", path, errno);
    struct stat st {};
    if (::fstat(f.fd, &st) != 0) return px_fail("无法取得文件大小", path, errno);
    if (!S_ISREG(st.st_mode)) return fail(IIS_E_UNSUPPORTED, "不是普通文件：" + path);
    if (uint64_t(st.st_size) > max_size) return fail(IIS_E_UNSUPPORTED, "文件过大：" + path);
    out.resize(size_t(st.st_size));
    errno = 0;
    if (!read_exact(f.fd, out.data(), out.size()))
        return errno ? px_fail("读取失败", path, errno) : fail(IIS_E_BUSY, "文件在读取过程中变短：" + path);
    return IIS_OK;
}

int rename_file(const std::string& from, const std::string& to, bool replace) {
    if (replace) {
        if (::rename(from.c_str(), to.c_str()) != 0) return px_fail("改名失败", from + " → " + to, errno);
    } else if (::renameat2(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE) != 0) {
        if (errno == EEXIST) return fail(IIS_E_CONFLICT, "目标文件已存在，不覆盖：" + to);
        if (errno != EINVAL && errno != ENOSYS && errno != ENOTSUP)
            return px_fail("改名失败", from + " → " + to, errno);
        // 文件系统不支持 RENAME_NOREPLACE：用硬链接实现“不覆盖”
        if (::link(from.c_str(), to.c_str()) != 0) {
            if (errno == EEXIST) return fail(IIS_E_CONFLICT, "目标文件已存在，不覆盖：" + to);
            return px_fail("改名失败", from + " → " + to, errno);
        }
        ::unlink(from.c_str());
    }
    sync_dir(parent(to));
    return IIS_OK;
}

int remove_file(const std::string& path) {
    if (::unlink(path.c_str()) != 0) return px_fail("删除失败", path, errno);
    return IIS_OK;
}

int list_dir(const std::string& dir, std::vector<DirEntry>& out) {
    out.clear();
    DIR* d = ::opendir(dir.c_str());
    if (!d) return px_fail("无法读取目录", dir, errno);
    errno = 0;
    while (dirent* e = ::readdir(d)) {
        const std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        DirEntry de;
        de.name = n;
        struct stat st {};
        if (::lstat(join(dir, n).c_str(), &st) == 0) {
            de.dir = S_ISDIR(st.st_mode);
            de.link = S_ISLNK(st.st_mode);
        }
        out.push_back(std::move(de));
    }
    const int err = errno;
    ::closedir(d);
    if (err) return px_fail("读取目录中断", dir, err);
    return IIS_OK;
}

// POSIX 没有强制独占打开：复核与删除之间还有一个极短的窗口，在其中被改写的文件仍会被删除。
// 产品首发在 Windows；Linux 版（阶段 3）如需完全封闭，可改为复核后先改名到隔离名再复核一次。
Release release_if_unchanged(const std::string& path, uint64_t size, const uint8_t sha[32], std::string& why) {
    Fd f;
    f.fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (f.fd < 0) {
        if (errno == ENOENT) return Release::Gone;
        why = "无法打开原文件（errno " + std::to_string(errno) + "）";
        return errno == ELOOP ? Release::Changed : Release::Failed;
    }
    if (int st = fault::at("release.locked")) {
        why = "注入故障（" + std::to_string(st) + "）";
        return Release::Failed;
    }
    struct stat st {};
    if (::fstat(f.fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        why = "原路径已不是普通文件";
        return Release::Changed;
    }
    if (uint64_t(st.st_size) != size) {
        why = "原文件长度已变化";
        return Release::Changed;
    }
    iis_sha256_ctx ctx;
    iis_sha256_init(&ctx);
    std::vector<uint8_t> buf(kChunk);
    for (uint64_t left = size; left;) {
        const size_t want = size_t(std::min<uint64_t>(left, kChunk));
        if (!read_exact(f.fd, buf.data(), want)) {
            why = "复核时读取失败";
            return Release::Failed;
        }
        iis_sha256_update(&ctx, buf.data(), want);
        left -= want;
    }
    uint8_t now[32];
    iis_sha256_final(&ctx, now);
    if (std::memcmp(now, sha, 32) != 0) {
        why = "原文件内容已变化";
        return Release::Changed;
    }
    if (int e = fault::at("release.hashed")) {
        why = "注入故障（" + std::to_string(e) + "）";
        return Release::Failed;
    }
    struct stat cur {};
    if (::lstat(path.c_str(), &cur) != 0) return Release::Gone;
    if (cur.st_dev != st.st_dev || cur.st_ino != st.st_ino) {
        why = "原路径已换成别的文件";
        return Release::Changed;
    }
    if (::unlink(path.c_str()) != 0) {
        if (errno == ENOENT) return Release::Gone;
        why = "删除失败（errno " + std::to_string(errno) + "）";
        return Release::Failed;
    }
    sync_dir(parent(path));
    return Release::Released;
}

#endif

}  // namespace iis::fs
