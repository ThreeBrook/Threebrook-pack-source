// 服务日志：按天分文件（UTF-8），保留若干天；控制台模式同时输出到屏幕。线程安全。
#pragma once

#include <cstdio>
#include <mutex>
#include <string>

namespace iis::svc {

// 日志文件名：iis-service-YYYYMMDD.log
inline bool is_log_name(const std::string& n) {
    if (n.size() != 24 || n.compare(0, 12, "iis-service-") != 0 || n.compare(20, 4, ".log") != 0) return false;
    for (size_t i = 12; i < 20; ++i)
        if (n[i] < '0' || n[i] > '9') return false;
    return true;
}

class Log {
public:
    enum Level { INFO, WARN, ERR };
    // dir 为空时只输出到控制台
    void open(const std::string& dir, int keep_days, bool console);
    void write(Level lv, const std::string& msg);
    void info(const std::string& m) { write(INFO, m); }
    void warn(const std::string& m) { write(WARN, m); }
    void error(const std::string& m) { write(ERR, m); }
    ~Log();

private:
    void rotate(const std::string& day);
    std::mutex mu_;
    std::string dir_, day_;
    int keep_days_ = 30;
    bool console_ = false;
    std::FILE* f_ = nullptr;
};

}  // namespace iis::svc
