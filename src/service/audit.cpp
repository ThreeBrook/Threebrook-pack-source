#include "audit.hpp"

#include "iis/iis.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace iis::svc {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

const std::string kZero(64, '0');
constexpr size_t kHashTail = 75;  // ,"hash":"<64 位十六进制>"}

std::string hex_sha(const std::string& s) {
    uint8_t d[32];
    iis_sha256(s.data(), s.size(), d);
    static const char* x = "0123456789abcdef";
    std::string h;
    for (uint8_t b : d) h += x[b >> 4], h += x[b & 15];
    return h;
}

std::string now_text() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[64];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tm);
    char ms[8];
    std::snprintf(ms, sizeof ms, ".%03d", int(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000));
    char zone[8];
    std::strftime(zone, sizeof zone, "%z", &tm);
    return std::string(buf) + ms + zone;
}

std::string month_file() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof buf, "audit-%Y%m.log", &tm);
    return buf;
}

std::vector<fs::path> audit_files(const std::string& dir) {
    std::vector<fs::path> v;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(fs::u8path(dir), ec)) {
        const auto n = e.path().filename().u8string();
        if (n.size() == 16 && n.compare(0, 6, "audit-") == 0 && n.compare(12, 4, ".log") == 0) v.push_back(e.path());
    }
    std::sort(v.begin(), v.end());
    return v;
}

// 一行 → 去掉 hash 的正文与 hash；格式不对返回 false
bool split_line(const std::string& line, std::string& body, std::string& hash) {
    if (line.size() < kHashTail + 2 || line.compare(line.size() - kHashTail, 9, ",\"hash\":\"") != 0 ||
        line.compare(line.size() - 2, 2, "\"}") != 0)
        return false;
    hash = line.substr(line.size() - 66, 64);
    body = line.substr(0, line.size() - kHashTail) + "}";
    return true;
}

}  // namespace

Audit::~Audit() {
    if (lock_) CloseHandle(lock_);
}

std::string Audit::last_hash() const {
    std::lock_guard<std::mutex> lk(mu_);
    return hash_;
}

bool Audit::open(const std::string& dir, std::string& err) {
    dir_ = dir;
    std::error_code ec;
    fs::create_directories(fs::u8path(dir), ec);
    lock_ = CreateFileW((fs::u8path(dir) / "audit.lock").wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (lock_ == INVALID_HANDLE_VALUE) {
        lock_ = nullptr;
        err = "无法打开审计日志目录：" + dir + "（错误 " + std::to_string(GetLastError()) + "）";
        return false;
    }
    return append({});  // 读出最后一条（顺带修复半行）
}

bool Audit::append(const std::string& act, const std::string& path, json detail) {
    return append(std::vector<Entry>{{act, path, std::move(detail)}});
}

bool Audit::append(const std::vector<Entry>& entries) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!lock_) return false;
    OVERLAPPED ov{};
    if (!LockFileEx(lock_, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &ov)) return false;
    struct Unlock {
        HANDLE h;
        ~Unlock() {
            OVERLAPPED o{};
            UnlockFileEx(h, 0, 1, 0, &o);
        }
    } unlock{lock_};

    // 找到最后一条：最新的非空文件的最后一个完整行
    std::vector<Entry> extra;
    seq_ = 0;
    hash_ = kZero;
    auto files = audit_files(dir_);
    for (auto it = files.rbegin(); it != files.rend(); ++it) {
        HANDLE f = CreateFileW(it->wstring().c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{};
        GetFileSizeEx(f, &size);
        const int64_t n = std::min<int64_t>(size.QuadPart, 1 << 16);
        std::string tail(size_t(n), 0);
        LARGE_INTEGER pos;
        pos.QuadPart = size.QuadPart - n;
        SetFilePointerEx(f, pos, nullptr, FILE_BEGIN);
        DWORD got = 0;
        ReadFile(f, tail.data(), DWORD(n), &got, nullptr);
        tail.resize(got);
        if (!tail.empty() && tail.back() != '\n') {  // 半行：截到最后一个换行
            const size_t nl = tail.rfind('\n');
            const int64_t keep = pos.QuadPart + (nl == std::string::npos ? 0 : int64_t(nl) + 1);
            LARGE_INTEGER k;
            k.QuadPart = keep;
            SetFilePointerEx(f, k, nullptr, FILE_BEGIN);
            SetEndOfFile(f);
            extra.push_back({"audit_repair", it->filename().u8string(),
                             {{"truncated_bytes", size.QuadPart - keep}, {"reason", "上次追加未完成（断电或崩溃）"}}});
            tail.resize(size_t(keep - pos.QuadPart));
        }
        CloseHandle(f);
        if (tail.empty()) continue;
        tail.pop_back();
        const std::string line = tail.substr(tail.rfind('\n') == std::string::npos ? 0 : tail.rfind('\n') + 1);
        std::string body, hash;
        if (!split_line(line, body, hash)) return false;
        try {
            seq_ = json::parse(body).at("seq").get<uint64_t>();
        } catch (const json::exception&) {
            return false;
        }
        hash_ = hash;
        break;
    }
    extra.insert(extra.end(), entries.begin(), entries.end());
    if (extra.empty()) return true;

    std::string out;
    for (const auto& e : extra) {
        json j = {{"seq", seq_ + 1}, {"t", now_text()}, {"act", e.act}, {"path", e.path}, {"d", e.detail}, {"prev", hash_}};
        const std::string body = j.dump(-1, ' ', false, json::error_handler_t::replace);
        const std::string h = hex_sha(body);
        out += body.substr(0, body.size() - 1) + ",\"hash\":\"" + h + "\"}\n";
        ++seq_;
        hash_ = h;
    }
    HANDLE f = CreateFileW((fs::u8path(dir_) / month_file()).wstring().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD n = 0;
    const bool ok = WriteFile(f, out.data(), DWORD(out.size()), &n, nullptr) && n == out.size() && FlushFileBuffers(f);
    CloseHandle(f);
    return ok;
}

Audit::Verify Audit::verify(const std::string& dir) {
    Verify v;
    std::string prev = kZero;
    uint64_t seq = 0;
    for (const auto& p : audit_files(dir)) {
        ++v.files;
        std::ifstream f(p, std::ios::binary);
        std::string line;
        uint64_t lineno = 0;
        while (std::getline(f, line)) {
            ++lineno;
            const std::string where = p.filename().u8string() + " 第 " + std::to_string(lineno) + " 行";
            std::string body, hash;
            if (!split_line(line, body, hash)) {
                v.ok = false;
                v.bad_seq = seq + 1;
                v.error = where + "：格式不对";
                return v;
            }
            json j;
            try {
                j = json::parse(body);
            } catch (const json::exception&) {
                v.ok = false;
                v.bad_seq = seq + 1;
                v.error = where + "：不是合法 JSON";
                return v;
            }
            const uint64_t s = j.value("seq", uint64_t(0));
            if (hex_sha(body) != hash) {
                v.ok = false;
                v.bad_seq = s;
                v.error = where + "（序号 " + std::to_string(s) + "）：内容与 hash 不符，已被改动";
                return v;
            }
            if (j.value("prev", std::string()) != prev || s != seq + 1) {
                v.ok = false;
                v.bad_seq = s;
                v.error = where + "（序号 " + std::to_string(s) + "）：与上一条不连续，前面有记录被删除、插入或调换";
                return v;
            }
            prev = hash;
            seq = s;
            ++v.entries;
        }
    }
    v.last_hash = prev;
    return v;
}

}  // namespace iis::svc
