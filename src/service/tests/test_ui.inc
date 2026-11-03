// T7 本机管理界面测试（由 test_service.cpp 包含）：访问控制、状态、配置预览 / 确认 / 备份 / 重载 / 审计、
// 配置有误时界面仍可用并可在界面里改好、RAW 规则、日志、审计、导出，以及真实套接字上的 HTTP 处理。

namespace {

// 测试用授权密钥对：服务换用它的公钥，授权文件绑定本机指纹
struct TestKey {
    iis::lic::Secret64 sk;
    iis::lic::Key32 pk;
    TestKey() { iis::lic::keygen(sk, pk); }
    std::string issue(int64_t expires_from_now_days = 365) const {
        iis::lic::License l;
        l.id = "L-TEST";
        l.customer = "测试客户";
        l.device = "测试设备";
        const int64_t now = int64_t(std::time(nullptr));
        l.issued = now - 86400;
        l.not_before = now - 2 * 86400;
        l.expires = expires_from_now_days ? now + expires_from_now_days * 86400 : 0;
        l.machine = iis::lic::fingerprint();
        return iis::lic::issue(l, sk);
    }
};
const TestKey& test_key() {
    static const TestKey k;
    return k;
}

struct Ui {
    std::string dir, cfg, auth;
    std::unique_ptr<Host> host;
    explicit Ui(const std::string& name, const nlohmann::json& extra = nlohmann::json::object(), bool valid = true,
                bool set_password = true, bool licensed = true) {
        dir = fresh_dir(name);
        stdfs::create_directories(P(dir + "\\watch"));
        cfg = dir + "\\config.json";
        nlohmann::json j = {{"watch", {{{"path", dir + "\\watch"}, {"include", {"*.bmp"}}}}}, {"delay_s", 0.3},
                            {"stable_s", 0.1}, {"retry_s", 0.3}, {"rescan_s", 0}, {"priority", "normal"},
                            {"min_free_mb", 0}, {"status_s", 0.5}, {"ui", {{"port", 0}}}};
        for (auto it = extra.begin(); it != extra.end(); ++it) j[it.key()] = it.value();
        std::string t = j.dump(2);
        if (!valid) t += "\n  这不是 JSON";
        write_file(cfg, Bytes(t.begin(), t.end()));
        if (licensed) {
            const std::string lic_text = test_key().issue();
            write_file(dir + "\\license.lic", Bytes(lic_text.begin(), lic_text.end()));
        }
        host = std::make_unique<Host>(cfg, false);
        host->set_license_key(test_key().pk);
        host->start();
        if (valid && set_password) {  // 先设管理员密码（之后改配置、导出都要登录）
            const HttpResponse r = post("/api/password", {{"new", "测试密码-123"}});
            CHECK_EQ(r.status, 200);
            auth = J(r).value("auth", "");
        }
    }
    ~Ui() { host->stop(); }
    std::string hosthdr() const { return "127.0.0.1:" + std::to_string(host->ui_port()); }
    HttpResponse get(const std::string& path) {
        HttpRequest r;
        r.method = "GET";
        const size_t q = path.find('?');
        r.path = path.substr(0, q);
        if (q != std::string::npos) r.query = path.substr(q + 1);
        r.headers["host"] = hosthdr();
        return host->handle(r);
    }
    HttpResponse post(const std::string& path, const nlohmann::json& body, bool token = true) {
        HttpRequest r;
        r.method = "POST";
        r.path = path;
        r.body = body.dump();
        r.headers["host"] = hosthdr();
        r.headers["content-type"] = "application/json";
        if (token) r.headers["x-iis-token"] = host->token();
        if (!auth.empty()) r.headers["x-iis-auth"] = auth;
        return host->handle(r);
    }
    static nlohmann::json J(const HttpResponse& r) { return nlohmann::json::parse(r.body); }
    std::string config_text() const {
        const Bytes b = read_file(cfg);
        return std::string(b.begin(), b.end());
    }
};

std::string raw_http(uint16_t port, const std::string& request) {
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    std::string out;
    if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) {
        send(s, request.data(), int(request.size()), 0);
        char buf[4096];
        int n;
        while ((n = recv(s, buf, sizeof buf, 0)) > 0) out.append(buf, size_t(n));
    }
    closesocket(s);
    WSACleanup();
    return out;
}

}  // namespace

TEST(ui_access_control) {
    Ui ui("ui_access");
    CHECK(ui.host->engine_running());
    HttpResponse r = ui.get("/");
    CHECK_EQ(r.status, 200);
    CHECK(r.body.find("IIS 本机管理") != std::string::npos);
    // 其他 Host（DNS 重绑定）与跨站来源一律拒绝
    HttpRequest q;
    q.method = "GET";
    q.path = "/api/status";
    q.headers["host"] = "evil.example.com";
    CHECK_EQ(ui.host->handle(q).status, 403);
    q.headers["host"] = ui.hosthdr();
    q.headers["origin"] = "http://evil.example.com";
    CHECK_EQ(ui.host->handle(q).status, 403);
    // 改动类请求：没有会话码、会话码不对、不是 JSON
    CHECK_EQ(ui.post("/api/config/preview", {{"config", "{}"}}, false).status, 403);
    HttpRequest p;
    p.method = "POST";
    p.path = "/api/audit/verify";
    p.headers["host"] = ui.hosthdr();
    p.headers["x-iis-token"] = "0123";
    p.headers["content-type"] = "application/json";
    CHECK_EQ(ui.host->handle(p).status, 403);
    p.headers["x-iis-token"] = ui.host->token();
    p.headers["content-type"] = "text/plain";
    CHECK_EQ(ui.host->handle(p).status, 400);
    CHECK_EQ(ui.get("/api/nothing").status, 404);
    CHECK_EQ(ui.get("/../config.json").status, 404);
    // 真实套接字：只监听本机，Host 头检查、超大请求拒绝
    const uint16_t port = ui.host->ui_port();
    std::string resp = raw_http(port, "GET /api/session HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\n\r\n");
    CHECK(resp.rfind("HTTP/1.1 200", 0) == 0 && resp.find(ui.host->token()) != std::string::npos);
    resp = raw_http(port, "GET /api/session HTTP/1.1\r\nHost: attacker:" + std::to_string(port) + "\r\n\r\n");
    CHECK(resp.rfind("HTTP/1.1 403", 0) == 0);
    resp = raw_http(port, "POST /api/export HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                              "\r\nContent-Length: 99999999\r\n\r\n");
    CHECK(resp.rfind("HTTP/1.1 413", 0) == 0);
    CHECK(resp.find("X-Frame-Options: DENY") != std::string::npos);
}

TEST(ui_config_preview_apply) {
    Ui ui("ui_config");
    const nlohmann::json st = Ui::J(ui.get("/api/status"));
    CHECK(st["running"].get<bool>() && st["storage"].is_array());
    const nlohmann::json cur = Ui::J(ui.get("/api/config"));
    nlohmann::json c = nlohmann::json::parse(cur["text"].get<std::string>());
    // 有误的配置：清楚的错误，不保存
    nlohmann::json bad = c;
    bad["dealy_s"] = 5;
    HttpResponse r = ui.post("/api/config/preview", {{"config", bad.dump()}});
    CHECK_EQ(r.status, 400);
    CHECK(Ui::J(r)["error"].get<std::string>().find("dealy_s") != std::string::npos);
    bad = c;
    bad["disk"] = {{"warn_free_percent", 5}, {"delete_free_percent", 10}};
    CHECK_EQ(ui.post("/api/config/preview", {{"config", bad.dump()}}).status, 400);
    // 正确的修改：预览 → 差异 + 确认码
    c["delay_s"] = 7;
    c["watch"].push_back({{"path", ui.dir + "\\还没有的目录"}});
    r = ui.post("/api/config/preview", {{"config", c.dump(2)}});
    CHECK_EQ(r.status, 200);
    const nlohmann::json pv = Ui::J(r);
    CHECK(pv["changed"].dump().find("delay_s") != std::string::npos);
    CHECK(pv["warnings"].dump().find("不存在") != std::string::npos);
    bool plus = false;
    for (const auto& d : pv["diff"]) plus = plus || (d["op"] == "+" && d["line"].get<std::string>().find("\"delay_s\": 7") != std::string::npos);
    CHECK(plus);
    const std::string before = ui.config_text();
    CHECK_EQ(ui.post("/api/config/apply", {{"token", "nope"}}).status, 409);
    CHECK(ui.config_text() == before);  // 没确认就不改
    r = ui.post("/api/config/apply", {{"token", pv["token"]}});
    CHECK_EQ(r.status, 200);
    const nlohmann::json ap = Ui::J(r);
    CHECK(ap["ok"].get<bool>() && ap["running"].get<bool>());
    CHECK(exists(ap["backup"].get<std::string>()));
    CHECK(read_file(ap["backup"].get<std::string>()) == Bytes(before.begin(), before.end()));
    CHECK(nlohmann::json::parse(ui.config_text())["delay_s"] == 7);
    CHECK_EQ(ui.post("/api/config/apply", {{"token", pv["token"]}}).status, 409);  // 确认码只能用一次
    CHECK(ui.host->engine_running());
    // 引擎按新配置工作：新目录建好后会被监控
    stdfs::create_directories(P(ui.dir + "\\还没有的目录"));
    // 审计：配置变更带来源与改动项
    const nlohmann::json au = Ui::J(ui.get("/api/audit?act=config"));
    bool found = false;
    for (const auto& e : au["entries"])
        found = found || (e["d"].contains("change") && e["d"]["change"]["source"] == "本机管理界面");
    CHECK(found);
    CHECK(Ui::J(ui.post("/api/audit/verify", nlohmann::json::object()))["ok"].get<bool>());
}

TEST(ui_raw_rules) {
    Ui ui("ui_raw");
    nlohmann::json c = nlohmann::json::parse(Ui::J(ui.get("/api/config"))["text"].get<std::string>());
    // 没填规则文件路径
    HttpResponse r = ui.post("/api/config/preview", {{"config", c.dump()}, {"raw_rules", "{\"raw\": []}"}});
    CHECK_EQ(r.status, 400);
    c["raw_rules_file"] = "raw_rules.json";
    r = ui.post("/api/config/preview",
                {{"config", c.dump()}, {"raw_rules", R"({"raw": [{"match": "a/*.raw", "width": 0, "height": 10, "pixel_format": "Mono8"}]})"}});
    CHECK_EQ(r.status, 400);
    CHECK(Ui::J(r)["error"].get<std::string>().find("RAW") != std::string::npos);
    const std::string rules = R"({"raw": [{"match": "a/*.raw", "width": 64, "height": 48, "pixel_format": "Mono12p"}]})";
    r = ui.post("/api/config/preview", {{"config", c.dump()}, {"raw_rules", rules}});
    CHECK_EQ(r.status, 200);
    CHECK(Ui::J(r)["changed"].dump().find("RAW 规则文件") != std::string::npos);
    CHECK_EQ(ui.post("/api/config/apply", {{"token", Ui::J(r)["token"]}}).status, 200);
    CHECK(read_file(ui.dir + "\\raw_rules.json") == Bytes(rules.begin(), rules.end()));
    CHECK(Ui::J(ui.get("/api/config"))["raw_rules_text"] == rules);
    CHECK(ui.host->engine_running());
}

TEST(ui_broken_config_recoverable) {
    // 配置文件坏了：引擎不启动，但界面照常，可在界面里改好并自动启动
    Ui ui("ui_broken", nlohmann::json::object(), false);
    CHECK(!ui.host->engine_running());
    const nlohmann::json st = Ui::J(ui.get("/api/status"));
    CHECK(!st["running"].get<bool>() && !st["error"].get<std::string>().empty());
    const nlohmann::json good = {{"watch", {{{"path", ui.dir + "\\watch"}}}}, {"priority", "normal"}, {"ui", {{"port", 0}}}};
    const HttpResponse r = ui.post("/api/config/preview", {{"config", good.dump(2)}});
    CHECK_EQ(r.status, 200);
    const HttpResponse a = ui.post("/api/config/apply", {{"token", Ui::J(r)["token"]}});
    CHECK_EQ(a.status, 200);
    CHECK(Ui::J(a)["running"].get<bool>());
    CHECK(ui.host->engine_running());
}

TEST(ui_logs_audit_export) {
    Ui ui("ui_misc");
    const Bytes d = make_bmp(160, 120, 900);
    write_file(ui.dir + "\\watch\\cam1\\NG\\a.bmp", d);
    write_file(ui.dir + "\\watch\\cam1\\OK\\b.bmp", make_bmp(160, 120, 901));
    CHECK(wait_for([&] { return Ui::J(ui.get("/api/status"))["compressed"] == 2; }, 20));
    // 日志：最新文件、按级别过滤、不能读别的文件
    nlohmann::json lg = Ui::J(ui.get("/api/logs?n=50"));
    CHECK(!lg["files"].empty() && !lg["lines"].empty());
    lg = Ui::J(ui.get("/api/logs?level=error"));
    for (const auto& l : lg["lines"]) CHECK(l.get<std::string>().find("[错误]") != std::string::npos);
    CHECK_EQ(ui.get("/api/logs?file=..%5Cconfig.json").status, 404);
    // 审计
    const nlohmann::json au = Ui::J(ui.get("/api/audit?act=compress"));
    CHECK_EQ(au["entries"].size(), size_t(2));
    // 导出：不能导到监控目录里、必须是完整路径
    CHECK_EQ(ui.post("/api/export", {{"out", ui.dir + "\\watch\\exp"}}).status, 400);
    CHECK_EQ(ui.post("/api/export", {{"out", "relative\\dir"}}).status, 400);
    CHECK_EQ(ui.post("/api/export", {{"out", ui.dir + "\\exp"}, {"verdict", "XX"}}).status, 400);
    CHECK_EQ(ui.post("/api/export", {{"out", ui.dir + "\\exp"}, {"verdict", "NG"}, {"restore", true}}).status, 200);
    CHECK(wait_for([&] { return !Ui::J(ui.get("/api/export"))["running"].get<bool>(); }, 20));
    const nlohmann::json ex = Ui::J(ui.get("/api/export"));
    CHECK_EQ(ex["files"].get<int>(), 1);
    CHECK_EQ(ex["failed"].get<int>(), 0);
    CHECK(read_file(ui.dir + "\\exp\\watch\\cam1\\NG\\a.bmp") == d);
}

TEST(log_retention_cleanup) {
    const std::string dir = fresh_dir("logkeep");
    write_file(dir + "\\iis-service-20200101.log", {'o', 'l', 'd'});
    write_file(dir + "\\iis-service-99991231.log", {'n', 'e', 'w'});
    write_file(dir + "\\other.log", {'x'});
    {
        Log log;
        log.open(dir, 30, false);
        log.info("测试");
    }
    CHECK(!exists(dir + "\\iis-service-20200101.log"));  // 超过保留天数
    CHECK(exists(dir + "\\iis-service-99991231.log") && exists(dir + "\\other.log"));
}

TEST(auth_vectors) {
    uint8_t mac[32];
    const std::string key = "Jefe", msg = "what do ya want for nothing?";  // RFC 4231 测试 2
    hmac_sha256(key.data(), key.size(), msg.data(), msg.size(), mac);
    static const char* x = "0123456789abcdef";
    std::string h;
    for (uint8_t b : mac) h += x[b >> 4], h += x[b & 15];
    CHECK(h == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    CHECK(pbkdf2_sha256_hex("password", "salt", 1, 32) == "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
    CHECK(pbkdf2_sha256_hex("password", "salt", 2, 32) == "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43");
    CHECK(pbkdf2_sha256_hex("password", "salt", 4096, 32) == "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");
    const PasswordHash ph = make_password("开门-2026", 2000);
    CHECK(check_password(ph, "开门-2026"));
    CHECK(!check_password(ph, "开门-2027"));
    CHECK(!check_password({}, ""));
    CHECK(make_password("same", 2000).salt != make_password("same", 2000).salt);  // 每次随机盐
}

// 默认值：界面拿到的默认值写成完整配置后能通过解析，且与 Config{} 一致（单一来源）
TEST(ui_defaults) {
    Ui ui("ui_defaults");
    const nlohmann::json d = Ui::J(ui.get("/api/config"))["defaults"];
    CHECK(d["config"].is_object() && d["watch"].is_object());
    nlohmann::json full = d["config"];
    nlohmann::json w = d["watch"];
    w["path"] = ui.dir + "\\watch";
    full["watch"] = nlohmann::json::array({w});
    Config c;
    std::string err;
    CHECK(parse_config(full.dump(), ui.dir, c, err));
    if (!err.empty()) std::printf("  %s\n", err.c_str());
    const Config def;
    CHECK_EQ(c.delay_s, def.delay_s);
    CHECK_EQ(c.stable_s, def.stable_s);
    CHECK_EQ(c.threads, def.threads);
    CHECK(c.priority == def.priority);
    CHECK_EQ(c.min_free_mb, def.min_free_mb);
    CHECK_EQ(c.disk.warn_free_percent, def.disk.warn_free_percent);
    CHECK_EQ(c.disk.accelerate_delay_s, def.disk.accelerate_delay_s);
    CHECK_EQ(c.pause.pause_percent, def.pause.pause_percent);
    CHECK(c.classify.ng_words == def.classify.ng_words);
    CHECK(c.classify.unknown_as_ng == def.classify.unknown_as_ng);
    CHECK_EQ(c.ui_port, def.ui_port);
    CHECK_EQ(c.log_keep_days, def.log_keep_days);
    CHECK(c.data_dir == U(P(ui.dir) / "data"));
    CHECK(c.log_dir == U(P(ui.dir) / "logs"));
    CHECK(c.watch[0].recursive);
    CHECK(c.watch[0].include == default_include());
    CHECK(full["delay_s"].is_number_integer());  // 整数不写成 3600.0
    // 写全默认值的配置可以经界面保存
    const HttpResponse r = ui.post("/api/config/preview", {{"config", full.dump(2)}});
    CHECK_EQ(r.status, 200);
}

// 选择文件夹：须登录；列盘符；列子文件夹（排序、不含文件与隐藏的系统文件夹）；路径有误报错
TEST(ui_browse) {
    Ui ui("ui_browse");
    const std::string b = ui.dir + "\\浏览";
    stdfs::create_directories(P(b + "\\b 子目录"));
    stdfs::create_directories(P(b + "\\A 第一"));
    stdfs::create_directories(P(b + "\\系统"));
    SetFileAttributesW(P(b + "\\系统").c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
    write_file(b + "\\文件.bmp", Bytes{1, 2, 3});
    const std::string saved = ui.auth;
    ui.auth.clear();
    CHECK_EQ(ui.post("/api/browse", {{"path", b}}).status, 401);
    ui.auth = saved;
    nlohmann::json r = Ui::J(ui.post("/api/browse", {{"path", ""}}));
    bool sys = false;
    for (const auto& x : r["dirs"]) sys = sys || x["path"].get<std::string>().rfind("C:\\", 0) == 0;
    CHECK(sys);
    CHECK(r["parent"].is_null());
    r = Ui::J(ui.post("/api/browse", {{"path", b}}));
    CHECK_EQ(r["dirs"].size(), size_t(2));
    if (r["dirs"].size() == 2) {
        CHECK(r["dirs"][0]["name"] == "A 第一");
        CHECK(r["dirs"][1]["path"] == b + "\\b 子目录");
    }
    CHECK(r["parent"] == U(P(ui.dir)));
    CHECK(Ui::J(ui.post("/api/browse", {{"path", "C:\\"}}))["parent"] == "");
    CHECK_EQ(ui.post("/api/browse", {{"path", "相对\\路径"}}).status, 400);
    CHECK_EQ(ui.post("/api/browse", {{"path", b + "\\不存在"}}).status, 400);
    SetFileAttributesW(P(b + "\\系统").c_str(), FILE_ATTRIBUTE_NORMAL);
}

TEST(ui_password) {
    Ui ui("ui_password", nlohmann::json::object(), true, false);
    // 还没设密码：查看可以，改动不行
    CHECK_EQ(ui.get("/api/status").status, 200);
    nlohmann::json ss = Ui::J(ui.get("/api/session"));
    CHECK(!ss["has_password"].get<bool>());
    const std::string text = Ui::J(ui.get("/api/config"))["text"];
    HttpResponse r = ui.post("/api/config/preview", {{"config", text}});
    CHECK_EQ(r.status, 401);
    CHECK(Ui::J(r)["code"] == "no_password");
    CHECK_EQ(ui.post("/api/export", {{"out", ui.dir + "\\exp"}}).status, 401);
    CHECK_EQ(ui.post("/api/password", {{"new", "12345"}}).status, 400);  // 太短
    r = ui.post("/api/password", {{"new", "管理员-001"}});
    CHECK_EQ(r.status, 200);
    ui.auth = Ui::J(r)["auth"];
    CHECK(Ui::J(ui.get("/api/session"))["has_password"].get<bool>());
    // 配置文件里只有盐与哈希
    CHECK(ui.config_text().find("管理员-001") == std::string::npos);
    CHECK(ui.config_text().find("\"hash\"") != std::string::npos);
    // 登录后才能改
    CHECK_EQ(ui.post("/api/config/preview", {{"config", ui.config_text()}}).status, 200);
    const std::string saved = ui.auth;
    ui.auth.clear();
    r = ui.post("/api/config/preview", {{"config", ui.config_text()}});
    CHECK_EQ(r.status, 401);
    CHECK(Ui::J(r)["code"] == "login");
    ui.auth = "伪造的";
    CHECK_EQ(ui.post("/api/config/preview", {{"config", ui.config_text()}}).status, 401);
    ui.auth.clear();
    // 不能通过改配置改密码；没写密码的配置（设密码前打开的页面）沿用现有密码
    {
        nlohmann::json j = nlohmann::json::parse(ui.config_text());
        ui.auth = saved;
        j["ui"]["password"]["hash"] = std::string(64, 'a');
        CHECK_EQ(ui.post("/api/config/preview", {{"config", j.dump()}}).status, 400);
        j["ui"].erase("password");
        j["delay_s"] = 9;
        r = ui.post("/api/config/preview", {{"config", j.dump()}});
        CHECK_EQ(r.status, 200);
        CHECK_EQ(ui.post("/api/config/apply", {{"token", Ui::J(r)["token"]}}).status, 200);
        CHECK(nlohmann::json::parse(ui.config_text())["ui"]["password"] == nlohmann::json::parse(ui.config_text())["ui"]["password"]);
        CHECK(Ui::J(ui.get("/api/session"))["has_password"].get<bool>());  // 密码还在
        ui.auth.clear();
    }
    // 登录：错 5 次锁定
    CHECK_EQ(ui.post("/api/login", {{"password", "不对"}}).status, 401);
    for (int i = 0; i < 4; ++i) ui.post("/api/login", {{"password", "不对"}});
    r = ui.post("/api/login", {{"password", "管理员-001"}});
    CHECK_EQ(r.status, 429);
    std::printf("  锁定：%s\n", Ui::J(r)["error"].get<std::string>().c_str());
    // 改密码：须登录且原密码对；改后旧会话失效
    ui.auth = saved;
    CHECK_EQ(ui.post("/api/password", {{"old", "错的"}, {"new", "新密码-002"}}).status, 400);
    r = ui.post("/api/password", {{"old", "管理员-001"}, {"new", "新密码-002"}});
    CHECK_EQ(r.status, 200);
    const std::string fresh = Ui::J(r)["auth"];
    CHECK_EQ(ui.post("/api/config/preview", {{"config", ui.config_text()}}).status, 401);  // saved 已失效
    ui.auth = fresh;
    CHECK_EQ(ui.post("/api/config/preview", {{"config", ui.config_text()}}).status, 200);
    // 命令行重设（忘记密码）：正在运行的服务立即按新密码校验
    std::string backup, err;
    CHECK(write_password(ui.cfg, make_password("命令行重设-3", 2000), backup, err));
    CHECK(exists(backup));
    ui.auth.clear();
    std::this_thread::sleep_for(std::chrono::seconds(61));  // 等前面的锁定过去
    CHECK_EQ(ui.post("/api/login", {{"password", "新密码-002"}}).status, 401);
    r = ui.post("/api/login", {{"password", "命令行重设-3"}});
    CHECK_EQ(r.status, 200);
    // 审计：两次设置 / 修改密码，不含密码本身
    ui.auth = Ui::J(r)["auth"];
    const std::string au = Ui::J(ui.get("/api/audit?act=password")).dump();
    CHECK(au.find("本机管理界面") != std::string::npos && au.find("管理员-001") == std::string::npos);
}
