#include "host.hpp"

#include "audit.hpp"
#include "auth.hpp"
#include "license.hpp"
#include "index.hpp"
#include "storage.hpp"

#include "iis/iis.h"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <random>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace iis::svc {

namespace fs = std::filesystem;
using nlohmann::json;

extern const unsigned char kIndexHtml[];  // ui_embed.cpp（构建时由 ui/index.html 生成）
extern const size_t kIndexHtmlSize;

namespace {

HttpResponse js(const json& j, int status = 200) {
    return {status, "application/json; charset=utf-8", j.dump(-1, ' ', false, json::error_handler_t::replace)};
}
HttpResponse fail(int status, const std::string& msg) { return js({{"ok", false}, {"error", msg}}, status); }

std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::string t((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (t.size() >= 3 && t.compare(0, 3, "\xEF\xBB\xBF") == 0) t.erase(0, 3);
    return t;
}

std::string random_hex(size_t bytes) {
    std::random_device rd;
    static const char* x = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < bytes; ++i) {
        const unsigned v = rd() & 0xFF;
        s += x[v >> 4], s += x[v & 15];
    }
    return s;
}

std::string stamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tm);
    return buf;
}

// 备份后原子替换：先写临时文件并落盘，再改名覆盖
bool write_atomic(const fs::path& p, const std::string& text, std::string& backup, std::string& err) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    if (fs::exists(p)) {
        const fs::path b = p.wstring() + L".bak-" + fs::u8path(stamp()).wstring();
        if (!CopyFileW(p.wstring().c_str(), b.wstring().c_str(), FALSE)) {
            err = "无法备份 " + p.u8string();
            return false;
        }
        backup = b.u8string();
        // 只保留最近 20 个备份
        std::vector<fs::path> baks;
        for (const auto& e : fs::directory_iterator(p.parent_path(), ec))
            if (e.path().filename().wstring().rfind(p.filename().wstring() + L".bak-", 0) == 0) baks.push_back(e.path());
        std::sort(baks.begin(), baks.end());
        for (size_t i = 0; i + 20 < baks.size(); ++i) fs::remove(baks[i], ec);
    }
    const std::wstring tmp = p.wstring() + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD n = 0;
    const bool ok = h != INVALID_HANDLE_VALUE && WriteFile(h, text.data(), DWORD(text.size()), &n, nullptr) &&
                    n == text.size() && FlushFileBuffers(h);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), p.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        err = "无法写入 " + p.u8string() + "（错误 " + std::to_string(GetLastError()) + "）";
        return false;
    }
    return true;
}

std::string pretty(const std::string& text) {
    const json j = json::parse(text, nullptr, false);
    return j.is_discarded() ? text : j.dump(2, ' ', false, json::error_handler_t::replace);
}

std::vector<std::string> lines(const std::string& t) {
    std::vector<std::string> v;
    size_t i = 0;
    while (i < t.size()) {
        size_t e = t.find('\n', i);
        if (e == std::string::npos) e = t.size();
        std::string l = t.substr(i, e - i);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        v.push_back(l);
        i = e + 1;
    }
    return v;
}

bool inside(const std::string& path, const std::string& dir) {
    std::error_code ec;
    const auto a = fs::weakly_canonical(fs::u8path(path), ec).wstring();
    auto b = fs::weakly_canonical(fs::u8path(dir), ec).wstring();
    if (b.empty()) return false;
    auto low = [](std::wstring s) {
        for (auto& c : s) c = towlower(c);
        return s;
    };
    const std::wstring la = low(a) + L"\\", lb = low(b) + (b.back() == L'\\' ? L"" : L"\\");
    return la.rfind(lb, 0) == 0;
}

}  // namespace

bool write_password(const std::string& config_path, const PasswordHash& h, std::string& backup, std::string& err) {
    const fs::path p = fs::u8path(config_path);
    json j = json::parse(read_text(p), nullptr, false);
    if (!j.is_object()) {
        err = "配置文件无法解析，请先改好";
        return false;
    }
    if (!j.contains("ui") || !j["ui"].is_object()) j["ui"] = json::object();
    j["ui"]["password"] = {{"salt", h.salt}, {"hash", h.hash}, {"iterations", h.iterations}};
    const std::string text = j.dump(2) + "\n";
    Config check;
    if (!parse_config(text, fs::absolute(p).parent_path().u8string(), check, err)) return false;
    return write_atomic(p, text, backup, err);
}

json line_diff(const std::string& a, const std::string& b) {
    const auto x = lines(a), y = lines(b);
    const size_t n = x.size(), m = y.size();
    std::vector<std::vector<uint32_t>> L(n + 1, std::vector<uint32_t>(m + 1, 0));
    for (size_t i = n; i-- > 0;)
        for (size_t j = m; j-- > 0;) L[i][j] = x[i] == y[j] ? L[i + 1][j + 1] + 1 : std::max(L[i + 1][j], L[i][j + 1]);
    std::vector<std::pair<char, std::string>> ops;
    size_t i = 0, j = 0;
    while (i < n || j < m) {
        if (i < n && j < m && x[i] == y[j]) ops.push_back({' ', x[i]}), ++i, ++j;
        else if (j < m && (i == n || L[i][j + 1] >= L[i + 1][j])) ops.push_back({'+', y[j]}), ++j;
        else ops.push_back({'-', x[i]}), ++i;
    }
    // 只保留改动及前后 3 行
    std::vector<bool> keep(ops.size(), false);
    for (size_t k = 0; k < ops.size(); ++k)
        if (ops[k].first != ' ')
            for (size_t d = k >= 3 ? k - 3 : 0; d <= std::min(ops.size() - 1, k + 3); ++d) keep[d] = true;
    json out = json::array();
    bool gap = false;
    for (size_t k = 0; k < ops.size(); ++k) {
        if (!keep[k]) {
            gap = true;
            continue;
        }
        if (gap && !out.empty()) out.push_back({{"op", "…"}, {"line", ""}});
        gap = false;
        out.push_back({{"op", std::string(1, ops[k].first)}, {"line", ops[k].second}});
    }
    return out;
}

Host::Host(std::string config_path, bool console) : path_(std::move(config_path)), console_(console) {
    const auto abs = fs::absolute(fs::u8path(path_));
    path_ = abs.u8string();
    base_dir_ = abs.parent_path().u8string();
    token_ = random_hex(16);
}

Host::~Host() { stop(); }

bool Host::start_engine() {
    engine_.reset();
    std::string err;
    Config c;
    if (!load_config(path_, c, err)) {
        error_ = "配置有误：" + err;
        log_.error(error_);
        return false;
    }
    cfg_ = c;
    cfg_ok_ = true;
    log_.open(cfg_.log_dir, cfg_.log_keep_days, console_);
    auto e = std::make_unique<Engine>(cfg_, log_);
    if (!e->start()) {
        error_ = "引擎启动失败，详见日志";
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(lic_mu_);
        e->set_licensed(lic_status_.can_compress());
    }
    engine_ = std::move(e);
    error_.clear();
    return true;
}

bool Host::start() {
    fp_ = lic::fingerprint();
    {
        std::lock_guard<std::mutex> lk(mu_);  // 先读出配置里的授权文件位置
        Config c;
        std::string err;
        if (load_config(path_, c, err)) cfg_ = c, cfg_ok_ = true;
    }
    check_license();
    bool ok;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!fs::exists(fs::u8path(path_))) {
            error_ = "配置文件不存在：" + path_;
        }
        ok = start_engine();
        if (!cfg_ok_) log_.open((fs::u8path(base_dir_) / "logs").u8string(), 30, console_);
    }
    {
        const lic::Status st = license_status();
        if (st.can_compress()) log_.info(std::string("授权：") + st.text + "（" + st.lic.customer + " / " + st.lic.device + "）");
        else log_.warn(std::string("授权：") + lic::state_name(st.state) + "——" + st.text + "；不压缩新文件（已压缩文件的查看、还原、导出不受影响）");
        if (lic::dev_key()) log_.warn("本程序使用开发用授权公钥，不得用于正式发布");
    }
    lic_thread_ = std::thread([this] {
        std::unique_lock<std::mutex> lk(lic_mu_);
        while (!lic_cv_.wait_for(lk, std::chrono::seconds(60), [&] { return lic_stop_; })) {
            lk.unlock();
            check_license();
            lk.lock();
        }
    });
    const bool ui = cfg_ok_ ? cfg_.ui_enabled : true;  // 配置读不了时也开界面（默认端口），方便修好它
    const int port = cfg_ok_ ? cfg_.ui_port : 8686;
    if (ui) {
        std::string err;
        if (ui_.start(uint16_t(port), [this](const HttpRequest& r) { return handle(r); }, err)) {
            log_.info("本机管理界面：http://127.0.0.1:" + std::to_string(ui_.port()) + "/");
        } else {
            log_.warn(err);
        }
    }
    return ok;
}

void Host::stop() {
    ui_.stop();
    {
        std::lock_guard<std::mutex> lk(lic_mu_);
        lic_stop_ = true;
    }
    lic_cv_.notify_all();
    if (lic_thread_.joinable()) lic_thread_.join();
    {
        std::lock_guard<std::mutex> lk(ex_mu_);
    }
    if (ex_thread_.joinable()) ex_thread_.join();
    std::lock_guard<std::mutex> lk(mu_);
    engine_.reset();
}

bool Host::engine_running() {
    std::lock_guard<std::mutex> lk(mu_);
    return engine_ != nullptr;
}

std::string Host::error() {
    std::lock_guard<std::mutex> lk(mu_);
    return error_;
}

HttpResponse Host::handle(const HttpRequest& req) {
    // 只接受本机浏览器直接访问：Host 头必须是本机地址（防 DNS 重绑定），跨站来源一律拒绝
    const std::string port = std::to_string(ui_.port());
    const auto h = req.headers.find("host");
    const std::string host = h == req.headers.end() ? "" : h->second;
    if (host != "127.0.0.1:" + port && host != "localhost:" + port) return fail(403, "只允许本机访问");
    const auto o = req.headers.find("origin");
    if (o != req.headers.end() && o->second != "http://127.0.0.1:" + port && o->second != "http://localhost:" + port)
        return fail(403, "拒绝跨站请求");
    if (req.method == "GET" && (req.path == "/" || req.path == "/index.html"))
        return {200, "text/html; charset=utf-8", std::string(reinterpret_cast<const char*>(kIndexHtml), kIndexHtmlSize)};
    if (req.path.rfind("/api/", 0) == 0) return api(req);
    return fail(404, "没有这个页面");
}

HttpResponse Host::api(const HttpRequest& req) {
    if (req.method == "GET" && req.path == "/api/session")
        return js({{"token", token_}, {"version", iis_version()}, {"has_password", !file_password().empty()},
                   {"logged_in", authed(req)}});
    if (req.method == "POST") {
        // 改动类请求：必须带会话码（别的网页拿不到）且为 JSON
        const auto t = req.headers.find("x-iis-token");
        if (t == req.headers.end() || t->second != token_) return fail(403, "会话已失效，请刷新页面");
        const auto ct = req.headers.find("content-type");
        if (ct == req.headers.end() || ct->second.find("application/json") == std::string::npos)
            return fail(400, "请求格式不对");
        const json body = json::parse(req.body.empty() ? "{}" : req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object()) return fail(400, "请求不是合法 JSON");
        if (req.path == "/api/login") return login(body);
        if (req.path == "/api/logout") return logout(req);
        if (req.path == "/api/password") return set_password(req, body);
        if (req.path == "/api/audit/verify") return audit_verify();
        // 以下改动配置、导入授权或带走数据：须登录；还没设密码时须先设
        if (!authed(req)) {
            Config probe;
            std::string perr;
            const bool parses = load_config(path_, probe, perr);
            bool known;
            {
                std::lock_guard<std::mutex> lk(mu_);
                known = cfg_ok_ && !cfg_.ui_password.empty();
            }
            // 配置文件坏了且不知道密码：放行修配置（能弄坏配置文件的人本来就能直接改它）
            const bool rescue = !parses && !known && (req.path == "/api/config/preview" || req.path == "/api/config/apply");
            if (!rescue) {
                if (file_password().empty())
                    return js({{"ok", false}, {"code", "no_password"}, {"error", "请先设置管理员密码"}}, 401);
                return js({{"ok", false}, {"code", "login"}, {"error", "请先登录"}}, 401);
            }
        }
        if (req.path == "/api/config/preview") return preview(body);
        if (req.path == "/api/config/apply") return apply(body);
        if (req.path == "/api/export") return export_start(body);
        if (req.path == "/api/license") return license_import(body);
        if (req.path == "/api/browse") return browse(body);
        return fail(404, "没有这个接口");
    }
    if (req.method != "GET") return fail(405, "不支持的请求方法");
    if (req.path == "/api/status") return status();
    if (req.path == "/api/config") return get_config();
    if (req.path == "/api/logs") return logs(req);
    if (req.path == "/api/audit") return audit_list(req);
    if (req.path == "/api/export") return export_state();
    if (req.path == "/api/license") return license_get();
    return fail(404, "没有这个接口");
}

HttpResponse Host::status() {
    std::lock_guard<std::mutex> lk(mu_);
    json j = engine_ ? json::parse(engine_->status_json(), nullptr, false) : json::object();
    j["running"] = engine_ != nullptr;
    j["error"] = error_;
    j["config_path"] = path_;
    j["version"] = iis_version();
    j["build"] = iis_build_info();
    {
        std::lock_guard<std::mutex> lk2(lic_mu_);
        j["license"] = {{"state", lic::state_name(lic_status_.state)}, {"ok", lic_status_.can_compress()},
                        {"text", lic_status_.text}, {"days_left", lic_status_.days_left}, {"dev_key", lic::dev_key()}};
    }
    j["log_dir"] = cfg_ok_ ? cfg_.log_dir : "";
    return js(j);
}

HttpResponse Host::get_config() {
    std::lock_guard<std::mutex> lk(mu_);
    json j = {{"path", path_}, {"base_dir", base_dir_}, {"text", read_text(fs::u8path(path_))}, {"error", error_}};
    // RAW 规则文件：按当前文件里写的路径读（配置有误时尽量从文本里取）
    std::string rules = cfg_ok_ ? cfg_.raw_rules_file : "";
    const json parsed = json::parse(j["text"].get<std::string>(), nullptr, false);
    if (!parsed.is_discarded() && parsed.is_object() && parsed.contains("raw_rules_file") &&
        parsed["raw_rules_file"].is_string()) {
        const auto r = fs::u8path(parsed["raw_rules_file"].get<std::string>());
        rules = (r.is_absolute() ? r : fs::u8path(base_dir_) / r).u8string();
    }
    j["raw_rules_path"] = rules;
    j["raw_rules_text"] = rules.empty() ? "" : read_text(fs::u8path(rules));
    j["defaults"] = json::parse(defaults_json());  // 界面把没写的项显示为默认值
    return js(j);
}

// 选择文件夹（只读，须登录）：path 为空时列出盘符，否则列出其下的子文件夹（不含隐藏的系统文件夹）。
// 列出的是服务（LocalSystem）能看到的目录——也正是服务能监控的目录
HttpResponse Host::browse(const json& body) {
    const std::string path = body.value("path", std::string());
    json dirs = json::array();
    if (path.empty()) {
        const DWORD mask = GetLogicalDrives();
        for (int i = 0; i < 26; ++i) {
            if (!(mask & (1u << i))) continue;
            const std::wstring root = std::wstring(1, wchar_t(L'A' + i)) + L":\\";
            const UINT type = GetDriveTypeW(root.c_str());
            if (type == DRIVE_CDROM || type == DRIVE_NO_ROOT_DIR) continue;
            wchar_t label[MAX_PATH + 1] = L"";
            if (type == DRIVE_FIXED) GetVolumeInformationW(root.c_str(), label, MAX_PATH + 1, nullptr, nullptr, nullptr, nullptr, 0);
            const char* kind = type == DRIVE_REMOVABLE ? "可移动磁盘" : type == DRIVE_REMOTE ? "网络驱动器" : "本地磁盘";
            const std::string r = fs::path(root).u8string();
            dirs.push_back({{"name", r}, {"path", r}, {"note", label[0] ? fs::path(label).u8string() + "（" + kind + "）" : std::string(kind)}});
        }
        return js({{"path", ""}, {"parent", nullptr}, {"dirs", dirs}});
    }
    fs::path p = fs::u8path(path);
    if (!p.is_absolute()) return fail(400, "请填写完整路径，如 D:\\Images");
    p = p.lexically_normal();
    std::wstring w = p.wstring();
    if (w.size() == 2 && w[1] == L':') w += L'\\';  // “D:” → “D:\”
    std::error_code ec;
    if (!fs::is_directory(fs::path(w), ec)) return fail(400, "文件夹不存在或无法访问：" + fs::path(w).u8string());
    std::wstring pattern = w;
    if (pattern.back() != L'\\') pattern += L'\\';
    pattern += L'*';
    if (pattern.rfind(L"\\\\", 0) != 0) pattern = L"\\\\?\\" + pattern;  // 长路径（网络共享原样）
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchLimitToDirectories, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    std::vector<std::wstring> names;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const DWORD a = fd.dwFileAttributes;
            if (!(a & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if ((a & FILE_ATTRIBUTE_HIDDEN) && (a & FILE_ATTRIBUTE_SYSTEM)) continue;  // $RECYCLE.BIN、System Volume Information
            const std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            names.push_back(n);
        } while (names.size() < 5000 && FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) {
        return CompareStringOrdinal(a.c_str(), int(a.size()), b.c_str(), int(b.size()), TRUE) == CSTR_LESS_THAN;
    });
    const fs::path base(w);
    for (const auto& n : names) dirs.push_back({{"name", fs::path(n).u8string()}, {"path", (base / n).u8string()}});
    const fs::path parent = base.parent_path();
    // 盘符根目录的上一级是“此电脑”（盘符列表）
    const json up = base == base.root_path() ? json("") : json(parent.u8string());
    return js({{"path", base.u8string()}, {"parent", up}, {"dirs", dirs}, {"truncated", names.size() >= 5000}});
}

HttpResponse Host::preview(const json& body) {
    if (!body.contains("config") || !body["config"].is_string()) return fail(400, "缺少配置内容");
    std::string text = body["config"].get<std::string>();
    // 管理员密码只能在“管理员密码”页改：提交的配置里没写密码时沿用文件里的（页面可能是设密码之前打开的），写了不同的则拒绝
    {
        auto pw_of = [](const json& j) {
            return j.is_object() && j.contains("ui") && j["ui"].is_object() && j["ui"].contains("password") ? j["ui"]["password"] : json();
        };
        const json cur = json::parse(read_text(fs::u8path(path_)), nullptr, false);
        json next = json::parse(text, nullptr, false);
        if (next.is_object() && !pw_of(cur).is_null()) {
            if (pw_of(next).is_null()) {
                if (!next.contains("ui") || !next["ui"].is_object()) next["ui"] = json::object();
                next["ui"]["password"] = pw_of(cur);
                text = next.dump(2) + "\n";
            } else if (pw_of(next) != pw_of(cur)) {
                return fail(400, "管理员密码不能在配置里改，请到“管理员密码”页修改");
            }
        }
    }
    Config nc;
    std::string err;
    if (!parse_config(text, base_dir_, nc, err)) return fail(400, err);
    json warnings = json::array();
    for (const auto& w : nc.watch) {
        std::error_code ec;
        if (!fs::is_directory(fs::u8path(w.path), ec)) warnings.push_back("监控目录现在不存在：" + w.path + "（服务会每 5 秒重试）");
    }
    const bool rules = body.contains("raw_rules") && body["raw_rules"].is_string();
    std::string rules_text;
    if (rules) {
        rules_text = body["raw_rules"].get<std::string>();
        if (nc.raw_rules_file.empty()) return fail(400, "要保存 RAW 规则，请先填写“RAW 规则文件”路径");
        if (!rules_text.empty()) {
            iis_raw_rules* r = nullptr;
            if (iis_raw_rules_parse(rules_text.data(), rules_text.size(), &r) != IIS_OK)
                return fail(400, std::string("RAW 规则有误：") + iis_last_error());
            iis_raw_rules_free(r);
        }
    }
    std::lock_guard<std::mutex> lk(mu_);
    const std::string old_text = read_text(fs::u8path(path_));
    const json old_j = json::parse(old_text, nullptr, false), new_j = json::parse(text);
    json changed = json::array();
    if (old_j.is_object()) {
        for (auto it = new_j.begin(); it != new_j.end(); ++it)
            if (!old_j.contains(it.key()) || old_j[it.key()] != it.value()) changed.push_back(it.key());
        for (auto it = old_j.begin(); it != old_j.end(); ++it)
            if (!new_j.contains(it.key())) changed.push_back(it.key());
    } else {
        changed.push_back("（原配置无法解析，整体替换）");
    }
    for (const auto& k : changed) {
        const std::string key = k.get<std::string>();
        if (key == "ui") warnings.push_back("管理界面端口的修改要重启服务才生效");
        if (key == "cpu_rate_percent") warnings.push_back("CPU 硬上限的修改要重启服务才生效");
    }
    if (nc.release != (cfg_ok_ ? cfg_.release : true))
        warnings.push_back(nc.release ? "将开始释放（删除）已压缩文件的原文件" : "将不再释放原文件，磁盘占用会增加");
    json rules_diff = json::array();
    std::string old_rules;
    if (rules) {
        old_rules = read_text(fs::u8path(nc.raw_rules_file));
        rules_diff = line_diff(pretty(old_rules), pretty(rules_text));
        if (old_rules != rules_text) changed.push_back("RAW 规则文件");
    }
    pending_ = {random_hex(12), text, rules_text, nc.raw_rules_file, rules, changed, std::chrono::steady_clock::now()};
    return js({{"ok", true},
               {"token", pending_.token},
               {"changed", changed},
               {"warnings", warnings},
               {"diff", line_diff(old_j.is_discarded() ? old_text : old_j.dump(2), new_j.dump(2))},
               {"rules_diff", rules_diff}});
}

HttpResponse Host::apply(const json& body) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!body.contains("token") || !body["token"].is_string() || pending_.token.empty() ||
        body["token"].get<std::string>() != pending_.token)
        return fail(409, "确认码无效或已用过，请重新预览");
    if (std::chrono::steady_clock::now() - pending_.at > std::chrono::minutes(10)) {
        pending_ = {};
        return fail(409, "预览已超过 10 分钟，请重新预览");
    }
    Pending p = pending_;
    pending_ = {};
    // 预览后文件可能被别人改过：再校验一次
    Config nc;
    std::string err, backup, rules_backup;
    if (!parse_config(p.config_text, base_dir_, nc, err)) return fail(400, err);
    if (p.rules && !write_atomic(fs::u8path(p.rules_path), p.rules_text, rules_backup, err)) return fail(500, err);
    if (!write_atomic(fs::u8path(path_), p.config_text, backup, err)) return fail(500, err);
    log_.info("管理界面修改配置：" + p.changed.dump() + "（原文件备份为 " + (backup.empty() ? "无" : backup) + "）");
    // 重载：停旧引擎（等正在压缩的文件做完）→ 按新配置启动；变更说明写入配置变更审计
    engine_.reset();
    const json note = {{"source", "本机管理界面"}, {"changed", p.changed}, {"backup", backup}, {"rules_backup", rules_backup}};
    std::string text_err;
    Config c;
    if (load_config(path_, c, text_err)) {
        c.change_note = note.dump();
        cfg_ = c;
        cfg_ok_ = true;
        log_.open(cfg_.log_dir, cfg_.log_keep_days, console_);
        auto e = std::make_unique<Engine>(cfg_, log_);
        if (e->start()) {
            {
                std::lock_guard<std::mutex> lk2(lic_mu_);
                e->set_licensed(lic_status_.can_compress());
            }
            engine_ = std::move(e);
            error_.clear();
        } else {
            error_ = "引擎启动失败，详见日志";
        }
    } else {
        error_ = "配置有误：" + text_err;
    }
    return js({{"ok", true}, {"running", engine_ != nullptr}, {"error", error_}, {"backup", backup}});
}

HttpResponse Host::logs(const HttpRequest& req) {
    std::string dir;
    {
        std::lock_guard<std::mutex> lk(mu_);
        dir = cfg_ok_ ? cfg_.log_dir : (fs::u8path(base_dir_) / "logs").u8string();
    }
    std::vector<std::string> files;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(fs::u8path(dir), ec)) {
        const std::string n = e.path().filename().u8string();
        if (is_log_name(n)) files.push_back(n);
    }
    std::sort(files.rbegin(), files.rend());
    std::string file = req.param("file");
    if (file.empty() && !files.empty()) file = files.front();
    if (!file.empty() && std::find(files.begin(), files.end(), file) == files.end()) return fail(404, "没有这个日志文件");
    const int n = std::clamp(req.param("n").empty() ? 300 : std::atoi(req.param("n").c_str()), 1, 5000);
    const std::string level = req.param("level"), q = req.param("q");
    json out = json::array();
    if (!file.empty()) {
        std::ifstream f(fs::u8path(dir) / fs::u8path(file), std::ios::binary);
        f.seekg(0, std::ios::end);
        const std::streamoff size = f.tellg();
        const std::streamoff from = std::max<std::streamoff>(0, size - (4 << 20));
        f.seekg(from);
        std::string t((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        auto ls = lines(t);
        if (from > 0 && !ls.empty()) ls.erase(ls.begin());  // 第一行可能不完整
        for (auto it = ls.rbegin(); it != ls.rend() && int(out.size()) < n; ++it) {
            if (it->empty()) continue;
            if (level == "warn" && it->find("[警告]") == std::string::npos && it->find("[错误]") == std::string::npos) continue;
            if (level == "error" && it->find("[错误]") == std::string::npos) continue;
            if (!q.empty() && it->find(q) == std::string::npos) continue;
            out.push_back(*it);
        }
    }
    return js({{"files", files}, {"file", file}, {"lines", out}});
}

HttpResponse Host::audit_list(const HttpRequest& req) {
    std::string dir;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!cfg_ok_) return fail(409, "配置有误，无法定位审计日志");
        dir = (cfg_.data_dir.empty() ? cfg_.log_dir + "\\data" : cfg_.data_dir) + "\\audit";
    }
    const int n = std::clamp(req.param("n").empty() ? 200 : std::atoi(req.param("n").c_str()), 1, 2000);
    const std::string act = req.param("act"), q = req.param("q");
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(fs::u8path(dir), ec))
        if (e.path().extension() == ".log") files.push_back(e.path());
    std::sort(files.rbegin(), files.rend());
    json out = json::array();
    for (const auto& p : files) {
        const auto ls = lines(read_text(p));
        for (auto it = ls.rbegin(); it != ls.rend() && int(out.size()) < n; ++it) {
            if (it->empty()) continue;
            if (!q.empty() && it->find(q) == std::string::npos) continue;
            json j = json::parse(*it, nullptr, false);
            if (j.is_discarded()) continue;
            if (!act.empty() && j.value("act", "") != act) continue;
            out.push_back(j);
        }
        if (int(out.size()) >= n) break;
    }
    return js({{"entries", out}});
}

HttpResponse Host::audit_verify() {
    std::string dir;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!cfg_ok_) return fail(409, "配置有误，无法定位审计日志");
        dir = (cfg_.data_dir.empty() ? cfg_.log_dir + "\\data" : cfg_.data_dir) + "\\audit";
    }
    const auto v = Audit::verify(dir);
    return js({{"ok", v.ok}, {"entries", v.entries}, {"files", v.files}, {"error", v.error}, {"last_hash", v.last_hash}});
}

HttpResponse Host::export_start(const json& body) {
    Config c;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!cfg_ok_) return fail(409, "配置有误，无法导出");
        c = cfg_;
    }
    auto s = [&](const char* k) { return body.contains(k) && body[k].is_string() ? body[k].get<std::string>() : std::string(); };
    const std::string out = s("out");
    if (out.empty() || !fs::u8path(out).is_absolute()) return fail(400, "请填写导出目录的完整路径（如 E:\\导出）");
    for (const auto& w : c.watch)
        if (inside(out, w.path) || inside(w.path, out))
            return fail(400, "导出目录不能在监控目录里面（否则导出的文件会被再次压缩）：" + w.path);
    Filter f;
    auto day = [&](const std::string& v, int64_t& ms) {
        std::tm tm{};
        if (sscanf_s(v.c_str(), "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) != 3) return false;
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        tm.tm_isdst = -1;
        const std::time_t t = std::mktime(&tm);
        if (t == -1) return false;
        ms = int64_t(t) * 1000;
        return true;
    };
    if (!s("from").empty() && !day(s("from"), f.from_ms)) return fail(400, "开始日期格式应为 YYYY-MM-DD");
    if (!s("to").empty()) {
        if (!day(s("to"), f.to_ms)) return fail(400, "结束日期格式应为 YYYY-MM-DD");
        f.to_ms += 86400000;
    }
    f.station = s("station");
    f.product = s("product");
    f.batch = s("batch");
    f.verdict = s("verdict");
    f.root = s("root");
    if (!f.verdict.empty() && f.verdict != "OK" && f.verdict != "NG" && f.verdict != "?") return fail(400, "OK/NG 取值不对");
    const bool restore = body.value("restore", false);
    std::lock_guard<std::mutex> lk(ex_mu_);
    if (ex_state_.value("running", false)) return fail(409, "已有导出任务在进行");
    if (ex_thread_.joinable()) ex_thread_.join();
    ex_state_ = {{"running", true}, {"out", out}, {"restore", restore}, {"files", 0}, {"failed", 0}, {"bytes", 0},
                 {"errors", json::array()}};
    const std::string data = c.data_dir.empty() ? c.log_dir + "\\data" : c.data_dir;
    ex_thread_ = std::thread([this, f, out, restore, data] {
        Index index;
        Audit audit;
        std::string err;
        ExportResult r;
        if (index.open(data + "\\index.db", err) && audit.open(data + "\\audit", err)) {
            r = export_files(
                index, audit, f, out, restore,
                [this](const std::string& e) {
                    std::lock_guard<std::mutex> lk(ex_mu_);
                    if (ex_state_["errors"].size() < 50) ex_state_["errors"].push_back(e);
                },
                [this](const ExportResult& p) {
                    std::lock_guard<std::mutex> lk(ex_mu_);
                    ex_state_["files"] = p.files;
                    ex_state_["failed"] = p.failed;
                    ex_state_["bytes"] = p.bytes;
                });
        }
        std::lock_guard<std::mutex> lk(ex_mu_);
        ex_state_["running"] = false;
        ex_state_["files"] = r.files;
        ex_state_["failed"] = r.failed;
        ex_state_["bytes"] = r.bytes;
        if (!err.empty()) ex_state_["error"] = err;
        log_.info("管理界面导出到 " + out + "：" + std::to_string(r.files) + " 个文件，失败 " + std::to_string(r.failed));
    });
    return js({{"ok", true}});
}

PasswordHash Host::file_password() {
    Config c;
    std::string err;
    std::lock_guard<std::mutex> lk(mu_);
    if (load_config(path_, c, err)) return c.ui_password;
    return cfg_ok_ ? cfg_.ui_password : PasswordHash{};  // 文件坏了：用最后一次读到的
}

bool Host::authed(const HttpRequest& req) {
    const auto h = req.headers.find("x-iis-auth");
    if (h == req.headers.end()) return false;
    std::lock_guard<std::mutex> lk(auth_mu_);
    const auto now = std::chrono::steady_clock::now();
    for (auto it = sessions_.begin(); it != sessions_.end();)  // 30 分钟不用即失效
        it = now - it->second > std::chrono::minutes(30) ? sessions_.erase(it) : std::next(it);
    const auto it = sessions_.find(h->second);
    if (it == sessions_.end()) return false;
    it->second = now;
    return true;
}

HttpResponse Host::login(const json& body) {
    const PasswordHash pw = file_password();
    if (pw.empty()) return js({{"ok", false}, {"code", "no_password"}, {"error", "还没有设置管理员密码"}}, 401);
    {
        std::lock_guard<std::mutex> lk(auth_mu_);
        const auto now = std::chrono::steady_clock::now();
        if (now < locked_until_) {
            const auto s = std::chrono::duration_cast<std::chrono::seconds>(locked_until_ - now).count() + 1;
            return fail(429, "密码错误次数过多，请 " + std::to_string(s) + " 秒后再试");
        }
    }
    const std::string given = body.value("password", std::string());
    const bool ok = check_password(pw, given);
    std::lock_guard<std::mutex> lk(auth_mu_);
    if (!ok) {
        if (++fails_ >= 5) {
            locked_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            fails_ = 0;
            log_.warn("管理界面连续 5 次密码错误，锁定 60 秒");
        }
        return js({{"ok", false}, {"code", "login"}, {"error", "密码不对"}}, 401);
    }
    fails_ = 0;
    if (sessions_.size() >= 16) sessions_.erase(sessions_.begin());
    const std::string t = random_hex(24);
    sessions_[t] = std::chrono::steady_clock::now();
    return js({{"ok", true}, {"auth", t}});
}

HttpResponse Host::logout(const HttpRequest& req) {
    const auto h = req.headers.find("x-iis-auth");
    std::lock_guard<std::mutex> lk(auth_mu_);
    if (h != req.headers.end()) sessions_.erase(h->second);
    return js({{"ok", true}});
}

// 设置 / 修改管理员密码：已设过的要先登录并提供原密码；写入配置文件（先备份），记审计（不含密码）
HttpResponse Host::set_password(const HttpRequest& req, const json& body) {
    const PasswordHash cur = file_password();
    const bool first = cur.empty();
    if (!first) {
        if (!authed(req)) return js({{"ok", false}, {"code", "login"}, {"error", "请先登录"}}, 401);
        if (!check_password(cur, body.value("old", std::string()))) return fail(400, "原密码不对");
    }
    const std::string pw = body.value("new", std::string());
    if (pw.size() < 6) return fail(400, "新密码至少 6 个字符");
    const PasswordHash h = make_password(pw);
    std::string backup, err;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!write_password(path_, h, backup, err)) return fail(409, err);
        cfg_.ui_password = h;
        if (cfg_ok_) {
            Audit a;
            if (a.open((cfg_.data_dir.empty() ? cfg_.log_dir + "\\data" : cfg_.data_dir) + "\\audit", err))
                a.append("password", "", {{"source", "本机管理界面"}, {"first", first}, {"backup", backup}});
        }
    }
    log_.info(first ? "管理界面设置了管理员密码" : "管理界面修改了管理员密码");
    std::lock_guard<std::mutex> lk(auth_mu_);
    sessions_.clear();  // 改密码后所有会话失效，重新登录
    const std::string t = random_hex(24);
    sessions_[t] = std::chrono::steady_clock::now();
    return js({{"ok", true}, {"auth", t}});
}

std::string Host::license_path() {
    std::lock_guard<std::mutex> lk(mu_);
    return cfg_ok_ && !cfg_.license_file.empty() ? cfg_.license_file : (fs::u8path(base_dir_) / "license.lic").u8string();
}

std::string Host::data_dir() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!cfg_ok_) return (fs::u8path(base_dir_) / "data").u8string();
    return cfg_.data_dir.empty() ? cfg_.log_dir + "\\data" : cfg_.data_dir;
}

void Host::audit(const std::string& act, const json& detail) {
    Audit a;
    std::string err;
    if (a.open(data_dir() + "\\audit", err)) a.append(act, "", detail);
}

// 此前观察到的最大时间：clock.json 记录的高水位与审计日志最后一条的时间，取较大者
int64_t Host::high_water() {
    int64_t hw = 0;
    const fs::path dir = fs::u8path(data_dir());
    const json c = json::parse(read_text(dir / "clock.json"), nullptr, false);
    if (c.is_object() && c.contains("high_water") && c["high_water"].is_number_integer()) hw = c["high_water"].get<int64_t>();
    std::vector<fs::path> logs;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir / "audit", ec))
        if (e.path().extension() == ".log") logs.push_back(e.path());
    std::sort(logs.begin(), logs.end());
    if (!logs.empty()) {
        const auto ls = lines(read_text(logs.back()));
        for (auto it = ls.rbegin(); it != ls.rend(); ++it) {
            const json j = json::parse(*it, nullptr, false);
            if (j.is_discarded() || !j.contains("t")) continue;
            // 2026-11-02T10:00:00.123+0800 → Unix 秒
            int Y, M, D, h, m, s, ms;
            char sign = '+';
            int off = 0;
            if (sscanf_s(j["t"].get<std::string>().c_str(), "%d-%d-%dT%d:%d:%d.%d%c%d", &Y, &M, &D, &h, &m, &s, &ms, &sign, 1, &off) == 9) {
                std::tm tm{};
                tm.tm_year = Y - 1900, tm.tm_mon = M - 1, tm.tm_mday = D, tm.tm_hour = h, tm.tm_min = m, tm.tm_sec = s;
                const int64_t utc = int64_t(_mkgmtime(&tm)) - (sign == '-' ? -1 : 1) * ((off / 100) * 3600 + (off % 100) * 60);
                hw = std::max(hw, utc);
            }
            break;
        }
    }
    return hw;
}

lic::Status Host::license_status() {
    std::lock_guard<std::mutex> lk(lic_mu_);
    return lic_status_;
}

void Host::check_license() {
    const std::string path = license_path();
    std::error_code ec;
    const bool have = fs::exists(fs::u8path(path), ec);
    const std::string text = have ? read_text(fs::u8path(path)) : std::string();
    const int64_t now = int64_t(std::time(nullptr)), hw = high_water();
    const lic::Status st = lic::evaluate(have ? &text : nullptr, lic_key_, fp_, now, hw);
    if (st.state != lic::State::ClockRollback && now > hw) {  // 记下新的高水位
        std::string backup, err;
        const fs::path clock = fs::u8path(data_dir()) / "clock.json";
        fs::create_directories(clock.parent_path(), ec);
        std::ofstream f(clock, std::ios::binary | std::ios::trunc);
        f << json{{"high_water", now}}.dump();
    }
    lic::State before;
    {
        std::lock_guard<std::mutex> lk(lic_mu_);
        before = lic_status_.state;
        lic_status_ = st;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (engine_) engine_->set_licensed(st.can_compress());
    }
    if (before != st.state && !(before == lic::State::None && st.state == lic::State::None)) {
        log_.write(st.can_compress() ? Log::INFO : Log::WARN,
                   std::string("授权状态：") + lic::state_name(before) + " → " + lic::state_name(st.state) + "：" + st.text);
        audit("license_state", {{"from", lic::state_name(before)}, {"to", lic::state_name(st.state)}, {"text", st.text},
                                {"id", st.lic.id}});
    }
}

HttpResponse Host::license_get() {
    const lic::Status st = license_status();
    json j = {{"state", lic::state_name(st.state)},
              {"ok", st.can_compress()},
              {"text", st.text},
              {"machine_code", lic::machine_code(fp_)},
              {"fingerprint_items", fp_.size()},
              {"dev_key", lic::dev_key()},
              {"file", license_path()}};
    if (st.state != lic::State::None && st.state != lic::State::Invalid) {
        j["license"] = {{"id", st.lic.id},
                        {"customer", st.lic.customer},
                        {"device", st.lic.device},
                        {"tier", st.lic.tier},
                        {"issued", lic::date_text(st.lic.issued)},
                        {"not_before", lic::date_text(st.lic.not_before)},
                        {"expires", st.lic.expires ? lic::date_text(st.lic.expires - 1) : "永久"},
                        {"days_left", st.days_left},
                        {"matched", st.matched},
                        {"total", st.total},
                        {"features", st.lic.features}};
    }
    return js(j);
}

// 导入授权：先按本机判定，只接受有效或未到生效日的；备份原文件后原子写入，记审计，立即生效
HttpResponse Host::license_import(const json& body) {
    const std::string text = body.value("text", std::string());
    if (text.empty()) return fail(400, "请粘贴授权文件内容或选择授权文件");
    const lic::Status st = lic::evaluate(&text, lic_key_, fp_, int64_t(std::time(nullptr)), high_water());
    if (st.state != lic::State::Valid && st.state != lic::State::NotYet)
        return fail(400, std::string("授权不能导入：") + lic::state_name(st.state) + "——" + st.text);
    std::string backup, err;
    if (!write_atomic(fs::u8path(license_path()), text, backup, err)) return fail(500, err);
    audit("license", {{"source", "本机管理界面"}, {"id", st.lic.id}, {"customer", st.lic.customer}, {"device", st.lic.device},
                      {"tier", st.lic.tier}, {"expires", st.lic.expires ? lic::date_text(st.lic.expires - 1) : "永久"},
                      {"backup", backup}});
    log_.info("导入授权 " + st.lic.id + "（" + st.lic.customer + " / " + st.lic.device + "）");
    check_license();
    return license_get();
}

HttpResponse Host::export_state() {
    std::lock_guard<std::mutex> lk(ex_mu_);
    return js(ex_state_);
}

}  // namespace iis::svc
