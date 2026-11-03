#include "package.hpp"

#include "fs.hpp"
#include "internal.hpp"
#include "io.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <thread>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/stat.h>
#endif

namespace iis::pkg {

namespace stdfs = std::filesystem;

namespace {

constexpr uint32_t kLocalSig = 0x04034b50, kCentralSig = 0x02014b50, kEndSig = 0x06054b50;
constexpr uint32_t kEnd64Sig = 0x06064b50, kLoc64Sig = 0x07064b50;
constexpr uint16_t kExtraZip64 = 0x0001, kExtraIis = 0x4949;  // 0x4949 = "II"：IIS 附加字段
constexpr uint16_t kFlagUtf8 = 0x0800;
constexpr uint16_t kMadeBy = 63;  // ZIP 规范 6.3，主机 0（MS-DOS 属性）
constexpr uint32_t kMax32 = 0xFFFFFFFFu;
constexpr uint16_t kMax16 = 0xFFFFu;
constexpr uint16_t kIisExtraLen = 50;  // 版本 1 + 标志 1 + 原长 8 + SHA-256 32 + 修改时间 8
constexpr int64_t kUnixEpochFt = 116444736000000000LL;  // 1970-01-01 的 FILETIME
constexpr uint64_t kPackBudget = 512ull << 20;          // 打包时同时在内存中的原文件字节上限（单个大文件除外）
constexpr size_t kMaxProblems = 200;

// ---------------- 小工具 ----------------

struct Bytes {
    std::vector<uint8_t> v;
    void u8(uint8_t x) { v.push_back(x); }
    void u16(uint32_t x) { u8(uint8_t(x)), u8(uint8_t(x >> 8)); }
    void u32(uint32_t x) { u16(x & 0xFFFF), u16(x >> 16); }
    void u64(uint64_t x) { u32(uint32_t(x)), u32(uint32_t(x >> 32)); }
    void raw(const void* p, size_t n) { v.insert(v.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n); }
    void raw(const std::string& s) { raw(s.data(), s.size()); }
};

uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t rd32(const uint8_t* p) { return uint32_t(rd16(p)) | uint32_t(rd16(p + 2)) << 16; }
uint64_t rd64(const uint8_t* p) { return uint64_t(rd32(p)) | uint64_t(rd32(p + 4)) << 32; }

bool ends_with_ci(const std::string& s, const char* e) {
    const size_t n = std::strlen(e);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower(uint8_t(s[s.size() - n + i])) != std::tolower(uint8_t(e[i]))) return false;
    return true;
}

stdfs::path P(const std::string& u8) { return stdfs::u8path(u8); }

std::string U(const stdfs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string native(std::string rel) {
#ifdef _WIN32
    std::replace(rel.begin(), rel.end(), '/', '\\');
#endif
    return rel;
}

std::string tmp_name(const std::string& final_path) {
    thread_local std::mt19937 rng(std::random_device{}() ^
                                  uint32_t(std::chrono::steady_clock::now().time_since_epoch().count()));
    char b[16];
    std::snprintf(b, sizeof b, "%08x", unsigned(rng()));
    return final_path + "." + b + ".iistmp";
}

int default_threads(int want) {
    if (want > 0) return want;
    const unsigned h = std::thread::hardware_concurrency();
    return int(std::clamp(h ? h : 2u, 1u, 8u));
}

void parallel_for(size_t n, int threads, const std::function<void(size_t)>& fn) {
    const size_t t = std::min<size_t>(size_t(default_threads(threads)), std::max<size_t>(n, 1));
    std::atomic<size_t> next{0};
    std::vector<std::thread> ts;
    for (size_t k = 0; k < t; ++k)
        ts.emplace_back([&] {
            for (size_t i; (i = next++) < n;) fn(i);
        });
    for (auto& th : ts) th.join();
}

template <class R>
void problem(R& r, const std::string& rel, const std::string& why) {
    if (r.problems.size() < kMaxProblems) r.problems.push_back(rel + "：" + why);
}

bool cancelled(const std::atomic<bool>* c) { return c && c->load(); }

// ---------------- 修改时间 ----------------

bool stat_path(const std::string& path, uint64_t& size, int64_t& mtime) {
#ifdef _WIN32
    const std::wstring w = io::win_path(path.c_str());
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (w.empty() || !GetFileAttributesExW(w.c_str(), GetFileExInfoStandard, &d)) return false;
    size = uint64_t(d.nFileSizeHigh) << 32 | d.nFileSizeLow;
    mtime = int64_t(uint64_t(d.ftLastWriteTime.dwHighDateTime) << 32 | d.ftLastWriteTime.dwLowDateTime);
#else
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) return false;
    size = uint64_t(st.st_size);
#  ifdef __APPLE__
    mtime = int64_t(st.st_mtimespec.tv_sec) * 10000000 + st.st_mtimespec.tv_nsec / 100 + kUnixEpochFt;
#  else
    mtime = int64_t(st.st_mtim.tv_sec) * 10000000 + st.st_mtim.tv_nsec / 100 + kUnixEpochFt;
#  endif
#endif
    return true;
}

void set_mtime(const std::string& path, int64_t ft) {
    if (ft <= 0) return;
#ifdef _WIN32
    const std::wstring w = io::win_path(path.c_str());
    HANDLE h = CreateFileW(w.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME f{DWORD(uint64_t(ft)), DWORD(uint64_t(ft) >> 32)};
    SetFileTime(h, nullptr, nullptr, &f);
    CloseHandle(h);
#else
    const int64_t u = ft - kUnixEpochFt;
    int64_t sec = u / 10000000, rem = u % 10000000;
    if (rem < 0) rem += 10000000, --sec;
    struct timespec ts[2];
    ts[0].tv_sec = 0;
    ts[0].tv_nsec = UTIME_OMIT;
    ts[1].tv_sec = time_t(sec);
    ts[1].tv_nsec = long(rem * 100);
    ::utimensat(AT_FDCWD, path.c_str(), ts, 0);
#endif
}

// ZIP 头里的 MS-DOS 日期时间（本地时间，2 秒精度，1980~2107 年）
void dos_time(int64_t ft, uint16_t& time, uint16_t& date) {
    const std::time_t t = ft > kUnixEpochFt ? std::time_t((ft - kUnixEpochFt) / 10000000) : std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    if (tm.tm_year < 80) {
        time = 0;
        date = (1 << 5) | 1;  // 1980-01-01
        return;
    }
    const int year = std::min(tm.tm_year - 80, 127);
    date = uint16_t(year << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday);
    time = uint16_t(tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2);
}

// ---------------- 写 ZIP ----------------

class ZipWriter {
public:
    ZipWriter(io::Sink& sink, bool force64) : sink_(sink), force64_(force64) { iis_sha256_init(&sha_); }

    int add(const std::string& name, const uint8_t* data, uint64_t size, uint32_t crc, int64_t mtime,
            const std::vector<uint8_t>& extra, bool dir) {
        if (name.size() >= kMax16) return fail(IIS_E_INVALID_ARG, "路径过长：" + name);
        Rec r{name, crc, size, pos_, 0, 0, extra, dir};
        dos_time(mtime, r.time, r.date);
        const bool z64 = force64_ || size >= kMax32;
        Bytes h;
        h.u32(kLocalSig);
        h.u16(z64 ? 45 : 20);
        h.u16(kFlagUtf8);
        h.u16(0);  // 只存不压
        h.u16(r.time);
        h.u16(r.date);
        h.u32(crc);
        h.u32(z64 ? kMax32 : uint32_t(size));
        h.u32(z64 ? kMax32 : uint32_t(size));
        h.u16(uint32_t(name.size()));
        h.u16(z64 ? 20 : 0);
        h.raw(name);
        if (z64) {
            h.u16(kExtraZip64);
            h.u16(16);
            h.u64(size);
            h.u64(size);
        }
        if (int st = put(h.v.data(), h.v.size())) return st;
        if (size)
            if (int st = put(data, size_t(size))) return st;
        recs_.push_back(std::move(r));
        return IIS_OK;
    }

    int finish() {
        const uint64_t cd_off = pos_;
        for (const Rec& r : recs_) {
            const bool zs = force64_ || r.size >= kMax32, zo = force64_ || r.offset >= kMax32;
            Bytes x;
            if (zs || zo) {
                x.u16(kExtraZip64);
                x.u16((zs ? 16 : 0) + (zo ? 8 : 0));
                if (zs) x.u64(r.size), x.u64(r.size);
                if (zo) x.u64(r.offset);
            }
            x.raw(r.extra.data(), r.extra.size());
            Bytes c;
            c.u32(kCentralSig);
            c.u16(kMadeBy);
            c.u16(zs || zo ? 45 : 20);
            c.u16(kFlagUtf8);
            c.u16(0);
            c.u16(r.time);
            c.u16(r.date);
            c.u32(r.crc);
            c.u32(zs ? kMax32 : uint32_t(r.size));
            c.u32(zs ? kMax32 : uint32_t(r.size));
            c.u16(uint32_t(r.name.size()));
            c.u16(uint32_t(x.v.size()));
            c.u16(0);                    // 注释
            c.u16(0);                    // 起始盘
            c.u16(0);                    // 内部属性
            c.u32(r.dir ? 0x10 : 0x20);  // MS-DOS 属性：目录 / 存档
            c.u32(zo ? kMax32 : uint32_t(r.offset));
            c.raw(r.name);
            c.raw(x.v.data(), x.v.size());
            if (int st = put(c.v.data(), c.v.size())) return st;
        }
        const uint64_t cd_size = pos_ - cd_off, n = recs_.size();
        const bool z64 = force64_ || n >= kMax16 || cd_size >= kMax32 || cd_off >= kMax32;
        Bytes e;
        if (z64) {
            const uint64_t at = pos_;
            e.u32(kEnd64Sig);
            e.u64(44);
            e.u16(kMadeBy);
            e.u16(45);
            e.u32(0);
            e.u32(0);
            e.u64(n);
            e.u64(n);
            e.u64(cd_size);
            e.u64(cd_off);
            e.u32(kLoc64Sig);
            e.u32(0);
            e.u64(at);
            e.u32(1);
        }
        e.u32(kEndSig);
        e.u16(0);
        e.u16(0);
        e.u16(z64 ? kMax16 : uint32_t(n));
        e.u16(z64 ? kMax16 : uint32_t(n));
        e.u32(z64 ? kMax32 : uint32_t(cd_size));
        e.u32(z64 ? kMax32 : uint32_t(cd_off));
        e.u16(0);
        return put(e.v.data(), e.v.size());
    }

    uint64_t size() const { return pos_; }
    size_t entries() const { return recs_.size(); }
    void digest(uint8_t out[32]) { iis_sha256_final(&sha_, out); }

private:
    struct Rec {
        std::string name;
        uint32_t crc;
        uint64_t size, offset;
        uint16_t time, date;
        std::vector<uint8_t> extra;
        bool dir;
    };

    int put(const void* p, size_t n) {
        iis_sha256_update(&sha_, p, n);
        pos_ += n;
        return sink_.write(p, n);
    }

    io::Sink& sink_;
    bool force64_;
    uint64_t pos_ = 0;
    iis_sha256_ctx sha_;
    std::vector<Rec> recs_;
};

std::vector<uint8_t> iis_extra(bool container, uint64_t orig_size, const uint8_t sha[32], int64_t mtime) {
    Bytes b;
    b.u16(kExtraIis);
    b.u16(kIisExtraLen);
    b.u8(1);  // 版本
    b.u8(container ? 1 : 0);
    b.u64(orig_size);
    b.raw(sha, 32);
    b.u64(uint64_t(mtime));
    return b.v;
}

// 包内名称 → 解出的相对路径；不安全的名称（.. 、盘符、Windows 不允许的字符等）给出原因
void sanitize(Entry& e) {
    std::string s = e.name;
    if (e.dir) s.pop_back();
    if (s.empty()) {
        e.bad = "名称为空";
        return;
    }
    size_t a = 0;
    std::vector<std::string> comps;
    for (;;) {
        const size_t b = s.find('/', a);
        comps.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    for (const auto& c : comps) {
        if (c.empty() || c == "." || c == "..") {
            e.bad = "名称含空的、“.”或“..”的路径段";
            return;
        }
        for (const char ch : c)
            if (uint8_t(ch) < 0x20 || std::strchr("\\:*?\"<>|", ch)) {
                e.bad = "名称含不允许的字符";
                return;
            }
        if (c.back() == ' ' || c.back() == '.') {
            e.bad = "名称以空格或“.”结尾";
            return;
        }
    }
    std::string& last = comps.back();
    if (e.iis && !e.dir && last.size() > 4 && ends_with_ci(last, ".iis")) last.resize(last.size() - 4);
    for (size_t i = 0; i < comps.size(); ++i) e.path += (i ? "/" : "") + comps[i];
}

// 写新文件：临时文件 → 落盘 → 改名（不覆盖）。失败不留下文件
int write_new(const std::string& target, const std::vector<uint8_t>& data, std::string& why) {
    const std::string tmp = tmp_name(target);
    std::unique_ptr<io::Sink> s;
    int st = io::open_file_sink(tmp.c_str(), s);
    if (st == IIS_OK && !data.empty()) st = s->write(data.data(), data.size());
    if (st == IIS_OK) st = s->finish(true);
    if (st != IIS_OK) {
        why = iis_last_error();
        if (s) s->abort();
        return st;
    }
    s.reset();
    if ((st = fs::rename_file(tmp, target, false)) != IIS_OK) {
        why = iis_last_error();
        fs::remove_file(tmp);
    }
    return st;
}

// ---------------- 打包 ----------------

struct Item {
    std::string rel, abs;
    uint64_t size = 0;
    int64_t mtime = 0;
};

struct Dir {
    std::string rel, abs;  // rel 以 '/' 结尾
    int64_t mtime = 0;
};

void walk(const std::string& dir, const std::string& rel, std::vector<Dir>& dirs, std::vector<Item>& files,
          PackResult& r) {
    std::vector<fs::DirEntry> list;
    if (fs::list_dir(dir, list) != IIS_OK) {
        problem(r, rel, std::string("无法读取文件夹：") + iis_last_error());
        return;
    }
    for (const auto& e : list) {
        if (e.name == "." || e.name == "..") continue;
        const std::string abs = fs::join(dir, e.name), r2 = rel + e.name;
        if (e.link) {
            problem(r, r2, "符号链接或目录联接，未打包");
            continue;
        }
        if (e.dir) {
            Dir d{r2 + "/", abs, 0};
            uint64_t ignore = 0;
            stat_path(abs, ignore, d.mtime);
            dirs.push_back(d);
            walk(abs, r2 + "/", dirs, files, r);
            continue;
        }
        if (ends_with_ci(e.name, ".iistmp")) continue;  // IIS 自己的临时文件
        Item it{r2, abs, 0, 0};
        if (!stat_path(abs, it.size, it.mtime)) {
            ++r.unreadable;
            problem(r, r2, "无法读取文件信息，未打包");
            continue;
        }
        files.push_back(std::move(it));
    }
}

struct Prep {
    bool done = false, ok = false, iis = false;
    int method = IIS_METHOD_STORE;
    std::string why;
    std::vector<uint8_t> data;
    uint64_t orig = 0;
    uint8_t sha[32] = {};
    uint32_t crc = 0;
};

// 同名参数文件：a.raw → a.json（与单文件压缩一致）
std::string sidecar_of(const std::string& abs) {
    const std::string name = fs::base_name(abs);
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) return {};
    const std::string sp = fs::join(fs::parent(abs), name.substr(0, dot) + ".json");
    if (sp == abs || fs::kind(sp) != fs::Kind::File) return {};
    std::vector<uint8_t> s;
    if (fs::read_file(sp, s, 1 << 20) != IIS_OK) return {};
    return std::string(s.begin(), s.end());
}

void prepare(const Item& it, const PackOptions& opt, Prep& p) {
    try {
        std::vector<uint8_t> d;
        if (fs::read_file(it.abs, d) != IIS_OK) {
            p.why = iis_last_error();
            return;
        }
        p.orig = d.size();
        iis_sha256(d.data(), d.size(), p.sha);
        // 已经是 IIS 容器或包的文件原样存放，不再套一层
        const bool already = (d.size() >= 8 && std::memcmp(d.data(), "IISCONT\0", 8) == 0) || is_package_name(it.rel);
        if (!already) {
            iis_compress_options co{};
            co.fallback_zstd_level = opt.zstd_level;
            co.min_saving = opt.min_saving;
            co.rel_path_utf8 = it.rel.c_str();
            co.raw_auto = 1;  // 无文件头的 RAW 自动识别参数
            const std::string side = sidecar_of(it.abs);
            if (!side.empty()) co.raw_sidecar_json = side.c_str();
            iis_buffer c{};
            iis_compress_result cr{};
            // 成功返回的容器已自检（独立读回、解码、重组、SHA-256 一致）；任何失败都退回原样存放
            if (iis_compress(d.data(), d.size(), &co, &c, &cr) == IIS_OK && cr.method != IIS_METHOD_STORE && c.size) {
                p.data.assign(c.data, c.data + c.size);
                p.iis = true;
                p.method = cr.method;
            }
            iis_buffer_free(&c);
        }
        if (!p.iis) p.data = std::move(d);
        p.crc = crc32(0, p.data.data(), p.data.size());
        p.ok = true;
    } catch (const std::bad_alloc&) {
        p.data.clear();
        p.why = "内存不足";
    } catch (const std::exception& e) {
        p.data.clear();
        p.why = std::string("内部错误：") + e.what();
    }
}

}  // namespace

uint32_t crc32(uint32_t crc, const void* data, size_t size) {
    static const auto T = [] {
        std::vector<std::array<uint32_t, 256>> t(8);
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[0][i] = c;
        }
        for (int k = 1; k < 8; ++k)
            for (uint32_t i = 0; i < 256; ++i) t[size_t(k)][i] = (t[size_t(k) - 1][i] >> 8) ^ t[0][t[size_t(k) - 1][i] & 255];
        return t;
    }();
    const uint8_t* p = static_cast<const uint8_t*>(data);
    crc = ~crc;
    while (size >= 8) {  // 一次 8 字节查表
        const uint32_t a = crc ^ rd32(p), b = rd32(p + 4);
        crc = T[7][a & 255] ^ T[6][(a >> 8) & 255] ^ T[5][(a >> 16) & 255] ^ T[4][a >> 24] ^ T[3][b & 255] ^
              T[2][(b >> 8) & 255] ^ T[1][(b >> 16) & 255] ^ T[0][b >> 24];
        p += 8;
        size -= 8;
    }
    while (size--) crc = T[0][(crc ^ *p++) & 255] ^ (crc >> 8);
    return ~crc;
}

bool is_package_name(const std::string& name) { return ends_with_ci(name, ".iisz"); }

std::string unique_path(const std::string& path, bool dir) {
    std::error_code ec;
    const stdfs::path p = P(path);
    if (!stdfs::exists(p, ec) && !ec) return path;
    const std::string name = U(p.filename());
    std::string stem = name, ext;
    if (!dir) {
        const size_t k = name.rfind('.');
        if (k != std::string::npos && k > 0) stem = name.substr(0, k), ext = name.substr(k);
    }
    for (int n = 2; n < 100000; ++n) {
        const stdfs::path c = p.parent_path() / P(stem + " (" + std::to_string(n) + ")" + ext);
        if (!stdfs::exists(c, ec) && !ec) return U(c);
    }
    return path;  // 实在找不到：调用方会因目标已存在而失败（不覆盖）
}

PackResult pack_folder(const std::string& folder, const std::string& out_arg, const PackOptions& opt,
                       const ProgressFn& progress) {
    PackResult r;
    const auto t0 = std::chrono::steady_clock::now();
    auto done = [&](int st, std::string why) {
        r.status = st;
        r.error = std::move(why);
        r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        return r;
    };

    std::error_code ec;
    stdfs::path rootp = stdfs::absolute(P(folder), ec).lexically_normal();
    if (!rootp.has_filename()) rootp = rootp.parent_path();
    const std::string name = U(rootp.filename());
    if (name.empty() || rootp == rootp.root_path()) return done(IIS_E_INVALID_ARG, "不能把整个磁盘打成一个包，请选择文件夹");
    const std::string root = U(rootp);
    if (fs::kind(root) != fs::Kind::Dir) return done(IIS_E_NOT_FOUND, "不是文件夹：" + root);
    const std::string parent = U(rootp.parent_path());

    std::string out = out_arg;
    if (out.empty()) {
        out = unique_path(fs::join(parent, name + ".iisz"), false);
    } else if (fs::kind(out) != fs::Kind::Missing) {
        return done(IIS_E_CONFLICT, "目标已存在，不覆盖：" + out);
    }

    std::vector<Dir> dirs{{name + "/", root, 0}};
    uint64_t ignore = 0;
    stat_path(root, ignore, dirs[0].mtime);
    std::vector<Item> files;
    walk(root, name + "/", dirs, files, r);
    std::sort(dirs.begin(), dirs.end(), [](const Dir& a, const Dir& b) { return a.rel < b.rel; });
    std::sort(files.begin(), files.end(), [](const Item& a, const Item& b) { return a.rel < b.rel; });

    Progress pg;
    pg.files_total = files.size();
    for (const auto& f : files) pg.bytes_total += f.size;
    if (progress) progress(pg);

    const std::string tmp = tmp_name(out);
    std::unique_ptr<io::Sink> sink;
    if (int st = io::open_file_sink(tmp.c_str(), sink)) return done(st, std::string("无法创建包：") + iis_last_error());
    ZipWriter zw(*sink, opt.force_zip64);
    for (const auto& d : dirs) {
        if (int st = zw.add(d.rel, nullptr, 0, 0, d.mtime, {}, true)) {
            sink->abort();
            return done(st, std::string("写包失败：") + iis_last_error());
        }
    }
    r.dirs = dirs.size();

    // 多线程准备（读、压缩、自检），按顺序写入；同时在内存中的原文件字节不超过 kPackBudget
    std::vector<Prep> preps(files.size());
    std::mutex mu;
    std::condition_variable cv;
    size_t next = 0;
    uint64_t inflight = 0;
    bool stop = false;
    auto worker = [&] {
        for (;;) {
            size_t i;
            {
                std::unique_lock<std::mutex> lk(mu);
                cv.wait(lk, [&] {
                    return stop || next >= files.size() || inflight == 0 || inflight + files[next].size <= kPackBudget;
                });
                if (stop || next >= files.size()) return;
                i = next++;
                inflight += files[i].size;
            }
            Prep p;
            prepare(files[i], opt, p);
            {
                std::lock_guard<std::mutex> lk(mu);
                preps[i] = std::move(p);
                preps[i].done = true;
            }
            cv.notify_all();
        }
    };
    std::vector<std::thread> pool;
    const int nt = std::min<int>(default_threads(opt.threads), int(std::max<size_t>(files.size(), 1)));
    for (int k = 0; k < nt; ++k) pool.emplace_back(worker);
    auto halt = [&] {
        {
            std::lock_guard<std::mutex> lk(mu);
            stop = true;
        }
        cv.notify_all();
        for (auto& t : pool) t.join();
        pool.clear();
    };

    struct Packed {
        size_t index;
        uint64_t size;
        uint8_t sha[32];
    };
    std::vector<Packed> packed;
    for (size_t i = 0; i < files.size(); ++i) {
        if (cancelled(opt.cancel)) {
            halt();
            sink->abort();
            return done(kCancelled, "已取消，没有生成包");
        }
        {
            std::unique_lock<std::mutex> lk(mu);
            cv.wait(lk, [&] { return preps[i].done; });
        }
        Prep& p = preps[i];
        if (!p.ok) {
            ++r.unreadable;
            problem(r, files[i].rel, "无法读取，未打包：" + p.why);
        } else {
            const std::string ename = p.iis ? files[i].rel + ".iis" : files[i].rel;
            if (int st = zw.add(ename, p.data.data(), p.data.size(), p.crc, files[i].mtime,
                                iis_extra(p.iis, p.orig, p.sha, files[i].mtime), false)) {
                const std::string why = iis_last_error();
                halt();
                sink->abort();
                return done(st, "写包失败：" + why);
            }
            ++r.files;
            r.orig_bytes += p.orig;
            (p.method == IIS_METHOD_IMAGE ? r.as_image : p.method == IIS_METHOD_ZSTD ? r.as_zstd : r.as_stored)++;
            Packed k{i, p.orig, {}};
            std::memcpy(k.sha, p.sha, 32);
            packed.push_back(k);
        }
        std::vector<uint8_t>().swap(p.data);
        {
            std::lock_guard<std::mutex> lk(mu);
            inflight -= files[i].size;
        }
        cv.notify_all();
        ++pg.files_done;
        pg.bytes_done += files[i].size;
        pg.current = files[i].rel;
        if (progress) progress(pg);
    }
    halt();

    if (int st = zw.finish()) {
        const std::string why = iis_last_error();
        sink->abort();
        return done(st, "写包失败：" + why);
    }
    if (int st = sink->finish(true)) {
        const std::string why = iis_last_error();
        sink->abort();
        return done(st, "写包失败：" + why);
    }
    sink.reset();
    uint8_t want[32];
    zw.digest(want);

    // 读回核对：整个包的 SHA-256 与写入的一致，且能按 ZIP 结构读出全部条目
    {
        std::string why;
        std::unique_ptr<io::Source> src;
        int st = io::open_file_source(tmp.c_str(), src);
        if (st == IIS_OK && src->size() != zw.size()) st = IIS_E_IO, why = "长度不符";
        if (st == IIS_OK) {
            iis_sha256_ctx c;
            iis_sha256_init(&c);
            std::vector<uint8_t> buf(8u << 20);
            for (uint64_t at = 0; st == IIS_OK && at < src->size(); at += buf.size()) {
                const size_t n = size_t(std::min<uint64_t>(buf.size(), src->size() - at));
                st = src->read_at(at, buf.data(), n);
                iis_sha256_update(&c, buf.data(), n);
            }
            uint8_t got[32];
            iis_sha256_final(&c, got);
            if (st == IIS_OK && std::memcmp(got, want, 32) != 0) st = IIS_E_IO, why = "SHA-256 不符";
        } else if (why.empty()) {
            why = iis_last_error();
        }
        src.reset();
        if (st == IIS_OK) {
            Reader chk;
            if ((st = chk.open(tmp)) != IIS_OK) why = chk.error();
            else if (chk.entries().size() != zw.entries()) st = IIS_E_IO, why = "条目数不符";
        }
        if (st != IIS_OK) {
            fs::remove_file(tmp);
            return done(st, "包写入后核对不一致（" + why + "），没有生成包；原文件未动");
        }
    }
    if (int st = fs::rename_file(tmp, out, false)) {
        const std::string why = iis_last_error();
        fs::remove_file(tmp);
        return done(st, "无法完成包：" + why);
    }
    r.package = out;
    r.package_bytes = zw.size();

    if (opt.delete_originals) {
        // 包已落盘并核对。逐个独占打开原文件，长度与 SHA-256 都与打包时一致才删除
        for (const auto& k : packed) {
            std::string why;
            switch (fs::release_if_unchanged(files[k.index].abs, k.size, k.sha, why)) {
                case fs::Release::Released: ++r.released; break;
                case fs::Release::Gone: break;
                case fs::Release::Busy:
                    ++r.not_released, problem(r, files[k.index].rel, "被占用，未删除原文件");
                    break;
                case fs::Release::Changed:
                    ++r.not_released, problem(r, files[k.index].rel, "打包后被修改，未删除原文件");
                    break;
                case fs::Release::Failed:
                    ++r.not_released, problem(r, files[k.index].rel, "未删除原文件：" + why);
                    break;
            }
        }
        // 变空的文件夹由深到浅删除（非空的删不掉，保留）
        std::vector<Dir> rev = dirs;
        std::sort(rev.begin(), rev.end(), [](const Dir& a, const Dir& b) {
            return std::count(a.rel.begin(), a.rel.end(), '/') > std::count(b.rel.begin(), b.rel.end(), '/');
        });
        for (const auto& d : rev) stdfs::remove(P(d.abs), ec);
    }
    return done(IIS_OK, "");
}

// ---------------- 读 ZIP ----------------

struct Reader::Impl {
    std::unique_ptr<io::Source> src;
    mutable std::mutex mu;
    uint64_t cd_off = 0;
};

Reader::Reader() : impl_(new Impl) {}
Reader::~Reader() { delete impl_; }

int Reader::open(const std::string& path) {
    std::unique_ptr<io::Source> src;
    if (int st = io::open_file_source(path.c_str(), src)) {
        path_ = path;
        entries_.clear();
        error_ = iis_last_error();
        return st;
    }
    return open(std::move(src), path);
}

int Reader::open(std::unique_ptr<io::Source> source, const std::string& name) {
    path_ = name;
    entries_.clear();
    error_.clear();
    auto bad = [&](const std::string& why) {
        error_ = why;
        return IIS_E_CORRUPT;
    };
    if (!source) return bad("没有数据");
    impl_->src = std::move(source);
    io::Source& src = *impl_->src;
    const uint64_t size = src.size();
    if (size < 22) return bad("不是 .iisz 包（文件太短）");
    const size_t tail = size_t(std::min<uint64_t>(size, 22 + 65535));
    std::vector<uint8_t> t(tail);
    if (src.read_at(size - tail, t.data(), tail)) return bad(iis_last_error());
    size_t at = std::string::npos;
    for (size_t i = tail - 22 + 1; i-- > 0;)
        if (rd32(&t[i]) == kEndSig && i + 22 + rd16(&t[i + 20]) <= tail) {
            at = i;
            break;
        }
    if (at == std::string::npos) return bad("不是 .iisz 包（找不到 ZIP 目录结尾）");
    const uint64_t eocd = size - tail + at;
    uint64_t n = rd16(&t[at + 10]), cd_size = rd32(&t[at + 12]), cd_off = rd32(&t[at + 16]);
    if (eocd >= 20) {
        uint8_t loc[20];
        if (src.read_at(eocd - 20, loc, 20) == IIS_OK && rd32(loc) == kLoc64Sig) {
            const uint64_t z = rd64(loc + 8);
            uint8_t e64[56];
            if (z > eocd || src.read_at(z, e64, 56) || rd32(e64) != kEnd64Sig) return bad("ZIP64 目录结尾损坏");
            n = rd64(e64 + 32);
            cd_size = rd64(e64 + 40);
            cd_off = rd64(e64 + 48);
        }
    }
    if (cd_off > eocd || cd_size > eocd - cd_off) return bad("ZIP 中央目录位置不对（文件不完整或已损坏）");
    if (cd_size > (1ull << 31)) return bad("ZIP 中央目录过大");
    std::vector<uint8_t> cd(static_cast<size_t>(cd_size));
    if (cd_size && src.read_at(cd_off, cd.data(), cd.size())) return bad(iis_last_error());
    size_t p = 0;
    for (uint64_t k = 0; k < n; ++k) {
        if (p + 46 > cd.size() || rd32(&cd[p]) != kCentralSig) return bad("ZIP 中央目录损坏");
        const uint8_t* h = &cd[p];
        const uint16_t flags = rd16(h + 8), method = rd16(h + 10), nl = rd16(h + 28), xl = rd16(h + 30), cl = rd16(h + 32);
        uint64_t csize = rd32(h + 20), usize = rd32(h + 24), ho = rd32(h + 42);
        if (p + 46 + nl + xl + cl > cd.size()) return bad("ZIP 中央目录损坏");
        Entry e;
        e.name.assign(reinterpret_cast<const char*>(h + 46), nl);
        e.crc = rd32(h + 16);
        for (size_t x = 46 + nl; x + 4 <= size_t(46) + nl + xl;) {
            const uint16_t id = rd16(h + x), len = rd16(h + x + 2);
            const uint8_t* d = h + x + 4;
            if (x + 4 + len > size_t(46) + nl + xl) return bad("ZIP 附加字段损坏");
            if (id == kExtraZip64) {
                size_t q = 0;
                auto take = [&](uint64_t& v) {
                    if (v != kMax32) return true;
                    if (q + 8 > len) return false;
                    v = rd64(d + q);
                    q += 8;
                    return true;
                };
                if (!take(usize) || !take(csize) || !take(ho)) return bad("ZIP64 附加字段损坏");
            } else if (id == kExtraIis && len >= kIisExtraLen && d[0] == 1) {
                e.has_meta = true;
                e.iis = (d[1] & 1) != 0;
                e.orig_size = rd64(d + 2);
                std::memcpy(e.sha256, d + 10, 32);
                e.mtime = int64_t(rd64(d + 42));
            }
            x += 4 + size_t(len);
        }
        if (flags & 1) return bad("包内文件已加密（不是 IIS 生成的包）：" + e.name);
        if (method != 0 || csize != usize)
            return bad("包内文件经过压缩（不是 IIS 生成的 .iisz 包，请用普通解压软件打开）：" + e.name);
        e.size = csize;
        e.header_offset = ho;
        if (!e.has_meta) e.orig_size = e.size;
        e.dir = !e.name.empty() && e.name.back() == '/';
        if (ho > cd_off || e.size > cd_off - ho) return bad("条目位置超出范围：" + e.name);
        sanitize(e);
        entries_.push_back(std::move(e));
        p += 46 + size_t(nl) + xl + cl;
    }
    impl_->cd_off = cd_off;
    return IIS_OK;
}

int Reader::read(size_t i, std::vector<uint8_t>& data, std::string& why) const {
    const Entry& e = entries_.at(i);
    data.clear();
    if (e.dir) return IIS_OK;
    uint8_t lh[30];
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        if (impl_->src->read_at(e.header_offset, lh, 30) || rd32(lh) != kLocalSig) {
            why = "条目头损坏";
            return IIS_E_CORRUPT;
        }
        const uint64_t off = e.header_offset + 30 + rd16(lh + 26) + rd16(lh + 28);
        if (off > impl_->cd_off || e.size > impl_->cd_off - off) {
            why = "条目位置超出范围";
            return IIS_E_CORRUPT;
        }
        data.resize(size_t(e.size));
        if (e.size && impl_->src->read_at(off, data.data(), data.size())) {
            why = iis_last_error();
            return IIS_E_IO;
        }
    }
    if (crc32(0, data.data(), data.size()) != e.crc) {
        why = "CRC-32 不符（数据已损坏）";
        return IIS_E_CORRUPT;
    }
    return IIS_OK;
}

int Reader::original(size_t i, std::vector<uint8_t>& out, std::string& why) const {
    if (int st = read(i, out, why)) return st;
    const Entry& e = entries_[i];
    if (e.iis) {
        iis_reader* rd = nullptr;
        iis_buffer b{};
        int st = iis_reader_open_memory(out.data(), out.size(), &rd);
        if (st == IIS_OK) st = iis_reader_restore(rd, &b);  // 已与容器记录的 SHA-256 比对
        iis_container_info info{};
        if (st == IIS_OK) iis_reader_info(rd, &info);
        else why = std::string("无法还原：") + iis_last_error();
        iis_reader_close(rd);
        if (st == IIS_OK && e.has_meta && (b.size != e.orig_size || std::memcmp(info.orig_sha256, e.sha256, 32) != 0)) {
            st = IIS_E_CORRUPT;
            why = "与包内记录的 SHA-256 不符";
        }
        if (st == IIS_OK) out.assign(b.data, b.data + b.size);
        iis_buffer_free(&b);
        return st;
    }
    if (e.has_meta) {
        uint8_t h[32];
        iis_sha256(out.data(), out.size(), h);
        if (out.size() != e.orig_size || std::memcmp(h, e.sha256, 32) != 0) {
            why = "与包内记录的 SHA-256 不符";
            return IIS_E_CORRUPT;
        }
    }
    return IIS_OK;
}

// ---------------- 解包与校验 ----------------

ExtractResult extract(const std::string& package, const ExtractOptions& opt, const ProgressFn& progress) {
    ExtractResult r;
    const auto t0 = std::chrono::steady_clock::now();
    Reader rd;
    if (int st = rd.open(package)) {
        r.status = st;
        r.error = rd.error();
        return r;
    }
    const auto& es = rd.entries();
    std::error_code ec;

    // 输出位置
    std::string root, top_from, top_to;
    if (opt.here) {
        const stdfs::path pp = stdfs::absolute(P(package), ec);
        const std::string parent = U(pp.parent_path());
        std::set<std::string> tops;
        bool all_in_dir = true;
        for (const auto& e : es) {
            if (!e.bad.empty()) continue;
            const size_t k = e.path.find('/');
            tops.insert(e.path.substr(0, k));
            if (!e.dir && k == std::string::npos) all_in_dir = false;
        }
        if (tops.size() == 1 && all_in_dir) {  // 包内只有一个顶层文件夹：直接还原它
            top_from = *tops.begin();
            r.out_dir = unique_path(fs::join(parent, top_from), true);
            top_to = fs::base_name(r.out_dir);
            root = parent;
        } else {  // 放进“包名\”
            std::string stem = U(pp.filename());
            if (is_package_name(stem)) stem.resize(stem.size() - 5);
            root = r.out_dir = unique_path(fs::join(parent, stem), true);
        }
    } else {
        root = r.out_dir = opt.dest;
    }
    auto target_of = [&](const Entry& e) {
        std::string p = e.path;
        if (!top_from.empty()) p = top_to + p.substr(top_from.size());
        return fs::join(root, native(p));
    };

    stdfs::create_directories(P(r.out_dir), ec);
    if (fs::kind(r.out_dir) != fs::Kind::Dir) {
        r.status = IIS_E_IO;
        r.error = "无法创建文件夹：" + r.out_dir;
        return r;
    }
    std::vector<size_t> files;
    Progress pg;
    for (size_t i = 0; i < es.size(); ++i) {
        const Entry& e = es[i];
        if (!e.bad.empty()) {
            ++r.files, ++r.failed;
            problem(r, e.name, "不解出：" + e.bad);
            continue;
        }
        if (e.dir) {
            stdfs::create_directories(P(target_of(e)), ec);
            continue;
        }
        stdfs::create_directories(P(target_of(e)).parent_path(), ec);
        files.push_back(i);
        pg.bytes_total += e.orig_size;
    }
    r.files += files.size();
    pg.files_total = files.size();
    if (progress) progress(pg);

    std::mutex mu;
    parallel_for(files.size(), opt.threads, [&](size_t k) {
        if (cancelled(opt.cancel)) return;
        const Entry& e = es[files[k]];
        const std::string target = target_of(e);
        std::string why;
        int st = IIS_OK;
        bool skip = fs::kind(target) != fs::Kind::Missing;
        if (!skip) {
            std::vector<uint8_t> data;
            st = rd.original(files[k], data, why);
            if (st == IIS_OK) {
                st = write_new(target, data, why);
                if (st == IIS_E_CONFLICT) skip = true, st = IIS_OK;
                else if (st == IIS_OK) set_mtime(target, e.mtime);
            }
        }
        std::lock_guard<std::mutex> lk(mu);
        if (skip) {
            ++r.skipped;
            problem(r, e.path, "已存在，未覆盖");
        } else if (st == IIS_OK) {
            ++r.ok;
            r.bytes += e.orig_size;
        } else {
            ++r.failed;
            problem(r, e.path, why);
        }
        ++pg.files_done;
        pg.bytes_done += e.orig_size;
        pg.current = e.path;
        if (progress) progress(pg);
    });
    if (cancelled(opt.cancel)) {
        r.status = kCancelled;
        r.error = "已取消（已解出的文件保留）";
    }
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

ExtractResult verify(const std::string& package, bool deep, const ProgressFn& progress) {
    ExtractResult r;
    const auto t0 = std::chrono::steady_clock::now();
    Reader rd;
    if (int st = rd.open(package)) {
        r.status = st;
        r.error = rd.error();
        return r;
    }
    const auto& es = rd.entries();
    std::vector<size_t> files;
    Progress pg;
    for (size_t i = 0; i < es.size(); ++i) {
        if (!es[i].bad.empty()) {
            ++r.files, ++r.failed;
            problem(r, es[i].name, es[i].bad);
        } else if (!es[i].dir) {
            files.push_back(i);
            pg.bytes_total += es[i].orig_size;
        }
    }
    r.files += files.size();
    pg.files_total = files.size();
    std::mutex mu;
    parallel_for(files.size(), 0, [&](size_t k) {
        std::vector<uint8_t> data;
        std::string why;
        const int st = deep ? rd.original(files[k], data, why) : rd.read(files[k], data, why);
        std::lock_guard<std::mutex> lk(mu);
        if (st == IIS_OK) ++r.ok, r.bytes += es[files[k]].orig_size;
        else ++r.failed, problem(r, es[files[k]].path, why);
        ++pg.files_done;
        pg.bytes_done += es[files[k]].orig_size;
        pg.current = es[files[k]].path;
        if (progress) progress(pg);
    });
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

}  // namespace iis::pkg
