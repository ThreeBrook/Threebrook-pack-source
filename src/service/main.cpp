// iis-service：Windows 后台压缩服务。
//
//   iis-service run --config 配置.json          控制台运行（调试；Ctrl+C 停止）
//   iis-service check --config 配置.json        只检查配置
//   iis-service install --config 配置.json [--name IIS]   注册为开机自启服务（需管理员），崩溃自动重启
//   iis-service uninstall [--name IIS]
//   iis-service service --config 配置.json      由服务控制管理器启动（不要手动运行）
//   iis-service export --config 配置.json --out 目录 [--from 日期] [--to 日期] [--station 通配] [--product 通配]
//                [--batch 通配] [--verdict OK|NG|?] [--root 监控目录] [--restore]
//   iis-service audit-verify --config 配置.json  校验审计日志哈希链
//   iis-service stats --config 配置.json         用量、等效容量、压缩比、每日吞吐
//   iis-service set-password --config 配置.json  设置 / 重设管理界面的管理员密码（忘记密码时用）
//   iis-service machine-code                     本机机器码（申请授权用）
//   iis-service license --config 配置.json        授权状态
//   iis-service init-config --config 配置.json [--watch "D:\图像;E:\图像2"] [--port 8686] [--from 预置配置.json]
//               [--license 授权.lic]               安装包调用：配置不存在时生成（已存在保持不变），导入授权
//   iis-service ui [--config 配置.json]            在浏览器打开本机管理界面（开始菜单快捷方式）
//   iis-service configure-service [--name IIS]   安装包调用：设置崩溃自动重启与延迟启动
#include "audit.hpp"
#include "config.hpp"
#include "engine.hpp"
#include "host.hpp"
#include "index.hpp"
#include "license.hpp"
#include "log.hpp"
#include "storage.hpp"

#include "iis/iis.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <shellapi.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ws2_32.lib")

using namespace iis::svc;

namespace {

const wchar_t* kDefaultName = L"IIS";

std::wstring widen(const std::string& s) {
    std::wstring w(s.size() + 1, L'\0');
    w.resize(size_t(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), int(w.size()))));
    return w;
}

std::string narrow(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

struct Args {
    std::string mode, config;
    std::wstring name = kDefaultName;
    std::map<std::string, std::string> opt;  // 其他 --键 值
    bool restore = false;
};

// 命令行按 UTF-16 取，路径里有中文也不会乱
bool parse_args(Args& a) {
    int n = 0;
    LPWSTR* v = CommandLineToArgvW(GetCommandLineW(), &n);
    std::vector<std::wstring> args(v, v + n);
    LocalFree(v);
    if (n < 2) return false;
    a.mode = narrow(args[1]);
    for (int i = 2; i < n; ++i) {
        if (args[i] == L"--config" && i + 1 < n) a.config = narrow(args[++i]);
        else if (args[i] == L"--name" && i + 1 < n) a.name = args[++i];
        else if (args[i] == L"--restore") a.restore = true;
        else if (args[i].rfind(L"--", 0) == 0) {
            // --键 值；后面没有值（或紧跟另一个 --键）时是开关
            const std::string key = narrow(args[i].substr(2));
            const bool has_value = i + 1 < n && args[i + 1].rfind(L"--", 0) != 0;
            a.opt[key] = has_value ? narrow(args[++i]) : "1";
        }
        else return false;
    }
    return true;
}

void usage() {
    std::printf(
        "用法：\n"
        "  iis-service run --config 配置.json        控制台运行（Ctrl+C 停止）\n"
        "  iis-service check --config 配置.json      检查配置\n"
        "  iis-service install --config 配置.json [--name IIS]   注册服务（需管理员）\n"
        "  iis-service uninstall [--name IIS]        删除服务\n"
        "  iis-service export --config 配置.json --out 目录 [--from 2026-11-01] [--to 2026-11-02] [--station 通配]\n"
        "               [--product 通配] [--batch 通配] [--verdict OK|NG|?] [--root 监控目录] [--restore]\n"
        "                                            导出（--restore 还原为原始格式，否则复制 .iis）\n"
        "  iis-service audit-verify --config 配置.json   校验审计日志\n"
        "  iis-service stats --config 配置.json      用量与吞吐统计\n"
        "  iis-service set-password --config 配置.json   设置 / 重设管理界面的管理员密码\n"
        "  iis-service machine-code                  本机机器码（申请授权用）\n"
        "  iis-service license --config 配置.json    授权状态\n"
        "  iis-service init-config --config 配置.json [--watch 目录;目录2] [--port 8686] [--from 预置配置.json] [--license 授权.lic]\n"
        "                                            生成配置（已存在则保持不变）并导入授权（安装包调用）\n"
        "  iis-service ui [--config 配置.json]       在浏览器打开本机管理界面\n"
        "  iis-service configure-service [--name IIS]   设置服务自动重启与延迟启动（安装包调用，需管理员）\n");
}

std::string data_dir(const Config& c) { return c.data_dir.empty() ? c.log_dir + "\\data" : c.data_dir; }

// YYYY-MM-DD（本地时间 0 点）→ Unix 毫秒；格式不对返回 false
bool parse_day(const std::string& s, int64_t& ms) {
    std::tm tm{};
    if (sscanf_s(s.c_str(), "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) != 3) return false;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    tm.tm_isdst = -1;
    const std::time_t t = std::mktime(&tm);
    if (t == -1) return false;
    ms = int64_t(t) * 1000;
    return true;
}

std::string gb(uint64_t b) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f GB", double(b) / 1e9);
    return buf;
}

int cmd_export(const Config& cfg, const Args& a) {
    const auto get = [&](const char* k) { return a.opt.count(k) ? a.opt.at(k) : std::string(); };
    if (get("out").empty()) {
        std::printf("export 需要 --out 目录\n");
        return 2;
    }
    Filter f;
    if (!get("from").empty() && !parse_day(get("from"), f.from_ms)) return std::printf("--from 日期格式应为 YYYY-MM-DD\n"), 2;
    if (!get("to").empty()) {
        if (!parse_day(get("to"), f.to_ms)) return std::printf("--to 日期格式应为 YYYY-MM-DD\n"), 2;
        f.to_ms += 86400000;  // 含当天
    }
    f.station = get("station");
    f.product = get("product");
    f.batch = get("batch");
    f.verdict = get("verdict");
    f.root = get("root");
    Index index;
    Audit audit;
    std::string err;
    if (!index.open(data_dir(cfg) + "\\index.db", err) || !audit.open(data_dir(cfg) + "\\audit", err)) {
        std::printf("%s\n", err.c_str());
        return 1;
    }
    const auto r = export_files(index, audit, f, get("out"), a.restore,
                                [](const std::string& e) { std::printf("失败：%s\n", e.c_str()); });
    std::printf("导出 %llu 个文件（%s），失败 %llu；已记审计\n", (unsigned long long)r.files, gb(r.bytes).c_str(),
                (unsigned long long)r.failed);
    return r.failed ? 1 : 0;
}

int cmd_audit_verify(const Config& cfg) {
    const auto v = Audit::verify(data_dir(cfg) + "\\audit");
    if (v.ok) {
        std::printf("审计日志完整：%llu 个文件，%llu 条记录，最后一条哈希 %s\n", (unsigned long long)v.files,
                    (unsigned long long)v.entries, v.last_hash.c_str());
        return 0;
    }
    std::printf("审计日志校验失败：%s\n", v.error.c_str());
    return 1;
}

// 读一行密码：控制台上不回显
std::string read_secret(const char* prompt) {
    std::printf("%s", prompt);
    std::fflush(stdout);
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    std::string out;
    if (GetConsoleMode(in, &mode)) {
        SetConsoleMode(in, mode & ~DWORD(ENABLE_ECHO_INPUT));
        wchar_t buf[512];
        DWORD n = 0;
        ReadConsoleW(in, buf, 511, &n, nullptr);
        SetConsoleMode(in, mode);
        std::wstring w(buf, n);
        while (!w.empty() && (w.back() == L'\n' || w.back() == L'\r')) w.pop_back();
        out = narrow(w);
        std::printf("\n");
    } else {
        char c;
        DWORD n = 0;
        while (ReadFile(in, &c, 1, &n, nullptr) && n == 1 && c != '\n') out += c;
        if (!out.empty() && out.back() == '\r') out.pop_back();
    }
    return out;
}

int cmd_set_password(const Config& cfg, const std::string& path) {
    const std::string a = read_secret("新的管理员密码（至少 6 个字符）："), b = read_secret("再输一次：");
    if (a != b) return std::printf("两次输入不一致，未修改\n"), 1;
    if (a.size() < 6) return std::printf("密码至少 6 个字符，未修改\n"), 1;
    std::string backup, err;
    if (!write_password(path, make_password(a), backup, err)) return std::printf("%s\n", err.c_str()), 1;
    Audit audit;
    if (audit.open(data_dir(cfg) + "\\audit", err))
        audit.append("password", "", {{"source", "命令行"}, {"first", cfg.ui_password.empty()}, {"backup", backup}});
    std::printf("管理员密码已%s（原配置备份为 %s）；正在运行的服务立即按新密码校验\n",
                cfg.ui_password.empty() ? "设置" : "重设", backup.c_str());
    return 0;
}

int cmd_stats(const Config& cfg) {
    Index index;
    std::string err;
    if (!index.open(data_dir(cfg) + "\\index.db", err)) {
        std::printf("%s\n", err.c_str());
        return 1;
    }
    for (const auto& kv : index.all_totals()) {
        const Totals& t = kv.second;
        std::printf("%s：%llu 个文件，物理占用 %s，等效 %s（%.2f 倍）\n", kv.first.c_str(), (unsigned long long)t.files,
                    gb(t.used).c_str(), gb(t.equiv).c_str(), t.used ? double(t.equiv) / double(t.used) : 0.0);
    }
    for (const auto& kv : index.verdict_counts())
        std::printf("  %s：%llu\n", kv.first == "?" ? "未知" : kv.first.c_str(), (unsigned long long)kv.second);
    std::printf("最近每日（日期 / 压缩文件数 / 原始 → 存储 / 删除）：\n");
    for (const auto& d : index.daily(14))
        std::printf("  %s  %llu  %s → %s  删除 %llu（%s）\n", d.day.c_str(), (unsigned long long)d.files,
                    gb(d.bytes_in).c_str(), gb(d.bytes_out).c_str(), (unsigned long long)d.deleted,
                    gb(d.deleted_bytes).c_str());
    return 0;
}

HANDLE g_stop = nullptr;

BOOL WINAPI on_console(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        SetEvent(g_stop);
        return TRUE;
    }
    return FALSE;
}

// 配置有误时引擎不启动，但管理界面照常运行，可以在界面里改正（改正后自动重载）
int run_console(const std::string& config_path) {
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(on_console, TRUE);
    Host host(config_path, true);
    if (!host.start()) host.log().error(host.error() + "；管理界面仍可用于修改配置");
    WaitForSingleObject(g_stop, INFINITE);
    host.stop();
    return 0;
}

// ---- 服务模式 ----
std::string g_config;
std::wstring g_name;
SERVICE_STATUS_HANDLE g_status_handle = nullptr;
SERVICE_STATUS g_status{};

void set_state(DWORD state, DWORD exit_code = NO_ERROR, DWORD wait_ms = 0) {
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    g_status.dwWin32ExitCode = exit_code ? ERROR_SERVICE_SPECIFIC_ERROR : NO_ERROR;
    g_status.dwServiceSpecificExitCode = exit_code;
    g_status.dwWaitHint = wait_ms;
    ++g_status.dwCheckPoint;
    SetServiceStatus(g_status_handle, &g_status);
}

DWORD WINAPI on_control(DWORD ctrl, DWORD, void*, void*) {
    if (ctrl == SERVICE_CONTROL_STOP || ctrl == SERVICE_CONTROL_SHUTDOWN) {
        set_state(SERVICE_STOP_PENDING, NO_ERROR, 60000);  // 等正在压缩的文件做完
        SetEvent(g_stop);
        return NO_ERROR;
    }
    return ctrl == SERVICE_CONTROL_INTERROGATE ? NO_ERROR : ERROR_CALL_NOT_IMPLEMENTED;
}

void WINAPI service_main(DWORD, LPWSTR*) {
    g_status_handle = RegisterServiceCtrlHandlerExW(g_name.c_str(), on_control, nullptr);
    if (!g_status_handle) return;
    set_state(SERVICE_START_PENDING, NO_ERROR, 120000);  // 启动恢复可能要扫描较大的目录
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    {
        // 引擎起不来（如配置有误）时服务仍保持运行：管理界面可用于改正配置，改正后自动重载
        Host host(g_config, false);
        if (!host.start()) host.log().error(host.error() + "；管理界面仍可用于修改配置");
        set_state(SERVICE_RUNNING);
        WaitForSingleObject(g_stop, INFINITE);
        host.stop();
    }
    set_state(SERVICE_STOPPED);
}

// 服务描述、崩溃自动重启、延迟启动。install 与安装包（configure-service）共用
bool configure(SC_HANDLE svc) {
    SERVICE_DESCRIPTIONW desc{const_cast<wchar_t*>(L"后台无损压缩视觉软件保存的图像；读取用 IIS 查看器或命令行还原。")};
    bool ok = ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc) != 0;
    // 崩溃或异常退出后自动重启：5 秒、10 秒、之后每 60 秒；一天无故障后计数清零
    SC_ACTION acts[3] = {{SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 10000}, {SC_ACTION_RESTART, 60000}};
    SERVICE_FAILURE_ACTIONSW fa{};
    fa.dwResetPeriod = 86400;
    fa.cActions = 3;
    fa.lpsaActions = acts;
    ok = ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa) && ok;
    SERVICE_FAILURE_ACTIONS_FLAG flag{TRUE};  // 以非 0 退出码停止也算失败
    ok = ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &flag) && ok;
    SERVICE_DELAYED_AUTO_START_INFO delayed{TRUE};  // 开机延迟启动：不与视觉软件抢启动资源
    ok = ChangeServiceConfig2W(svc, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &delayed) && ok;
    return ok;
}

int install(const Args& a) {
    if (a.config.empty()) {
        std::printf("install 需要 --config\n");
        return 2;
    }
    wchar_t exe[32768];
    GetModuleFileNameW(nullptr, exe, 32768);
    const std::wstring cfg = std::filesystem::absolute(std::filesystem::u8path(a.config)).wstring();
    const std::wstring cmd = L"\"" + std::wstring(exe) + L"\" service --config \"" + cfg + L"\" --name \"" + a.name + L"\"";
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        std::printf("无法打开服务管理器（错误 %lu），需要以管理员身份运行\n", GetLastError());
        return 1;
    }
    SC_HANDLE svc = CreateServiceW(scm, a.name.c_str(), L"IIS 工业图像压缩存储", SERVICE_ALL_ACCESS,
                                   SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, cmd.c_str(),
                                   nullptr, nullptr, nullptr, nullptr, nullptr);
    if (!svc) {
        std::printf("创建服务失败（错误 %lu）\n", GetLastError());
        CloseServiceHandle(scm);
        return 1;
    }
    if (!configure(svc)) std::printf("注意：设置自动重启 / 延迟启动失败（错误 %lu）\n", GetLastError());
    std::printf("已注册服务 %s；用 sc start %s 启动\n", narrow(a.name).c_str(), narrow(a.name).c_str());
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return 0;
}

// 安装包调用：MSI 登记服务后补上崩溃自动重启与延迟启动（MSI 自带的 ServiceConfig 表文档注明“不能按预期工作”）
int configure_service(const Args& a) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        std::printf("无法打开服务管理器（错误 %lu）\n", GetLastError());
        return 1;
    }
    SC_HANDLE svc = OpenServiceW(scm, a.name.c_str(), SERVICE_CHANGE_CONFIG | SERVICE_START);
    if (!svc) {
        std::printf("找不到服务 %s（错误 %lu）\n", narrow(a.name).c_str(), GetLastError());
        CloseServiceHandle(scm);
        return 1;
    }
    const bool ok = configure(svc);
    if (ok) std::printf("已设置服务 %s：延迟自动启动，异常退出后 5 秒 / 10 秒 / 60 秒自动重启\n", narrow(a.name).c_str());
    else std::printf("设置服务 %s 失败（错误 %lu）\n", narrow(a.name).c_str(), GetLastError());
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok ? 0 : 1;
}

int uninstall(const Args& a) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        std::printf("无法打开服务管理器（错误 %lu）\n", GetLastError());
        return 1;
    }
    SC_HANDLE svc = OpenServiceW(scm, a.name.c_str(), SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
    if (!svc) {
        std::printf("找不到服务（错误 %lu）\n", GetLastError());
        CloseServiceHandle(scm);
        return 1;
    }
    SERVICE_STATUS st{};
    ControlService(svc, SERVICE_CONTROL_STOP, &st);
    for (int i = 0; i < 120 && QueryServiceStatus(svc, &st) && st.dwCurrentState != SERVICE_STOPPED; ++i) Sleep(500);
    const BOOL ok = DeleteService(svc);
    std::printf(ok ? "已删除服务（图像与 .iis 文件不受影响）\n" : "删除服务失败（错误 %lu）\n", GetLastError());
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok ? 0 : 1;
}


std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '"')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '"')) --e;
    return s.substr(b, e - b);
}

std::string read_file(const std::filesystem::path& p, bool& ok) {
    std::ifstream f(p, std::ios::binary);
    ok = bool(f);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// 先写临时文件再改名：断电时要么是旧文件、要么是完整的新文件
bool write_atomic(const std::filesystem::path& p, const std::string& text) {
    const auto tmp = std::filesystem::path(p).concat(L".tmp");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << text;
        f.flush();
        if (!f) return false;
    }
    return MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

// 安装包调用（以 LocalSystem 运行）。配置文件已存在时绝不修改（升级保留配置与索引）；
// 不存在时按预置配置（--from）或参数（--watch、--port）生成。--license 只在授权对本机有效时才放到位（旧的先备份）。
int cmd_init_config(const Args& a) {
    namespace fs = std::filesystem;
    if (a.config.empty()) {
        std::printf("init-config 需要 --config\n");
        return 2;
    }
    auto opt = [&](const char* k) {
        const auto i = a.opt.find(k);
        return i == a.opt.end() ? std::string() : trim(i->second);
    };
    const std::string from = opt("from"), watch = opt("watch"), port_text = opt("port"), lic = opt("license");
    for (const std::string& f : {from, lic}) {
        if (!f.empty() && !fs::u8path(f).is_absolute()) {
            std::printf("安装参数里的文件必须写绝对路径：%s\n", f.c_str());
            return 2;
        }
    }
    const fs::path cfg = fs::absolute(fs::u8path(a.config));
    std::error_code ec;
    fs::create_directories(cfg.parent_path(), ec);
    const auto base8 = cfg.parent_path().u8string();
    const std::string base(base8.begin(), base8.end());
    if (fs::exists(cfg, ec)) {
        std::printf("已有配置 %s，保持不变（升级与重装都保留原配置、索引与审计日志）\n", narrow(cfg.wstring()).c_str());
        if (!from.empty() || !watch.empty() || (!port_text.empty() && port_text != "8686"))
            std::printf("注意：已有配置时忽略 CONFIG_FILE / WATCH / UI_PORT 参数；请在管理界面修改配置\n");
    } else {
        std::string text;
        if (!from.empty()) {
            bool ok = false;
            text = read_file(fs::u8path(from), ok);
            if (!ok) {
                std::printf("读不到预置配置：%s（安装以 LocalSystem 运行，请放在本机磁盘上）\n", from.c_str());
                return 1;
            }
            if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
        } else {
            int port = 8686;
            if (!port_text.empty()) {
                port = std::atoi(port_text.c_str());
                if (port < 1 || port > 65535 || port_text.find_first_not_of("0123456789") != std::string::npos) {
                    std::printf("UI_PORT 应为 1~65535 的整数：%s\n", port_text.c_str());
                    return 2;
                }
            }
            // 写全所有默认值：现场打开配置文件或管理界面就能看到每一项，需要时再改
            const auto defs = nlohmann::ordered_json::parse(defaults_json());
            nlohmann::ordered_json j = defs["config"];
            size_t b = 0;
            while (b <= watch.size()) {
                size_t e = watch.find(';', b);
                if (e == std::string::npos) e = watch.size();
                std::string d = trim(watch.substr(b, e - b));
                while (d.size() > 3 && (d.back() == '\\' || d.back() == '/')) d.pop_back();  // D:\图像\ → D:\图像
                if (!d.empty()) {
                    if (!fs::u8path(d).is_absolute()) {
                        std::printf("监控目录必须写绝对路径：%s\n", d.c_str());
                        return 2;
                    }
                    nlohmann::ordered_json w = {{"path", d}};
                    for (const auto& [k, v] : defs["watch"].items()) w[k] = v;
                    j["watch"].push_back(w);
                }
                b = e + 1;
            }
            j["ui"]["port"] = port;
            text = j.dump(2) + "\n";
        }
        Config c;
        std::string err;
        if (!parse_config(text, base, c, err)) {
            std::printf("配置有误：%s\n", err.c_str());
            return 1;
        }
        for (const auto& w : c.watch)
            if (!fs::is_directory(fs::u8path(w.path), ec))
                std::printf("注意：监控目录目前不存在：%s（服务会等它出现）\n", w.path.c_str());
        if (!write_atomic(cfg, text)) {
            std::printf("写入配置失败：%s\n", narrow(cfg.wstring()).c_str());
            return 1;
        }
        std::printf("已生成配置 %s：%zu 个监控目录%s\n", narrow(cfg.wstring()).c_str(), c.watch.size(),
                    c.watch.empty() ? "（请在管理界面添加）" : "");
    }
    if (!lic.empty()) {
        Config c;
        std::string err;
        if (!load_config(narrow(cfg.wstring()), c, err)) {
            std::printf("配置有误，未导入授权：%s\n", err.c_str());
            return 0;  // 已有配置由用户维护；授权可以之后在管理界面导入
        }
        bool ok = false;
        const std::string text = read_file(fs::u8path(lic), ok);
        if (!ok) {
            std::printf("读不到授权文件：%s；可在安装后从管理界面导入\n", lic.c_str());
            return 0;
        }
        const auto st = iis::lic::evaluate(&text, iis::lic::product_key(), iis::lic::fingerprint(),
                                           int64_t(std::time(nullptr)), 0);
        if (!st.can_compress()) {
            // 不用无效的授权替换现有授权
            std::printf("授权未导入：%s——%s。本机机器码：%s\n", iis::lic::state_name(st.state), st.text.c_str(),
                        iis::lic::machine_code(iis::lic::fingerprint()).c_str());
            return 0;
        }
        const fs::path dst = fs::u8path(c.license_file);
        bool had = false;
        const std::string old = read_file(dst, had);
        if (had && old == text) {
            std::printf("授权已是这一份，不变\n");
            return 0;
        }
        if (had) {
            const fs::path bak = fs::path(dst).concat(L"." + std::to_wstring(std::time(nullptr)) + L".bak");
            fs::copy_file(dst, bak, ec);
            if (ec) {
                std::printf("备份旧授权失败（%s），未导入新授权\n", ec.message().c_str());
                return 0;
            }
        }
        if (!write_atomic(dst, text)) {
            std::printf("写入授权失败：%s\n", narrow(dst.wstring()).c_str());
            return 0;
        }
        std::printf("已导入授权（%s / %s，%s）\n", st.lic.customer.c_str(), st.lic.device.c_str(), st.text.c_str());
    }
    return 0;
}

// 开始菜单“IIS 管理界面”：等管理界面就绪（服务刚启动时可能要几秒）后用默认浏览器打开
int cmd_ui(const Args& a) {
    DWORD pids[2];
    if (GetConsoleProcessList(pids, 2) == 1) FreeConsole();  // 快捷方式启动时控制台窗口是我们自己的：收起
    int port = 8686;  // 与配置读不了时管理界面使用的端口一致
    std::string cfg = a.config;
    if (cfg.empty()) {
        wchar_t buf[MAX_PATH];
        const DWORD n = GetEnvironmentVariableW(L"ProgramData", buf, MAX_PATH);
        if (n && n < MAX_PATH) cfg = narrow(std::wstring(buf) + L"\\IIS\\config.json");
    }
    Config c;
    std::string err;
    if (!cfg.empty() && load_config(cfg, c, err)) {
        if (!c.ui_enabled) {
            MessageBoxW(nullptr, L"本机管理界面已在配置中关闭（ui.enabled = false）。", L"IIS 管理界面", MB_ICONINFORMATION);
            return 1;
        }
        if (c.ui_port > 0) port = c.ui_port;
    }
    const int wait_s = a.opt.count("wait") ? std::atoi(a.opt.at("wait").c_str()) : 30;
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    bool up = false;
    for (int i = 0; i <= wait_s * 2 && !up; ++i) {
        if (i) Sleep(500);
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(uint16_t(port));
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        up = connect(s, reinterpret_cast<sockaddr*>(&sa), sizeof sa) == 0;
        closesocket(s);
    }
    WSACleanup();
    const std::wstring url = L"http://127.0.0.1:" + std::to_wstring(port) + L"/";
    const bool quiet = a.opt.count("no-browser") != 0;  // 测试用：不开浏览器、不弹窗，只看退出码
    if (!up && quiet) std::printf("管理界面没有响应：%s\n", narrow(url).c_str());
    if (!up && !quiet) {
        const std::wstring msg = L"管理界面没有响应（" + url +
                                 L"）。\n\n请确认 Windows 服务“IIS 工业图像压缩存储”正在运行（services.msc）；"
                                 L"服务日志在 %ProgramData%\\IIS\\logs。";
        MessageBoxW(nullptr, msg.c_str(), L"IIS 管理界面", MB_ICONWARNING);
        return 1;
    }
    if (!up) return 1;
    if (!quiet) ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    std::printf("%s\n", narrow(url).c_str());
    return 0;
}

}  // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    Args a;
    if (!parse_args(a)) {
        usage();
        return 2;
    }
    if (a.mode == "uninstall") return uninstall(a);
    if (a.mode == "init-config") return cmd_init_config(a);
    if (a.mode == "configure-service") return configure_service(a);
    if (a.mode == "ui") return cmd_ui(a);
    if (a.mode == "machine-code") {
        std::printf("%s\n", iis::lic::machine_code(iis::lic::fingerprint()).c_str());
        return 0;
    }
    if (a.mode == "run" || a.mode == "service") {
        if (a.config.empty()) {
            std::printf("缺少 --config\n");
            return 2;
        }
        if (a.mode == "run") return run_console(a.config);
        g_config = a.config;
        g_name = a.name;
        SERVICE_TABLE_ENTRYW table[] = {{const_cast<wchar_t*>(g_name.c_str()), service_main}, {nullptr, nullptr}};
        if (!StartServiceCtrlDispatcherW(table)) {
            std::printf("只能由服务控制管理器启动；调试请用 run\n");
            return 1;
        }
        return 0;
    }
    if (a.mode == "install") {
        Config cfg;
        std::string err;
        if (!load_config(a.config, cfg, err)) {
            std::printf("配置有误：%s\n", err.c_str());
            return 2;
        }
        return install(a);
    }
    Config cfg;
    std::string err;
    if (a.config.empty() || !load_config(a.config, cfg, err)) {
        std::printf("配置有误：%s\n", err.empty() ? "缺少 --config" : err.c_str());
        return 2;
    }
    if (a.mode == "check") {
        std::printf("配置正确：%zu 个监控目录\n", cfg.watch.size());
        return 0;
    }
    if (a.mode == "export") return cmd_export(cfg, a);
    if (a.mode == "audit-verify") return cmd_audit_verify(cfg);
    if (a.mode == "stats") return cmd_stats(cfg);
    if (a.mode == "set-password") return cmd_set_password(cfg, a.config);
    if (a.mode == "license") {
        std::ifstream f(std::filesystem::u8path(cfg.license_file), std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        const auto st = iis::lic::evaluate(text.empty() ? nullptr : &text, iis::lic::product_key(), iis::lic::fingerprint(),
                                           int64_t(std::time(nullptr)), 0);
        std::printf("授权：%s——%s\n授权文件：%s\n", iis::lic::state_name(st.state), st.text.c_str(), cfg.license_file.c_str());
        if (st.state != iis::lic::State::None && st.state != iis::lic::State::Invalid)
            std::printf("客户 %s，设备 %s，档位 %s，指纹一致 %d / %d\n", st.lic.customer.c_str(), st.lic.device.c_str(),
                        st.lic.tier.c_str(), st.matched, st.total);
        if (iis::lic::dev_key()) std::printf("（本程序使用开发用授权公钥）\n");
        return st.can_compress() ? 0 : 1;
    }
    usage();
    return 2;
}
