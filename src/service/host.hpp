// 服务宿主（T7）：持有配置、日志、压缩引擎与本机管理界面。
// 配置有误时引擎不启动，但管理界面照常提供（便于在界面里改正）。
// 界面改配置走两步：预览（校验 + 差异 + 一次性确认码）→ 确认应用（备份原文件、原子写入、重载引擎、记审计）。
#pragma once

#include "config.hpp"
#include "engine.hpp"
#include "http.hpp"
#include "license.hpp"
#include "log.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace iis::svc {

class Host {
public:
    explicit Host(std::string config_path, bool console);
    ~Host();
    // 启动引擎与管理界面；返回 false 表示引擎没起来（原因见 error()），管理界面仍可能在运行
    bool start();
    void stop();
    bool engine_running();
    std::string error();
    uint16_t ui_port() const { return ui_.port(); }
    Log& log() { return log_; }
    // 管理界面请求处理（测试直接调用）
    HttpResponse handle(const HttpRequest& req);
    const std::string& token() const { return token_; }
    // 测试用：换掉编译进程序的授权公钥（须在 start 之前调用）
    void set_license_key(const lic::Key32& k) { lic_key_ = k; }
    lic::Status license_status();
    void check_license();  // 立即重新判定授权（导入、定时）

private:
    bool start_engine();  // 调用方持有 mu_
    HttpResponse api(const HttpRequest& req);
    HttpResponse status();
    HttpResponse get_config();
    HttpResponse preview(const nlohmann::json& body);
    HttpResponse apply(const nlohmann::json& body);
    HttpResponse logs(const HttpRequest& req);
    HttpResponse audit_list(const HttpRequest& req);
    HttpResponse audit_verify();
    HttpResponse export_start(const nlohmann::json& body);
    HttpResponse export_state();
    HttpResponse license_get();
    HttpResponse license_import(const nlohmann::json& body);
    HttpResponse browse(const nlohmann::json& body);
    std::string license_path();
    std::string data_dir();
    int64_t high_water();
    void audit(const std::string& act, const nlohmann::json& detail);
    HttpResponse login(const nlohmann::json& body);
    HttpResponse logout(const HttpRequest& req);
    HttpResponse set_password(const HttpRequest& req, const nlohmann::json& body);
    PasswordHash file_password();  // 每次从配置文件读（命令行重设密码后立即生效）
    bool authed(const HttpRequest& req);

    std::string path_, base_dir_;
    bool console_;
    std::mutex mu_;  // 配置与引擎
    Config cfg_;
    bool cfg_ok_ = false;
    std::string error_;
    Log log_;
    std::unique_ptr<Engine> engine_;
    HttpServer ui_;
    std::string token_;

    struct Pending {
        std::string token, config_text, rules_text, rules_path;
        bool rules = false;
        nlohmann::json changed;
        std::chrono::steady_clock::time_point at;
    } pending_;

    std::mutex auth_mu_;  // 登录会话
    std::map<std::string, std::chrono::steady_clock::time_point> sessions_;
    int fails_ = 0;
    std::chrono::steady_clock::time_point locked_until_{};

    // 授权：本机指纹启动时取一次；每分钟判定一次（防系统时间回拨的高水位存于 data/clock.json）
    lic::Key32 lic_key_ = lic::product_key();
    std::vector<lic::FpItem> fp_;
    std::mutex lic_mu_;
    lic::Status lic_status_;
    std::thread lic_thread_;
    std::condition_variable lic_cv_;
    bool lic_stop_ = false;

    std::mutex ex_mu_;  // 导出任务
    std::thread ex_thread_;
    nlohmann::json ex_state_ = {{"running", false}};
};

// 把管理员密码写进配置文件（先备份，原子替换）；配置文件须能解析
bool write_password(const std::string& config_path, const PasswordHash& h, std::string& backup, std::string& err);

// 两段文本按行比较：[{"op":"-|+| ","line":…}]（相同行太多时只保留改动附近 3 行）
nlohmann::json line_diff(const std::string& a, const std::string& b);

}  // namespace iis::svc
