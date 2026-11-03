// T9 授权与服务：未授权不压缩、导入授权后压缩、换机器 / 篡改的授权不能导入、过期后停止压缩但查看还原导出照常、
// 系统时间回拨、审计记录。签名与指纹本身见 src/license/tests。

TEST(license_engine_gate) {
    const std::string root = fresh_dir("lic_gate");
    Config c = quick_config(root);
    Log log;
    log.open(c.log_dir, 30, false);
    Engine e(c, log);
    CHECK(e.start());  // 默认未授权
    const Bytes d = make_bmp(120, 90, 700);
    write_file(root + "\\a.bmp", d);
    std::this_thread::sleep_for(std::chrono::seconds(2));
    CHECK(exists(root + "\\a.bmp") && !exists(root + "\\a.bmp.iis"));
    CHECK(!e.snapshot().licensed);
    e.set_licensed(true);
    CHECK(wait_for([&] { return compressed_ok(root + "\\a.bmp", d); }, 15));
    e.stop();
}

TEST(license_service_flow) {
    // 没有授权：服务照常运行，只是不压缩；界面给出机器码
    Ui ui("lic_flow", nlohmann::json::object(), true, true, false);
    const Bytes d = make_bmp(140, 100, 710);
    write_file(ui.dir + "\\watch\\a.bmp", d);
    std::this_thread::sleep_for(std::chrono::seconds(2));
    CHECK(exists(ui.dir + "\\watch\\a.bmp") && !exists(ui.dir + "\\watch\\a.bmp.iis"));
    nlohmann::json st = Ui::J(ui.get("/api/status"));
    CHECK(st["running"].get<bool>() && !st["license"]["ok"].get<bool>() && !st["licensed"].get<bool>());
    nlohmann::json li = Ui::J(ui.get("/api/license"));
    CHECK(li["state"] == "未授权" && li["machine_code"].get<std::string>().rfind("IIS1-", 0) == 0);
    std::vector<iis::lic::FpItem> fp;
    std::string err;
    CHECK(iis::lic::parse_machine_code(li["machine_code"], fp, err) && fp.size() >= 2);
    // 导入别的机器的授权、被改过的授权：拒绝，不保存
    iis::lic::License other;
    other.id = "L-OTHER";
    other.customer = other.device = "x";
    other.issued = other.not_before = int64_t(std::time(nullptr)) - 86400;
    other.machine = {iis::lic::fp_item("cpu", "别的"), iis::lic::fp_item("os", "别的"), iis::lic::fp_item("disk", "别的")};
    HttpResponse r = ui.post("/api/license", {{"text", iis::lic::issue(other, test_key().sk)}});
    CHECK_EQ(r.status, 400);
    std::printf("  别的机器：%s\n", Ui::J(r)["error"].get<std::string>().c_str());
    std::string good = test_key().issue(30);
    std::string tampered = good;
    tampered.replace(tampered.find("测试客户"), std::strlen("测试客户"), "别的客户");
    CHECK_EQ(ui.post("/api/license", {{"text", tampered}}).status, 400);
    CHECK(!exists(ui.dir + "\\license.lic"));
    // 未登录不能导入
    const std::string auth = ui.auth;
    ui.auth.clear();
    CHECK_EQ(ui.post("/api/license", {{"text", good}}).status, 401);
    ui.auth = auth;
    // 导入有效授权：立即生效，开始压缩
    r = ui.post("/api/license", {{"text", good}});
    CHECK_EQ(r.status, 200);
    CHECK(Ui::J(r)["ok"].get<bool>() && Ui::J(r)["license"]["days_left"] == 30);
    CHECK(wait_for([&] { return compressed_ok(ui.dir + "\\watch\\a.bmp", d); }, 15));
    // 换成已过期的授权（模拟到期）：停止压缩新文件；已有 .iis 的导出（还原）照常
    const std::string expired = [] {
        iis::lic::License l;
        l.id = "L-EXP";
        l.customer = "测试客户";
        l.device = "测试设备";
        const int64_t now = int64_t(std::time(nullptr));
        l.issued = now - 40 * 86400;
        l.not_before = now - 41 * 86400;
        l.expires = now - 86400;
        l.machine = iis::lic::fingerprint();
        return iis::lic::issue(l, test_key().sk);
    }();
    CHECK_EQ(ui.post("/api/license", {{"text", expired}}).status, 400);  // 过期的不接受导入
    write_file(ui.dir + "\\license.lic", Bytes(expired.begin(), expired.end()));  // 文件里的授权到期了
    ui.host->check_license();
    CHECK(ui.host->license_status().state == iis::lic::State::Expired);
    const Bytes d2 = make_bmp(140, 100, 711);
    write_file(ui.dir + "\\watch\\b.bmp", d2);
    std::this_thread::sleep_for(std::chrono::seconds(2));
    CHECK(exists(ui.dir + "\\watch\\b.bmp") && !exists(ui.dir + "\\watch\\b.bmp.iis"));
    CHECK_EQ(ui.post("/api/export", {{"out", ui.dir + "\\exp"}, {"restore", true}}).status, 200);
    CHECK(wait_for([&] { return !Ui::J(ui.get("/api/export"))["running"].get<bool>(); }, 20));
    CHECK(read_file(ui.dir + "\\exp\\watch\\a.bmp") == d);  // 过期后仍能还原导出
    // 系统时间回拨：高水位在未来 3 天 → 时间异常，不压缩
    write_file(ui.dir + "\\license.lic", Bytes(good.begin(), good.end()));
    ui.host->check_license();
    CHECK(ui.host->license_status().can_compress());
    const std::string hw = "{\"high_water\":" + std::to_string(int64_t(std::time(nullptr)) + 3 * 86400) + "}";
    write_file(ui.dir + "\\data\\clock.json", Bytes(hw.begin(), hw.end()));
    ui.host->check_license();
    CHECK(ui.host->license_status().state == iis::lic::State::ClockRollback);
    std::printf("  回拨：%s\n", ui.host->license_status().text.c_str());
    st = Ui::J(ui.get("/api/status"));
    CHECK(!st["licensed"].get<bool>());
    // 审计：导入与状态变化都有记录
    const std::string au = Ui::J(ui.get("/api/audit?n=500")).dump();
    CHECK(au.find("\"act\":\"license\"") != std::string::npos);
    CHECK(au.find("license_state") != std::string::npos && au.find("系统时间异常") != std::string::npos);
}
