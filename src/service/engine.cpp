#include "engine.hpp"

#include "audit.hpp"
#include "classify.hpp"
#include "index.hpp"
#include "storage.hpp"

#include "iis/iis.h"

// 内核内部头文件（同仓库静态链接）：长路径、通配符
#include "formats.hpp"
#include "io.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <queue>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

namespace iis::svc {

namespace {

using steady = std::chrono::steady_clock;
using tp = steady::time_point;

tp after(double s) { return steady::now() + std::chrono::microseconds(int64_t(s * 1e6)); }

std::string lower(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

bool ends_with(const std::string& s, const char* suf) {
    const size_t n = std::strlen(suf);
    return s.size() >= n && lower(s.substr(s.size() - n)) == suf;
}

uint64_t ft64(const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

// FILETIME（1601 起 100ns）→ Unix 毫秒
int64_t ft_ms(uint64_t ft) { return (int64_t(ft) - 116444736000000000LL) / 10000; }

std::string hex32(const uint8_t* d) {
    static const char* x = "0123456789abcdef";
    std::string h;
    for (int i = 0; i < 32; ++i) h += x[d[i] >> 4], h += x[d[i] & 15];
    return h;
}

uint64_t now_ft() {
    FILETIME f;
    GetSystemTimeAsFileTime(&f);
    return ft64(f);
}

struct Info {
    bool exists = false, dir = false;
    uint64_t size = 0, mtime = 0;
};

Info stat(const std::string& path) {
    Info i;
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(io::win_path(path.c_str()).c_str(), GetFileExInfoStandard, &a)) return i;
    i.exists = true;
    i.dir = (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    i.size = (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
    i.mtime = ft64(a.ftLastWriteTime);
    return i;
}

// 写完判定的最后一步：拒绝他人写入方式打开（立即关闭）。别人正以写方式打开时失败 → 视为仍在写
enum class Open { Ok, Busy, Gone };
Open exclusive_test(const std::string& path) {
    HANDLE h = CreateFileW(io::win_path(path.c_str()).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? Open::Gone : Open::Busy;
    }
    CloseHandle(h);
    return Open::Ok;
}

std::string fmt1(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%g", v);
    return b;
}

std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    return a.back() == '\\' || a.back() == '/' ? a + b : a + "\\" + b;
}

// 检测进程 CPU 占用（整机百分比）
class ProcCpu {
public:
    explicit ProcCpu(const std::vector<std::string>& names) {
        for (const auto& n : names) names_.insert(lower(n));
        ncpu_ = std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    }
    // 返回自上次调用以来的占用；第一次调用返回 0
    double sample() {
        std::map<DWORD, uint64_t> cur;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe{};
            pe.dwSize = sizeof pe;
            for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
                const std::wstring w(pe.szExeFile);
                if (!names_.count(lower(io::narrow(w.data(), w.size())))) continue;
                HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
                if (!h) continue;
                FILETIME c, e, k, u;
                if (GetProcessTimes(h, &c, &e, &k, &u)) cur[pe.th32ProcessID] = ft64(k) + ft64(u);
                CloseHandle(h);
            }
            CloseHandle(snap);
        }
        const tp now = steady::now();
        double pct = 0;
        if (have_) {
            uint64_t busy = 0;
            for (const auto& kv : cur) {
                const auto it = prev_.find(kv.first);
                if (it != prev_.end() && kv.second >= it->second) busy += kv.second - it->second;
            }
            const double wall = std::chrono::duration<double>(now - last_).count();
            if (wall > 0) pct = double(busy) * 1e-7 / (wall * ncpu_) * 100.0;
        }
        prev_ = std::move(cur);
        last_ = now;
        have_ = true;
        return pct;
    }

private:
    std::set<std::string> names_;
    std::map<DWORD, uint64_t> prev_;
    tp last_;
    bool have_ = false;
    DWORD ncpu_ = 1;
};

}  // namespace

void apply_resources(const Config& cfg, Log& log) {
    HANDLE self = GetCurrentProcess();
    SetPriorityClass(self, PROCESS_MODE_BACKGROUND_END);  // 重载配置时先退出后台模式（未进入时调用无害）
    if (cfg.priority == "background") {
        // CPU、磁盘 IO、内存页优先级都降到后台：检测软件需要资源时让出
        if (SetPriorityClass(self, PROCESS_MODE_BACKGROUND_BEGIN)) {
            log.info("进程优先级：后台模式（CPU / IO 低优先级）");
        } else {
            log.warn("无法进入后台模式（错误 " + std::to_string(GetLastError()) + "），改用低优先级");
            SetPriorityClass(self, IDLE_PRIORITY_CLASS);
        }
    } else if (cfg.priority == "idle") {
        SetPriorityClass(self, IDLE_PRIORITY_CLASS);
        log.info("进程优先级：低");
    } else if (cfg.priority == "below_normal") {
        SetPriorityClass(self, BELOW_NORMAL_PRIORITY_CLASS);
        log.info("进程优先级：低于正常");
    } else {
        SetPriorityClass(self, NORMAL_PRIORITY_CLASS);
    }
    const DWORD n = GetActiveProcessorCount(0);
    DWORD_PTR mask = 0;
    if (cfg.cores.empty() && cfg.max_cores <= 0) {  // 不限：恢复全部核（重载配置时可能从限制改为不限）
        DWORD_PTR proc = 0, sys = 0;
        if (GetProcessAffinityMask(self, &proc, &sys) && proc != sys) SetProcessAffinityMask(self, sys);
    }
    if (!cfg.cores.empty()) {
        for (int c : cfg.cores)
            if (DWORD(c) < n) mask |= DWORD_PTR(1) << c;
    } else if (cfg.max_cores > 0 && DWORD(cfg.max_cores) < n) {
        // 取编号最大的若干个核：避开 0 号核（系统中断多），也通常与视觉软件绑定的低号核错开
        for (DWORD c = n - DWORD(cfg.max_cores); c < n; ++c) mask |= DWORD_PTR(1) << c;
    }
    if (mask) {
        if (SetProcessAffinityMask(self, mask)) {
            std::string s;
            for (DWORD c = 0; c < 64; ++c)
                if (mask >> c & 1) s += (s.empty() ? "" : ",") + std::to_string(c);
            log.info("限制使用逻辑核：" + s);
        } else {
            log.warn("设置核亲和性失败（错误 " + std::to_string(GetLastError()) + "）");
        }
    }
    static int job_rate = 0;  // 本进程已设置的 CPU 上限：Job Object 只能建一次，修改需重启服务
    if (job_rate && cfg.cpu_rate_percent != job_rate)
        log.warn("CPU 硬上限的修改要重启服务才生效（当前 " + std::to_string(job_rate) + "%）");
    if (cfg.cpu_rate_percent > 0 && !job_rate) {
        job_rate = cfg.cpu_rate_percent;
        HANDLE job = CreateJobObjectW(nullptr, nullptr);  // 句柄故意不关闭：随进程存在
        JOBOBJECT_CPU_RATE_CONTROL_INFORMATION r{};
        r.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
        r.CpuRate = DWORD(cfg.cpu_rate_percent) * 100;
        if (job && SetInformationJobObject(job, JobObjectCpuRateControlInformation, &r, sizeof r) &&
            AssignProcessToJobObject(job, self))
            log.info("CPU 硬上限：整机的 " + std::to_string(cfg.cpu_rate_percent) + "%（Job Object）");
        else
            log.warn("设置 Job Object CPU 上限失败（错误 " + std::to_string(GetLastError()) + "）");
    }
}

struct Engine::Impl {
    Config cfg;
    Log& log;
    Counters cnt;
    iis_raw_rules* rules = nullptr;
    iis_pool* pool = nullptr;
    Index index;
    Audit audit;
    Classifier cls;
    std::unique_ptr<Storage> storage;
    std::mutex claim_mu;  // 调度线程（提交压缩）与存储管理（删除）互斥占用路径
    std::unordered_set<std::string> inflight_paths, claimed_paths;
    std::vector<Audit::Entry> pending_audit;  // 调度线程累积，每轮写一次
    std::vector<std::string> finished_paths;
    bool accel_now = false;

    // ---- 跨线程队列 ----
    struct Note {
        std::string path;
        size_t root;
        bool have = false;
        uint64_t size = 0, mtime = 0;
    };
    struct Done {
        std::string path;
        int status;
        iis_file_result res;
        std::string error;
    };
    std::mutex qmu;
    std::condition_variable qcv;
    std::vector<Note> notes;
    std::vector<Done> dones;
    bool stopping = false;

    std::mutex smu;  // 扫描请求
    std::condition_variable scv;
    std::deque<std::pair<size_t, std::string>> scans;
    std::set<std::pair<size_t, std::string>> scan_set;

    HANDLE stop_event = nullptr;
    std::atomic<bool> paused{false}, low_space{false}, licensed{false};
    std::atomic<double> inspect_cpu{-1};
    std::vector<std::unique_ptr<std::atomic<bool>>> online;
    std::vector<std::unique_ptr<std::atomic<bool>>> root_low;
    std::vector<std::thread> threads;

    // ---- 调度状态（只由调度线程访问）----
    enum State { WAIT, STABLE };
    struct Item {
        size_t root = 0;
        uint64_t size = 0, mtime = 0;
        State state = WAIT;
        tp due;
        int attempts = 0;
        bool dirty = false;  // 处理中又有变化：完成后重新观察
    };
    std::unordered_map<std::string, Item> items, inflight;
    std::priority_queue<std::pair<tp, std::string>, std::vector<std::pair<tp, std::string>>, std::greater<>> heap;
    std::deque<std::string> ready;
    std::unordered_map<std::string, std::pair<uint64_t, uint64_t>> remembered;  // 保留原文件 / 冲突等：内容不变不再处理
    double tokens = 0;
    tp tokens_at = steady::now();
    std::atomic<size_t> n_wait{0}, n_ready{0}, n_inflight{0}, n_remembered{0};

    Impl(const Config& c, Log& l) : cfg(c), log(l) {}

    // ---------- 通用 ----------
    std::string rel_of(size_t root, const std::string& path) const {
        const std::string& r = cfg.watch[root].path;
        std::string rel = path.size() > r.size() ? path.substr(r.size()) : std::string();
        while (!rel.empty() && (rel[0] == '\\' || rel[0] == '/')) rel.erase(0, 1);
        std::replace(rel.begin(), rel.end(), '\\', '/');
        return rel;
    }

    bool wanted(size_t root, const std::string& rel) const {
        if (rel.empty() || ends_with(rel, ".iis") || ends_with(rel, ".iistmp")) return false;
        const WatchDir& w = cfg.watch[root];
        if (!w.recursive && rel.find('/') != std::string::npos) return false;
        bool inc = false;
        for (const auto& g : w.include) inc = inc || fmt::glob_match(g, rel);
        if (!inc) return false;
        for (const auto& g : w.exclude)
            if (fmt::glob_match(g, rel)) return false;
        return true;
    }

    void notify(Note n) {
        std::lock_guard<std::mutex> lk(qmu);
        notes.push_back(std::move(n));
        qcv.notify_one();
    }

    void request_scan(size_t root, const std::string& dir) {
        std::lock_guard<std::mutex> lk(smu);
        if (scan_set.insert({root, dir}).second) scans.push_back({root, dir});
        scv.notify_one();
    }

    bool wait_stop(double s) { return WaitForSingleObject(stop_event, DWORD(s * 1000)) == WAIT_OBJECT_0; }

    // ---------- 监控线程 ----------
    void watcher(size_t root) {
        const std::string& path = cfg.watch[root].path;
        const bool rec = cfg.watch[root].recursive;
        std::vector<DWORD> buf(std::max<size_t>(cfg.watch_buffer, 1024) / sizeof(DWORD));
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        bool first = true, reported_offline = false;
        while (WaitForSingleObject(stop_event, 0) != WAIT_OBJECT_0) {
            HANDLE h = CreateFileW(io::win_path(path.c_str()).c_str(), FILE_LIST_DIRECTORY,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                online[root]->store(false);
                if (!reported_offline)
                    log.warn("监控目录不可用，每 5 秒重试：" + path + "（错误 " + std::to_string(GetLastError()) + "）");
                reported_offline = true;
                if (wait_stop(5)) break;
                continue;
            }
            online[root]->store(true);
            if (reported_offline || !first) {
                log.info("监控目录已恢复，重新扫描：" + path);
                request_scan(root, path);  // 目录重建或句柄失效期间可能漏掉事件
            }
            first = reported_offline = false;
            for (;;) {
                ResetEvent(ov.hEvent);
                if (!ReadDirectoryChangesW(h, buf.data(), DWORD(buf.size() * sizeof(DWORD)), rec,
                                           FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                               FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE,
                                           nullptr, &ov, nullptr)) {
                    log.warn("监控失败：" + path + "（错误 " + std::to_string(GetLastError()) + "）");
                    break;
                }
                const HANDLE hs[2] = {ov.hEvent, stop_event};
                if (WaitForMultipleObjects(2, hs, FALSE, INFINITE) != WAIT_OBJECT_0) {
                    CancelIoEx(h, &ov);
                    DWORD n;
                    GetOverlappedResult(h, &ov, &n, TRUE);
                    CloseHandle(h);
                    CloseHandle(ov.hEvent);
                    return;
                }
                DWORD n = 0;
                const BOOL got = GetOverlappedResult(h, &ov, &n, FALSE);
                if (!got && GetLastError() == ERROR_NOTIFY_ENUM_DIR) n = 0;  // 另一种溢出报告方式
                else if (!got) {
                    log.warn("监控中断：" + path + "（错误 " + std::to_string(GetLastError()) + "），目录可能被删除或重建");
                    break;
                }
                if (n == 0) {  // 事件太多、缓冲区溢出：全量扫描补上
                    log.warn("目录事件溢出，全量扫描：" + path);
                    request_scan(root, path);
                    continue;
                }
                for (auto* p = reinterpret_cast<const uint8_t*>(buf.data());;) {
                    const auto* fi = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(p);
                    if (fi->Action == FILE_ACTION_ADDED || fi->Action == FILE_ACTION_MODIFIED ||
                        fi->Action == FILE_ACTION_RENAMED_NEW_NAME || fi->Action == FILE_ACTION_REMOVED ||
                        fi->Action == FILE_ACTION_RENAMED_OLD_NAME) {
                        Note note;
                        note.path = join(path, io::narrow(fi->FileName, fi->FileNameLength / sizeof(WCHAR)));
                        note.root = root;
                        notify(std::move(note));
                    }
                    if (!fi->NextEntryOffset) break;
                    p += fi->NextEntryOffset;
                }
                if (cfg.watch_stall_ms > 0) Sleep(DWORD(cfg.watch_stall_ms));
            }
            CloseHandle(h);
            online[root]->store(false);
            if (wait_stop(1)) break;
        }
        CloseHandle(ov.hEvent);
    }

    // ---------- 扫描线程 ----------
    void scan_dir(size_t root, const std::string& dir, int depth) {
        std::wstring w = io::win_path(dir.c_str());
        if (w.empty()) return;
        if (w.back() != L'\\') w += L'\\';
        w += L'*';
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(w.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                    FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) return;
        std::vector<std::string> subdirs;
        std::vector<Note> batch;
        do {
            const std::wstring_view n(fd.cFileName);
            if (n == L"." || n == L"..") continue;
            const std::string path = join(dir, io::narrow(n.data(), n.size()));
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;  // 不跟随链接，防止循环
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (cfg.watch[root].recursive) subdirs.push_back(path);
                continue;
            }
            Note note;
            note.path = path;
            note.root = root;
            note.have = true;
            note.size = (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            note.mtime = ft64(fd.ftLastWriteTime);
            batch.push_back(std::move(note));
            if (batch.size() >= 1024) {
                std::lock_guard<std::mutex> lk(qmu);
                notes.insert(notes.end(), std::make_move_iterator(batch.begin()), std::make_move_iterator(batch.end()));
                batch.clear();
                qcv.notify_one();
            }
        } while (FindNextFileW(h, &fd) && WaitForSingleObject(stop_event, 0) != WAIT_OBJECT_0);
        FindClose(h);
        if (!batch.empty()) {
            std::lock_guard<std::mutex> lk(qmu);
            notes.insert(notes.end(), std::make_move_iterator(batch.begin()), std::make_move_iterator(batch.end()));
            qcv.notify_one();
        }
        if (depth < 64)
            for (const auto& s : subdirs) scan_dir(root, s, depth + 1);
    }

    void scanner() {
        for (;;) {
            std::pair<size_t, std::string> job;
            {
                std::unique_lock<std::mutex> lk(smu);
                scv.wait(lk, [&] { return stopping || !scans.empty(); });
                if (stopping) return;
                job = scans.front();
            }
            const auto t0 = steady::now();
            scan_dir(job.first, job.second, 0);
            {
                std::lock_guard<std::mutex> lk(smu);
                scans.pop_front();
                scan_set.erase(job);
            }
            const double s = std::chrono::duration<double>(steady::now() - t0).count();
            if (s > 1) log.info("扫描完成：" + job.second + "（" + std::to_string(int(s)) + " 秒）");
        }
    }

    // ---------- 调度线程 ----------
    void schedule(const std::string& path, Item it) {
        heap.push({it.due, path});
        items[path] = std::move(it);
    }

    // 按修改时间算等待：最后修改后满 delay_s。系统时间回拨（修改时间在“未来”）时最多等 delay_s，不会卡住
    tp due_for(uint64_t mtime) const {
        const int64_t age = int64_t(now_ft()) - int64_t(mtime);  // 100ns
        const double delay = accel_now ? std::min(cfg.delay_s, cfg.disk.accelerate_delay_s) : cfg.delay_s;
        const double wait = std::clamp(delay - double(age) * 1e-7, 0.0, delay);
        return after(wait);
    }

    // ---------- 索引（调度线程）----------
    Row base_row(const std::string& path, size_t root, const std::string& rel) const {
        Row r;
        r.path = path;
        r.root = cfg.watch[root].path;
        r.rel = rel;
        const Labels l = cls.classify(rel);
        r.station = l.station;
        r.product = l.product;
        r.batch = l.batch;
        r.verdict = l.verdict;
        return r;
    }

    void index_pending(const std::string& path, size_t root, const std::string& rel, const Info& st) {
        Row old;
        const bool have = index.get(path, old);
        const int64_t m = ft_ms(st.mtime);
        if (have && old.state != ST_DELETED && old.mtime_ms == m && old.orig_size == st.size) return;
        if (have && old.state == ST_DELETED && !stat(path).exists) return;  // 扫描结果已过时（刚被删除），不要“复活”
        Row r = base_row(path, root, rel);
        r.mtime_ms = m;
        r.orig_size = st.size;
        const Info c = stat(path + ".iis");
        r.disk_size = st.size + (c.exists && !c.dir ? c.size : 0);
        r.state = ST_PENDING;
        index.upsert(r);
    }

    // 发现 X.iis（启动扫描、别处拷来的、以前压缩的）：原文件不在时按已压缩登记，大小与 SHA-256 取自容器头
    void observe_iis(const Note& n, const std::string& rel) {
        const std::string x = n.path.substr(0, n.path.size() - 4), xrel = rel.substr(0, rel.size() - 4);
        if (xrel.empty() || stat(x).exists) return;  // 原文件还在：以原文件为准
        Info c;
        if (n.have) c = {true, false, n.size, n.mtime};
        else c = stat(n.path);
        if (!c.exists || c.dir) {
            gone(x);
            return;
        }
        Row old;
        const bool have = index.get(x, old);
        if (have && old.state == ST_COMPRESSED && old.disk_size == c.size) return;
        if (have && old.state == ST_DELETED && !stat(n.path).exists) return;  // 扫描结果已过时
        Row r = base_row(x, n.root, xrel);
        r.mtime_ms = have && old.state != ST_DELETED ? old.mtime_ms : ft_ms(c.mtime);
        r.disk_size = c.size;
        r.state = ST_COMPRESSED;
        iis_reader* rd = nullptr;
        if (iis_reader_open_file(n.path.c_str(), &rd) == IIS_OK) {
            iis_container_info info{};
            if (iis_reader_info(rd, &info) == IIS_OK) {
                r.orig_size = info.orig_size;
                r.sha256 = hex32(info.orig_sha256);
            }
        }
        iis_reader_close(rd);
        index.upsert(r);
    }

    // 原文件与 .iis 都不在了（被外部删除或移走）：索引标为已删除并记审计
    void gone(const std::string& x) {
        {
            std::lock_guard<std::mutex> lk(claim_mu);
            if (claimed_paths.count(x)) return;  // 存储管理正在删除它：由那边记“delete”
        }
        if (stat(x).exists || stat(x + ".iis").exists) return;
        Row r;
        if (!index.get(x, r) || r.state == ST_DELETED) return;
        r.state = ST_DELETED;
        r.disk_size = 0;
        index.upsert(r);
        pending_audit.push_back({"missing", x, {{"reason", "文件被外部删除或移走"}, {"verdict", r.verdict}}});
    }

    void record(const Done& d, const Item& item) {
        const iis_file_result& r = d.res;
        if (d.status != IIS_OK || r.outcome == IIS_FILE_SKIPPED) return;
        Row row;
        if (!index.get(d.path, row) || row.state == ST_DELETED) row = base_row(d.path, item.root, rel_of(item.root, d.path));
        row.mtime_ms = ft_ms(item.mtime);
        row.orig_size = item.size;
        const bool orig_left = !(r.original == IIS_ORIG_RELEASED || r.original == IIS_ORIG_GONE);
        const char* released = r.original == IIS_ORIG_RELEASED ? "released" : r.original == IIS_ORIG_KEPT ? "kept"
                               : r.original == IIS_ORIG_BUSY ? "busy" : r.original == IIS_ORIG_CHANGED ? "changed"
                               : r.original == IIS_ORIG_GONE ? "gone" : "failed";
        if (r.outcome == IIS_FILE_STORED) {
            row.state = ST_STORED;
            row.disk_size = item.size;
        } else {
            row.state = ST_COMPRESSED;
            row.disk_size = r.container_size + (orig_left ? item.size : 0);
            iis_reader* rd = nullptr;
            if (iis_reader_open_file((d.path + ".iis").c_str(), &rd) == IIS_OK) {
                iis_container_info info{};
                if (iis_reader_info(rd, &info) == IIS_OK) row.sha256 = hex32(info.orig_sha256);
            }
            iis_reader_close(rd);
        }
        if (r.original == IIS_ORIG_CHANGED) row.state = ST_PENDING;  // 原文件已是新内容：等重新压缩
        index.upsert(row);
        if (r.outcome == IIS_FILE_ALREADY) {
            if (r.original == IIS_ORIG_RELEASED) pending_audit.push_back({"release", d.path, {{"reason", "已有一致的 .iis"}}});
            return;
        }
        const bool stored = r.outcome == IIS_FILE_STORED;
        index.add_daily({"", 1, item.size, stored ? item.size : r.container_size, 0, 0});
        static const char* methods[] = {"", "image", "zstd", "store"};
        pending_audit.push_back({"compress", d.path,
                                 {{"method", methods[r.compress.method & 3]}, {"orig_size", item.size},
                                  {"stored_size", stored ? item.size : r.container_size}, {"sha256", row.sha256},
                                  {"original", released}, {"verdict", row.verdict}, {"station", row.station},
                                  {"detail", r.detail}}});
    }

    void observe(const Note& n) {
        const std::string rel = rel_of(n.root, n.path);
        if (rel.empty() || ends_with(rel, ".iistmp")) return;
        if (ends_with(rel, ".iis")) {
            observe_iis(n, rel);
            return;
        }
        const bool want = wanted(n.root, rel);
        if (!want && n.have) return;  // 扫描到的普通文件不在范围内；事件则可能是目录，要先看一眼
        Info st;
        if (n.have) st = {true, false, n.size, n.mtime};
        else st = stat(n.path);
        if (!st.exists) {
            items.erase(n.path);
            remembered.erase(n.path);
            gone(n.path);
            return;
        }
        if (st.dir) {  // 新建或移入的子目录：里面的文件可能没有单独的事件
            if (cfg.watch[n.root].recursive) request_scan(n.root, n.path);
            return;
        }
        if (!want) return;
        if (auto f = inflight.find(n.path); f != inflight.end()) {
            f->second.dirty = true;
            return;
        }
        if (auto r = remembered.find(n.path); r != remembered.end()) {
            if (r->second == std::make_pair(st.size, st.mtime)) return;
            remembered.erase(r);
        }
        if (!cfg.release) {  // 保留原文件模式：已有更新的 .iis 就不再处理
            const Info c = stat(n.path + ".iis");
            if (c.exists && !c.dir && c.mtime >= st.mtime) {
                remembered[n.path] = {st.size, st.mtime};
                return;
            }
        }
        index_pending(n.path, n.root, rel, st);
        auto it = items.find(n.path);
        if (it != items.end() && it->second.size == st.size && it->second.mtime == st.mtime) return;  // 已在等待
        Item item;
        item.root = n.root;
        item.size = st.size;
        item.mtime = st.mtime;
        item.state = WAIT;
        item.due = due_for(st.mtime);
        if (it != items.end()) item.attempts = it->second.attempts;
        schedule(n.path, item);
    }

    void step(const std::string& path) {
        auto it = items.find(path);
        if (it == items.end()) return;
        Item& item = it->second;
        const Info st = stat(path);
        if (!st.exists || st.dir) {
            items.erase(it);
            return;
        }
        if (st.size != item.size || st.mtime != item.mtime) {  // 还在写：从新的修改时间重新计时
            item.size = st.size;
            item.mtime = st.mtime;
            item.state = WAIT;
            item.due = due_for(st.mtime);
            heap.push({item.due, path});
            return;
        }
        if (item.state == WAIT && cfg.stable_s > 0) {
            item.state = STABLE;
            item.due = after(cfg.stable_s);
            heap.push({item.due, path});
            return;
        }
        switch (exclusive_test(path)) {
            case Open::Gone: items.erase(it); return;
            case Open::Busy:  // 别人正以写方式打开：稍后再试
                item.state = STABLE;
                item.due = after(cfg.retry_s);
                heap.push({item.due, path});
                return;
            case Open::Ok:
                item.due = tp::max();  // 已就绪，不再从堆里触发
                ready.push_back(path);
                return;
        }
    }

    void retry_later(const std::string& path, Item item, double s) {
        item.state = STABLE;
        item.due = after(s);
        item.dirty = false;
        schedule(path, item);
    }

    void finish(Done& d) {
        auto f = inflight.find(d.path);
        if (f == inflight.end()) return;
        Item item = f->second;
        inflight.erase(f);
        finished_paths.push_back(d.path);
        record(d, item);
        const auto& r = d.res;
        const auto key = std::make_pair(item.size, item.mtime);
        const std::string name = d.path;
        if (d.status == IIS_OK) {
            switch (r.outcome) {
                case IIS_FILE_COMPRESSED:
                    ++cnt.compressed;
                    cnt.bytes_in += r.compress.orig_size;
                    cnt.bytes_out += r.container_size;
                    break;
                case IIS_FILE_ALREADY: ++cnt.already; break;
                case IIS_FILE_STORED: ++cnt.stored; break;
                default: ++cnt.skipped; break;
            }
            if (r.original == IIS_ORIG_RELEASED) ++cnt.released;
            if (r.outcome == IIS_FILE_COMPRESSED || r.outcome == IIS_FILE_ALREADY) {
                if (r.original == IIS_ORIG_BUSY) {
                    retry_later(name, item, cfg.retry_s);  // 原文件被占用：稍后再释放
                    return;
                }
                if (r.original == IIS_ORIG_CHANGED) item.dirty = true;
                if (r.original == IIS_ORIG_FAILED) log.warn(name + "：" + r.detail);
            }
            if (!(r.original == IIS_ORIG_RELEASED || r.original == IIS_ORIG_GONE)) remembered[name] = key;
        } else {
            ++cnt.errors;
            switch (d.status) {
                case IIS_E_NOT_FOUND: break;
                case IIS_E_BUSY: retry_later(name, item, cfg.retry_s); return;
                case IIS_E_NO_SPACE:
                    log.warn(name + "：磁盘已满，稍后重试");
                    retry_later(name, item, cfg.retry_s);
                    return;
                case IIS_E_CONFLICT:
                    log.warn(name + "：" + d.error);
                    remembered[name] = key;
                    break;
                default:
                    log.error(name + "：" + iis_status_str(d.status) + "：" + d.error);
                    if (++item.attempts < 3) {
                        retry_later(name, item, cfg.retry_s);
                        return;
                    }
                    log.error(name + "：连续失败 3 次，不再自动重试（文件变化后会重新处理）");
                    remembered[name] = key;
                    break;
            }
        }
        if (item.dirty) {
            Note n;
            n.path = name;
            n.root = item.root;
            observe(n);
        }
    }

    int accel_threads() const { return cfg.disk.accelerate_threads > 0 ? cfg.disk.accelerate_threads : cfg.threads * 2; }

    bool claim(const std::string& path) {
        std::lock_guard<std::mutex> lk(claim_mu);
        if (inflight_paths.count(path)) return false;
        claimed_paths.insert(path);
        return true;
    }

    void unclaim(const std::string& path) {
        std::lock_guard<std::mutex> lk(claim_mu);
        claimed_paths.erase(path);
    }

    bool submit_one() {
        if (paused.load() || !licensed.load() || ready.empty()) return false;
        const int cap = accel_now ? accel_threads() : cfg.threads;
        if (iis_pool_pending(pool) >= size_t(cap) + 1) return false;  // 队列保持很短，暂停能及时生效
        const std::string path = ready.front();
        auto it = items.find(path);
        if (it == items.end() || it->second.due != tp::max()) {  // 已删除，或就绪后又有变化（已重新排队）
            ready.pop_front();
            return true;
        }
        if (root_low[it->second.root]->load()) {
            ready.pop_front();
            Item item = it->second;
            items.erase(it);
            retry_later(path, item, cfg.retry_s);
            return true;
        }
        if (cfg.max_mb_s > 0) {
            const tp now = steady::now();
            const double rate = cfg.max_mb_s * 1e6;
            tokens = std::min(tokens + rate * std::chrono::duration<double>(now - tokens_at).count(), rate);
            tokens_at = now;
            if (tokens < 0) return false;
            tokens -= double(it->second.size);
        }
        {
            std::lock_guard<std::mutex> lk(claim_mu);
            if (claimed_paths.count(path)) return false;  // 正被存储管理删除：等它结束
            inflight_paths.insert(path);
        }
        ready.pop_front();
        const std::string rel = rel_of(it->second.root, path);
        if (iis_pool_submit(pool, path.c_str(), rel.c_str()) != IIS_OK) {
            {
                std::lock_guard<std::mutex> lk(claim_mu);
                inflight_paths.erase(path);
            }
            log.error(path + "：提交到线程池失败：" + iis_last_error());
            Item item = it->second;
            items.erase(it);
            retry_later(path, item, cfg.retry_s);
            return true;
        }
        inflight[path] = it->second;
        items.erase(it);
        return true;
    }

    void scheduler() {
        std::vector<Note> ns;
        std::vector<Done> ds;
        tp counts_at = steady::now();
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(qmu);
                tp until = after(0.2);
                if (!heap.empty()) until = std::min(until, heap.top().first);
                qcv.wait_until(lk, until, [&] { return stopping || !notes.empty() || !dones.empty(); });
                if (stopping) return;
                ns.swap(notes);
                ds.swap(dones);
            }
            const bool accel = storage && storage->accelerate();
            if (accel != accel_now) {
                accel_now = accel;
                log.info(accel ? "空间告警：加速压缩（线程 " + std::to_string(accel_threads()) + "，延迟 " +
                                     std::to_string(int(std::min(cfg.delay_s, cfg.disk.accelerate_delay_s))) + " 秒）"
                               : "空间恢复：压缩速度恢复正常");
                for (auto& kv : items)  // 等待中的文件按新的延迟重新计时
                    if (kv.second.state == WAIT) {
                        kv.second.due = due_for(kv.second.mtime);
                        heap.push({kv.second.due, kv.first});
                    }
            }
            {
                Index::Tx tx(index);
                for (auto& d : ds) finish(d);
                for (const auto& n : ns) observe(n);
                const tp now = steady::now();
                while (!heap.empty() && heap.top().first <= now) {
                    const auto [due, path] = heap.top();
                    heap.pop();
                    auto it = items.find(path);
                    if (it == items.end() || it->second.due != due) continue;  // 已过期的堆项
                    step(path);
                }
            }
            ds.clear();
            ns.clear();
            if (!finished_paths.empty()) {
                std::lock_guard<std::mutex> lk(claim_mu);
                for (const auto& p : finished_paths) inflight_paths.erase(p);
                finished_paths.clear();
            }
            if (!pending_audit.empty()) {
                if (!audit.append(pending_audit)) log.error("写审计日志失败");
                pending_audit.clear();
            }
            while (submit_one()) {
            }
            if (steady::now() < counts_at) continue;
            counts_at = after(0.5);
            size_t waiting = 0;
            for (const auto& kv : items) waiting += kv.second.due != tp::max();
            n_wait = waiting;
            n_ready = ready.size();
            n_inflight = inflight.size();
            n_remembered = remembered.size();
        }
    }

    // ---------- 巡检线程 ----------
    nlohmann::json status() {
        const Snapshot s = snap();
        nlohmann::json j = {{"time", std::time(nullptr)},
                            {"compressed", s.compressed},
                            {"already", s.already},
                            {"stored", s.stored},
                            {"skipped", s.skipped},
                            {"errors", s.errors},
                            {"released", s.released},
                            {"bytes_in", s.bytes_in},
                            {"bytes_out", s.bytes_out},
                            {"waiting", s.waiting},
                            {"ready", s.ready},
                            {"inflight", s.inflight},
                            {"paused", s.paused},
                            {"low_space", s.low_space},
                            {"roots_online", s.roots_online},
                            {"roots", cfg.watch.size()},
                            {"inspect_cpu", s.inspect_cpu},
                            {"accelerate", s.accelerate},
                            {"deleted", s.deleted},
                            {"deleted_ng", s.deleted_ng},
                            {"licensed", s.licensed},
                            {"audit_seq", audit.last_seq()},
                            {"audit_hash", audit.last_hash()}};
        // 物理用量与等效容量（等效容量只作统计，不作为可用容量对外报告）
        nlohmann::json roots = nlohmann::json::array();
        const auto totals = index.all_totals();
        std::vector<RootSpace> spaces = storage ? storage->spaces() : std::vector<RootSpace>{};
        for (size_t i = 0; i < cfg.watch.size(); ++i) {
            const auto t = totals.count(cfg.watch[i].path) ? totals.at(cfg.watch[i].path) : Totals{};
            nlohmann::json r = {{"path", cfg.watch[i].path}, {"files", t.files}, {"used", t.used}, {"equivalent", t.equiv},
                                {"ratio", t.used ? double(t.equiv) / double(t.used) : 0.0}};
            if (i < spaces.size()) {
                const RootSpace& sp = spaces[i];
                r["online"] = sp.online;
                r["capacity"] = sp.capacity;
                r["free"] = sp.free;
                r["level"] = sp.level == 2 ? "delete" : sp.level == 1 ? "warn" : "ok";
                r["stuck"] = sp.stuck;
                r["thresholds"] = {{"warn", sp.warn}, {"delete", sp.del}, {"target", sp.target}};
            }
            roots.push_back(r);
        }
        j["storage"] = roots;
        j["verdicts"] = index.verdict_counts();
        nlohmann::json days = nlohmann::json::array();
        for (const auto& d : index.daily(14))
            days.push_back({{"day", d.day}, {"files", d.files}, {"bytes_in", d.bytes_in}, {"bytes_out", d.bytes_out},
                            {"deleted", d.deleted}, {"deleted_bytes", d.deleted_bytes}});
        j["daily"] = days;
        return j;
    }

    void write_status() {
        const nlohmann::json j = status();
        const Snapshot s = snap();
        const auto p = std::filesystem::u8path(cfg.status_file);
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
        const auto tmp = p.wstring() + L".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            f << j.dump(1);
        }
        MoveFileExW(tmp.c_str(), p.wstring().c_str(), MOVEFILE_REPLACE_EXISTING);
        char ratio[32];
        std::snprintf(ratio, sizeof ratio, "%.2f", s.bytes_out ? double(s.bytes_in) / double(s.bytes_out) : 0.0);
        log.info("统计：已压缩 " + std::to_string(s.compressed) + "（" + std::to_string(s.bytes_in / 1000000) + " MB → " +
                 std::to_string(s.bytes_out / 1000000) + " MB，" + ratio + " 倍），保留原文件 " +
                 std::to_string(s.stored) + "，错误 " + std::to_string(s.errors) + "；等待 " +
                 std::to_string(s.waiting) + "，就绪 " + std::to_string(s.ready) + "，处理中 " +
                 std::to_string(s.inflight) + (s.paused ? "；已暂停" : "") + (s.low_space ? "；空间不足" : ""));
    }

    void housekeeping() {
        std::unique_ptr<ProcCpu> cpu;
        if (!cfg.pause.processes.empty()) cpu = std::make_unique<ProcCpu>(cfg.pause.processes);
        tp next_cpu = steady::now(), next_space = steady::now(), next_status = after(cfg.status_s);
        tp next_rescan = cfg.rescan_s > 0 ? after(cfg.rescan_s) : tp::max();
        tp next_storage = after(1);
        tp calm_since = tp::max();
        if (cpu) cpu->sample();
        while (!wait_stop(0.1)) {
            const tp now = steady::now();
            if (cpu && now >= next_cpu) {
                next_cpu = after(cfg.pause.sample_s);
                const double c = cpu->sample();
                inspect_cpu = c;
                if (!paused && c >= cfg.pause.pause_percent) {
                    paused = true;
                    iis_pool_pause(pool, 1);
                    calm_since = tp::max();
                    log.info("检测进程 CPU " + std::to_string(int(c + 0.5)) + "%，暂停压缩");
                } else if (paused) {
                    if (c >= cfg.pause.resume_percent) calm_since = tp::max();
                    else if (calm_since == tp::max()) calm_since = now;
                    if (calm_since != tp::max() &&
                        std::chrono::duration<double>(now - calm_since).count() >= cfg.pause.resume_after_s) {
                        paused = false;
                        iis_pool_pause(pool, 0);
                        log.info("检测进程 CPU 回落到 " + std::to_string(int(c + 0.5)) + "%，恢复压缩");
                        qcv.notify_one();
                    }
                }
            }
            if (now >= next_space) {
                next_space = after(5);
                bool any = false;
                for (size_t i = 0; i < cfg.watch.size(); ++i) {
                    ULARGE_INTEGER avail{};
                    const bool ok = GetDiskFreeSpaceExW(io::win_path(cfg.watch[i].path.c_str()).c_str(), &avail,
                                                        nullptr, nullptr);
                    const bool low = ok && double(avail.QuadPart) < cfg.min_free_mb * 1e6;
                    if (low != root_low[i]->load())
                        low ? log.warn("剩余空间低于 " + std::to_string(int64_t(cfg.min_free_mb)) + " MB，暂停写入：" +
                                       cfg.watch[i].path)
                            : log.info("剩余空间恢复，继续压缩：" + cfg.watch[i].path);
                    root_low[i]->store(low);
                    any = any || low;
                }
                low_space = any;
            }
            if (storage && now >= next_storage) {
                storage->tick();
                next_storage = after(cfg.disk.check_s);
                if (storage->accelerate() != accel_now) qcv.notify_one();
            }
            if (now >= next_rescan) {
                next_rescan = after(cfg.rescan_s);
                for (size_t i = 0; i < cfg.watch.size(); ++i) request_scan(i, cfg.watch[i].path);
            }
            if (now >= next_status) {
                next_status = after(cfg.status_s);
                write_status();
            }
        }
        write_status();
    }

    Snapshot snap() const {
        Snapshot s{};
        s.compressed = cnt.compressed;
        s.already = cnt.already;
        s.stored = cnt.stored;
        s.skipped = cnt.skipped;
        s.errors = cnt.errors;
        s.released = cnt.released;
        s.bytes_in = cnt.bytes_in;
        s.bytes_out = cnt.bytes_out;
        s.waiting = n_wait;
        s.ready = n_ready;
        s.inflight = n_inflight;
        s.remembered = n_remembered;
        s.paused = paused;
        s.low_space = low_space;
        for (const auto& o : online) s.roots_online += o->load() ? 1 : 0;
        s.inspect_cpu = inspect_cpu;
        s.accelerate = storage && storage->accelerate();
        s.deleted = storage ? storage->deleted() : 0;
        s.deleted_ng = storage ? storage->deleted_ng() : 0;
        s.licensed = licensed;
        return s;
    }

    static void on_done(void* user, const char* path, int status, const iis_file_result* res, const char* err) {
        auto* self = static_cast<Impl*>(user);
        Done d{path, status, *res, err ? err : ""};
        std::lock_guard<std::mutex> lk(self->qmu);
        self->dones.push_back(std::move(d));
        self->qcv.notify_one();
    }

    static void on_recover(void* user, const char* path, const char* action) {
        static_cast<Impl*>(user)->log.info(std::string("恢复：") + path + "：" + action);
    }

    bool start() {
        log.info(std::string("启动 ") + iis_build_info());
        for (const auto& w : cfg.watch) {
            std::string inc;
            for (const auto& g : w.include) inc += (inc.empty() ? "" : " ") + g;
            log.info("监控目录：" + w.path + (w.recursive ? "（含子目录）" : "") + "，文件：" + inc);
        }
        log.info("写完判定：最后修改后 " + std::to_string(int(cfg.delay_s)) + " 秒，稳定 " +
                 fmt1(cfg.stable_s) + " 秒；压缩线程 " + std::to_string(cfg.threads) + "；" +
                 (cfg.release ? "提交后释放原文件" : "保留原文件"));
        if (!cfg.raw_rules_file.empty()) {
            std::ifstream f(std::filesystem::u8path(cfg.raw_rules_file), std::ios::binary);
            const std::string t((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (!f.good() && t.empty()) {
                log.error("无法读取 RAW 规则文件：" + cfg.raw_rules_file);
                return false;
            }
            if (iis_raw_rules_parse(t.data(), t.size(), &rules) != IIS_OK) {
                log.error(std::string("RAW 规则文件有误：") + iis_last_error());
                return false;
            }
        }
        std::string err;
        if (!cls.init(cfg.classify, err)) {
            log.error("识别规则有误：" + err);
            return false;
        }
        const std::string data = cfg.data_dir.empty() ? cfg.log_dir + "\\data" : cfg.data_dir;
        if (!index.open(data + "\\index.db", err)) {
            log.error(err);
            return false;
        }
        if (!audit.open(data + "\\audit", err)) {
            log.error(err);
            return false;
        }
        if (!cfg.config_sha256.empty()) {
            const std::string before = index.meta("config_sha256");
            if (before != cfg.config_sha256) {
                nlohmann::json d = {{"before", before}, {"after", cfg.config_sha256}};
                if (!cfg.change_note.empty()) d["change"] = nlohmann::json::parse(cfg.change_note, nullptr, false);
                audit.append("config", "", d);
                index.set_meta("config_sha256", cfg.config_sha256);
                if (!before.empty()) log.info("配置已变更（已记审计）");
            }
        }
        audit.append("start", "", {{"build", iis_build_info()}, {"roots", cfg.watch.size()}});
        log.info("索引与审计：" + data + "（审计已有 " + std::to_string(audit.last_seq()) + " 条）");
        storage = std::make_unique<Storage>(
            cfg, log, index, audit, cls, [this](const std::string& p) { return claim(p); },
            [this](const std::string& p) { unclaim(p); });
        apply_resources(cfg, log);
        stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        for (size_t i = 0; i < cfg.watch.size(); ++i) {
            online.push_back(std::make_unique<std::atomic<bool>>(false));
            root_low.push_back(std::make_unique<std::atomic<bool>>(false));
        }
        iis_file_options opt{};
        opt.compress.fallback_zstd_level = cfg.zstd_level;
        opt.compress.raw_rules = rules;
        opt.compress.min_saving = cfg.min_saving;
        opt.release = cfg.release ? IIS_RELEASE_DELETE : IIS_RELEASE_KEEP;
        // 启动恢复：先于任何压缩
        for (const auto& w : cfg.watch) {
            iis_recover_report rep{};
            if (iis_recover_dir(w.path.c_str(), w.recursive, &opt, &Impl::on_recover, this, &rep) != IIS_OK) {
                log.warn("恢复跳过（" + std::string(iis_last_error()) + "）");
                continue;
            }
            if (rep.temps_removed || rep.temps_promoted || rep.originals_released || rep.pending || rep.errors)
                audit.append("recover", w.path,
                             {{"temps_removed", rep.temps_removed}, {"temps_promoted", rep.temps_promoted},
                              {"temps_kept", rep.temps_kept}, {"originals_released", rep.originals_released},
                              {"pending", rep.pending}, {"errors", rep.errors}});
            log.info("恢复完成：" + w.path + "：扫描 " + std::to_string(rep.files_scanned) + " 个文件，删除临时文件 " +
                     std::to_string(rep.temps_removed) + "，提升 " + std::to_string(rep.temps_promoted) + "，保留 " +
                     std::to_string(rep.temps_kept) + "，释放原文件 " + std::to_string(rep.originals_released) +
                     "，待处理 " + std::to_string(rep.pending) + "，错误 " + std::to_string(rep.errors));
        }
        if (iis_pool_create(std::max(cfg.threads, accel_threads()), &opt, &Impl::on_done, this, &pool) != IIS_OK) {
            log.error(std::string("无法创建线程池：") + iis_last_error());
            return false;
        }
        threads.emplace_back([this] { scheduler(); });
        threads.emplace_back([this] { scanner(); });
        threads.emplace_back([this] { housekeeping(); });
        for (size_t i = 0; i < cfg.watch.size(); ++i) {
            threads.emplace_back([this, i] { watcher(i); });
            request_scan(i, cfg.watch[i].path);
        }
        log.info("已启动");
        return true;
    }

    void stop() {
        if (!stop_event) return;
        log.info("正在停止：等待正在压缩的文件完成");
        SetEvent(stop_event);
        {
            std::lock_guard<std::mutex> lk(qmu);
            stopping = true;
        }
        {
            std::lock_guard<std::mutex> lk(smu);
        }
        qcv.notify_all();
        scv.notify_all();
        for (auto& t : threads) t.join();
        threads.clear();
        iis_pool_destroy(pool);  // 丢弃排队中的文件，等正在处理的完成；原文件都还在，下次启动会重新发现
        pool = nullptr;
        if (storage) {  // 停机过程中做完的文件：照常记入索引与审计
            std::vector<Done> ds;
            {
                std::lock_guard<std::mutex> lk(qmu);
                ds.swap(dones);
            }
            {
                Index::Tx tx(index);
                for (auto& d : ds) finish(d);
            }
            if (!pending_audit.empty()) audit.append(pending_audit);
            pending_audit.clear();
            audit.append("stop", "", {{"compressed", cnt.compressed.load()}, {"errors", cnt.errors.load()},
                                      {"deleted", storage->deleted()}});
        }
        iis_raw_rules_free(rules);
        rules = nullptr;
        CloseHandle(stop_event);
        stop_event = nullptr;
        log.info("已停止");
    }
};

Engine::Engine(const Config& cfg, Log& log) : p_(std::make_unique<Impl>(cfg, log)) {}
Engine::~Engine() { stop(); }
bool Engine::start() {
    if (p_->start()) return true;
    p_->stop();
    return false;
}
void Engine::stop() { p_->stop(); }
Snapshot Engine::snapshot() const { return p_->snap(); }
std::string Engine::status_json() { return p_->status().dump(1); }
void Engine::set_licensed(bool ok) {
    p_->licensed = ok;
    p_->qcv.notify_one();
}

}  // namespace iis::svc
