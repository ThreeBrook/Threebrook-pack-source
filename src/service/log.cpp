#include "log.hpp"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <share.h>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace iis::svc {

namespace fs = std::filesystem;

Log::~Log() {
    if (f_) std::fclose(f_);
}

void Log::open(const std::string& dir, int keep_days, bool console) {
    std::lock_guard<std::mutex> lk(mu_);
    if (dir != dir_) {  // 改了日志目录（管理界面重载配置）：下次写入时在新目录打开
        if (f_) std::fclose(f_);
        f_ = nullptr;
        day_.clear();
    }
    dir_ = dir;
    keep_days_ = keep_days;
    console_ = console;
    if (!dir_.empty()) {
        std::error_code ec;
        fs::create_directories(fs::u8path(dir_), ec);
    }
}

void Log::rotate(const std::string& day) {
    if (f_) std::fclose(f_);
    f_ = nullptr;
    day_ = day;
    if (dir_.empty()) return;
    const fs::path p = fs::u8path(dir_) / ("iis-service-" + day + ".log");
    f_ = _wfsopen(p.wstring().c_str(), L"ab", _SH_DENYWR);  // 允许别人（管理界面、记事本）同时读
    // 删除超过保留天数的旧日志（文件名里的日期比较即可）
    std::time_t t = std::time(nullptr) - std::time_t(keep_days_) * 86400;
    std::tm tm{};
    localtime_s(&tm, &t);
    char oldest[16];
    std::strftime(oldest, sizeof oldest, "%Y%m%d", &tm);
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(fs::u8path(dir_), ec)) {
        const auto n = e.path().filename().u8string();
        if (is_log_name(n) &&
            std::string(n.begin() + 12, n.begin() + 20) < oldest)
            fs::remove(e.path(), ec);
    }
}

void Log::write(Level lv, const std::string& msg) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    const int ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    std::tm tm{};
    localtime_s(&tm, &t);
    char day[16], ts[32];
    std::strftime(day, sizeof day, "%Y%m%d", &tm);
    std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    static const char* names[] = {"信息", "警告", "错误"};
    char head[64];
    std::snprintf(head, sizeof head, "%s.%03d [%s] ", ts, ms, names[lv]);
    const std::string line = head + msg + "\n";
    std::lock_guard<std::mutex> lk(mu_);
    if (day_ != day) rotate(day);
    if (f_) {
        std::fwrite(line.data(), 1, line.size(), f_);
        std::fflush(f_);
    }
    if (console_) {
        std::fwrite(line.data(), 1, line.size(), stdout);
        std::fflush(stdout);
    }
}

}  // namespace iis::svc
