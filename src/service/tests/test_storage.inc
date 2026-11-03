// T6 存储管理测试（由 test_service.cpp 包含）：识别规则、审计哈希链、索引、防爆盘、保留策略、导出、索引重建。

namespace {

void set_mtime_days_ago(const std::string& path, double days) {
    HANDLE h = CreateFileW(P(path).wstring().c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0,
                           nullptr);
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u{{ft.dwLowDateTime, ft.dwHighDateTime}};
    u.QuadPart -= uint64_t(days * 864000000000.0);
    ft = {u.LowPart, u.HighPart};
    SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
}

std::string audit_text(const std::string& dir) {
    std::string all;
    for (const auto& e : stdfs::directory_iterator(P(dir)))
        if (e.path().extension() == ".log") {
            const Bytes b = read_file(U(e.path()));
            all.append(b.begin(), b.end());
        }
    return all;
}

size_t count_of(const std::string& text, const std::string& what) {
    size_t n = 0;
    for (size_t at = 0; (at = text.find(what, at)) != std::string::npos; at += what.size()) ++n;
    return n;
}

}  // namespace

TEST(classify_rules) {
    Classifier c;
    std::string err;
    ClassifyConfig cfg;
    cfg.templates = {"{product}/{batch}/{station}_{verdict}_*", "**/{verdict}/*.bmp"};
    CHECK(c.init(cfg, err));
    Labels l = c.classify("型号A/B0001/cam3_NG_0001.bmp");
    CHECK(l.product == "型号A" && l.batch == "B0001" && l.station == "cam3" && l.verdict == "NG");
    l = c.classify("型号A/B0001/cam3_pass_0002.bmp");
    CHECK(l.verdict == "OK");
    l = c.classify("cam1/20261102/ok/x.bmp");  // 第二个模板，不区分大小写
    CHECK(l.verdict == "OK" && l.station == "cam1");
    // 没有模板命中：按词识别
    CHECK(c.classify("cam2/img_0001_NG.bmp").verdict == "NG");
    CHECK(c.classify("cam2/Fail/img.bmp").verdict == "NG");
    CHECK(c.classify("cam2/OK-images/img.bmp").verdict == "OK");
    CHECK(c.classify("cam2/img_0001.bmp").verdict == "?");
    CHECK(c.classify("cam2/OK/img_NG.tif").verdict == "NG");  // 同时出现按 NG 保护（.tif 不命中模板）
    CHECK(c.classify("cam2/BOOK/img.bmp").verdict == "?");    // 只认整词
    CHECK(!c.deletable_verdict("NG") && c.deletable_verdict("OK") && !c.deletable_verdict("?"));
    cfg = {};
    cfg.unknown_as_ng = false;
    cfg.word_rule = false;
    cfg.ng_globs = {"*/reject/*"};
    CHECK(c.init(cfg, err));
    CHECK(c.classify("a/reject/1.bmp").verdict == "NG");
    CHECK(c.classify("a/NG/1.bmp").verdict == "?");
    CHECK(c.deletable_verdict("?"));
    cfg.templates = {"{station"};
    CHECK(!c.init(cfg, err));
}

TEST(audit_chain) {
    const std::string dir = fresh_dir("audit");
    {
        Audit a, b;  // 两个实例（如服务与命令行）交替追加，链仍连续
        std::string err;
        CHECK(a.open(dir, err) && b.open(dir, err));
        for (int i = 0; i < 20; ++i) {
            CHECK((i % 2 ? a : b).append("compress", "D:\\图像\\" + std::to_string(i) + ".bmp", {{"i", i}}));
        }
        CHECK(a.append({{"delete", "x", {{"reason", "测试"}}}, {"export", "y", {}}}));
        CHECK_EQ(a.last_seq(), uint64_t(22));
    }
    auto v = Audit::verify(dir);
    CHECK(v.ok);
    CHECK_EQ(v.entries, uint64_t(22));
    stdfs::path log;
    for (const auto& e : stdfs::directory_iterator(P(dir)))
        if (e.path().extension() == ".log") log = e.path();
    const Bytes good = read_file(U(log));
    std::string text(good.begin(), good.end());
    // 改动一条记录的内容
    {
        std::string t = text;
        const size_t at = t.find("\\u56fe\\u50cf\\\\7.bmp");
        const size_t at2 = at == std::string::npos ? t.find("7.bmp") : at;
        t[at2] = t[at2] == '7' ? '8' : '9';
        write_file(U(log), Bytes(t.begin(), t.end()));
        v = Audit::verify(dir);
        CHECK(!v.ok);
        std::printf("  改动：%s\n", v.error.c_str());
    }
    // 删除一条记录
    {
        std::string t = text;
        const size_t a = t.find('\n', t.find('\n') + 1) + 1;  // 第 3 行开头
        const size_t b = t.find('\n', a) + 1;
        t.erase(a, b - a);
        write_file(U(log), Bytes(t.begin(), t.end()));
        v = Audit::verify(dir);
        CHECK(!v.ok);
        CHECK_EQ(v.bad_seq, uint64_t(4));
        std::printf("  删除：%s\n", v.error.c_str());
    }
    // 调换两条
    {
        std::string t = text;
        const size_t a = t.find('\n') + 1, b = t.find('\n', a) + 1, c = t.find('\n', b) + 1;
        t = t.substr(0, a) + t.substr(b, c - b) + t.substr(a, b - a) + t.substr(c);
        write_file(U(log), Bytes(t.begin(), t.end()));
        CHECK(!Audit::verify(dir).ok);
    }
    // 断电留下的半行：下次追加时截掉并记一条 audit_repair，链仍完整
    {
        std::string t = text + "{\"act\":\"compress\",\"d\":{},\"pa";
        write_file(U(log), Bytes(t.begin(), t.end()));
        CHECK(!Audit::verify(dir).ok);
        Audit a;
        std::string err;
        CHECK(a.open(dir, err));
        CHECK(a.append("compress", "after", {}));
        v = Audit::verify(dir);
        CHECK(v.ok);
        CHECK_EQ(v.entries, uint64_t(24));
        CHECK(count_of(audit_text(dir), "\"audit_repair\"") == 1);
    }
}

TEST(index_basic) {
    const std::string dir = fresh_dir("index");
    Index ix;
    std::string err;
    CHECK(ix.open(dir + "\\index.db", err));
    for (int i = 0; i < 10; ++i) {
        Row r;
        r.path = "R\\f" + std::to_string(i);
        r.root = "R";
        r.rel = "f" + std::to_string(i);
        r.station = i % 2 ? "cam1" : "cam2";
        r.verdict = i < 3 ? "NG" : "OK";
        r.mtime_ms = 1000 + i;
        r.orig_size = 100;
        r.disk_size = 40;
        r.state = ST_COMPRESSED;
        ix.upsert(r);
    }
    Totals t = ix.totals("R");
    CHECK_EQ(t.files, uint64_t(10));
    CHECK_EQ(t.used, uint64_t(400));
    CHECK_EQ(t.equiv, uint64_t(1000));
    Row r;
    CHECK(ix.get("R\\f5", r));
    r.state = ST_DELETED;
    ix.upsert(r);
    t = ix.totals("R");
    CHECK_EQ(t.files, uint64_t(9));
    CHECK_EQ(t.used, uint64_t(360));
    std::vector<std::string> order;
    ix.scan_oldest("R", 1008, [&](const Row& x) {
        order.push_back(x.rel);
        return true;
    });
    CHECK((order == std::vector<std::string>{"f0", "f1", "f2", "f3", "f4", "f6", "f7"}));
    Filter f;
    f.station = "cam1";
    f.verdict = "OK";
    int n = 0;
    ix.query(f, [&](const Row&) { return ++n, true; });
    CHECK_EQ(n, 3);  // f3 f7 f9（f5 已删除）
    ix.add_daily({"", 2, 200, 80, 0, 0});
    ix.add_daily({"", 1, 100, 40, 1, 40});
    const auto d = ix.daily(3);
    CHECK(d.size() == 1 && d[0].files == 3 && d[0].bytes_in == 300 && d[0].deleted == 1);
    CHECK(ix.verdict_counts()["NG"] == 3);
    // 重开：合计从库里重新算出
    Index ix2;
    CHECK(ix2.open(dir + "\\index.db", err));
    CHECK_EQ(ix2.totals("R").used, uint64_t(360));
}

namespace {

Config storage_config(const std::string& root) {
    Config c = quick_config(root);
    c.delay_s = 0.3;
    c.stable_s = 0.1;
    c.watch[0].include = {"*"};
    c.disk.check_s = 0.3;
    c.data_dir = root + "_data";
    std::error_code ec;
    stdfs::remove_all(P(c.data_dir), ec);  // 上次运行留下的索引与审计
    return c;
}

}  // namespace

TEST(storage_disk_guard_keeps_ng) {
    const std::string root = fresh_dir("guard");
    Config c = storage_config(root);
    c.watch[0].quota_mb = 1.2;  // 配额 1.2 MB：写满约 2 MB 的图（压缩后）必然触发删除
    c.disk.warn_free_percent = 40;
    c.disk.delete_free_percent = 20;
    c.disk.target_free_percent = 30;
    Running r(c);
    std::map<std::string, Bytes> ng, ok;
    for (int i = 0; i < 30; ++i) {
        const bool is_ng = i % 3 == 0;
        const std::string f = root + "\\cam" + std::to_string(i % 2) + "\\" + (is_ng ? "NG" : "OK") + "\\img_" +
                              std::to_string(i) + ".bmp";
        const Bytes d = make_bmp(400, 300, uint32_t(100 + i));
        write_file(f, d);
        (is_ng ? ng : ok)[f] = d;
        std::this_thread::sleep_for(std::chrono::milliseconds(150));  // 修改时间从旧到新
    }
    CHECK(wait_for([&] {
        const Snapshot s = r.snap();
        return s.waiting == 0 && s.ready == 0 && s.inflight == 0 && s.compressed + s.stored >= 30 - s.deleted;
    }, 60));
    std::this_thread::sleep_for(std::chrono::seconds(2));
    const Snapshot s = r.snap();
    std::printf("  压缩 %llu，删除 %llu（NG %llu）\n", (unsigned long long)s.compressed, (unsigned long long)s.deleted,
                (unsigned long long)s.deleted_ng);
    CHECK(s.deleted > 0);
    CHECK_EQ(s.deleted_ng, uint64_t(0));
    for (const auto& kv : ng) CHECK(compressed_ok(kv.first, kv.second));  // NG 一张不少
    int ok_left = 0, ok_deleted = 0;
    const std::string audit = audit_text(c.data_dir + "\\audit");
    for (const auto& kv : ok) {
        if (compressed_ok(kv.first, kv.second)) ++ok_left;
        else if (!exists(kv.first) && !exists(kv.first + ".iis")) {
            ++ok_deleted;
            nlohmann::json p = kv.first;
            CHECK(audit.find("\"act\":\"delete\"") != std::string::npos && audit.find(p.dump()) != std::string::npos);
        }
    }
    CHECK_EQ(ok_left + ok_deleted, int(ok.size()));
    CHECK_EQ(uint64_t(ok_deleted), s.deleted);
    // 删的是最旧的：留下的 OK 都比删掉的新
    int last_deleted = -1, first_left = 1000;
    for (int i = 0; i < 30; ++i) {
        if (i % 3 == 0) continue;
        const std::string f = root + "\\cam" + std::to_string(i % 2) + "\\OK\\img_" + std::to_string(i) + ".bmp";
        if (exists(f + ".iis")) first_left = std::min(first_left, i);
        else last_deleted = std::max(last_deleted, i);
    }
    CHECK(last_deleted < first_left);
    // 用量回到配额内
    Index ix;
    std::string err;
    CHECK(ix.open(c.data_dir + "\\index.db", err));
    CHECK(ix.totals(root).used <= uint64_t(1.2e6));
    CHECK(Audit::verify(c.data_dir + "\\audit").ok);
}

TEST(storage_disk_guard_only_ng_stuck) {
    // 只有 NG：空间不够也不删，只告警
    const std::string root = fresh_dir("stuck");
    Config c = storage_config(root);
    c.watch[0].quota_mb = 0.3;
    c.disk.warn_free_percent = 40;
    c.disk.delete_free_percent = 20;
    c.disk.target_free_percent = 30;
    Running r(c);
    std::map<std::string, Bytes> ng;
    for (int i = 0; i < 8; ++i) {
        const std::string f = root + "\\NG\\img_" + std::to_string(i) + ".bmp";
        ng[f] = make_bmp(300, 200, uint32_t(200 + i));
        write_file(f, ng[f]);
    }
    CHECK(wait_for([&] { return r.snap().compressed == 8; }, 30));
    std::this_thread::sleep_for(std::chrono::seconds(2));
    CHECK_EQ(r.snap().deleted, uint64_t(0));
    for (const auto& kv : ng) CHECK(compressed_ok(kv.first, kv.second));
    CHECK(r.snap().accelerate);
    CHECK(wait_for([&] { return exists(c.status_file); }, 5));
    const Bytes st = read_file(c.status_file);
    const auto j = nlohmann::json::parse(std::string(st.begin(), st.end()));
    CHECK(j["storage"][0]["stuck"].get<bool>());
    CHECK(j["storage"][0]["level"] == "delete");
}

TEST(storage_retention) {
    const std::string root = fresh_dir("retention");
    Config c = storage_config(root);
    c.retention = {{"cam9", "*", "*", 0},   // cam9 永久保留（第一条匹配的规则说了算）
                   {"*", "*", "*", 5},      // 其余 5 天；"*" 不含 NG
                   {"*", "*", "NG", 0}};
    c.retention_interval_s = 0.5;
    std::map<std::string, std::pair<double, bool>> files;  // 路径 → （几天前，应保留）
    files[root + "\\cam1\\OK\\old.bmp"] = {10, false};
    files[root + "\\cam1\\OK\\new.bmp"] = {1, true};
    files[root + "\\cam1\\NG\\old.bmp"] = {10, true};
    files[root + "\\cam1\\unknown_old.bmp"] = {10, false};  // 未知按 * 处理（未知默认不被防爆盘删，但保留规则 * 覆盖它）
    files[root + "\\cam9\\OK\\old.bmp"] = {10, true};
    uint32_t seed = 300;
    for (const auto& kv : files) {
        write_file(kv.first, make_bmp(120, 80, seed++));
        set_mtime_days_ago(kv.first, kv.second.first);
    }
    Running r(c);
    CHECK(wait_for([&] { return r.snap().deleted == 2; }, 30));
    std::this_thread::sleep_for(std::chrono::seconds(2));
    CHECK_EQ(r.snap().deleted, uint64_t(2));
    for (const auto& kv : files) {
        const bool present = exists(kv.first) || exists(kv.first + ".iis");
        if (present != kv.second.second) std::printf("  不符合预期：%s\n", kv.first.c_str());
        CHECK(present == kv.second.second);
    }
    // 明确写 NG 的规则才删 NG
    Config c2 = c;
    c2.retention = {{"*", "*", "NG", 5}};
    r.engine->stop();
    Running r2(c2);
    CHECK(wait_for([&] { return r2.snap().deleted == 1; }, 30));
    CHECK(!exists(root + "\\cam1\\NG\\old.bmp") && !exists(root + "\\cam1\\NG\\old.bmp.iis"));
    CHECK_EQ(r2.snap().deleted_ng, uint64_t(1));
    CHECK(Audit::verify(c.data_dir + "\\audit").ok);
}

TEST(storage_index_export_audit) {
    const std::string root = fresh_dir("export");
    const std::string cfg_path = root + "_cfg.json";
    std::error_code ec;
    stdfs::remove_all(P(root + "_data"), ec);  // 上次运行留下的索引与审计
    stdfs::remove_all(P(root + "_logs"), ec);
    auto write_cfg = [&](int threads) {
        nlohmann::json j = {{"watch", {{{"path", root}, {"include", {"*"}}}}}, {"delay_s", 0.3}, {"stable_s", 0.1},
                            {"retry_s", 0.3}, {"rescan_s", 0}, {"threads", threads}, {"priority", "normal"},
                            {"min_free_mb", 0}, {"status_s", 0.5}, {"log_dir", root + "_logs"},
                            {"data_dir", root + "_data"},
                            {"classify", {{"templates", {"{station}/{batch}/{verdict}/*"}}}}};
        const std::string t = j.dump(1);
        write_file(cfg_path, Bytes(t.begin(), t.end()));
    };
    write_cfg(2);
    Config c;
    std::string err;
    CHECK(load_config(cfg_path, c, err));
    std::map<std::string, Bytes> files;
    for (int i = 0; i < 12; ++i) {
        const std::string f = root + "\\cam" + std::to_string(i % 2) + "\\B" + std::to_string(i % 3) + "\\" +
                              (i % 4 == 0 ? "NG" : "OK") + "\\img" + std::to_string(i) + ".bmp";
        files[f] = make_bmp(160, 120, uint32_t(400 + i));
        write_file(f, files[f]);
    }
    {
        Running r(c);
        CHECK(wait_for([&] { return r.snap().compressed == 12; }, 30));
    }
    // 索引：已压缩、带 SHA-256 与标签
    {
        Index ix;
        CHECK(ix.open(c.data_dir + "\\index.db", err));
        int n = 0;
        ix.query({}, [&](const Row& row) {
            ++n;
            CHECK(row.state == ST_COMPRESSED && row.sha256.size() == 64 && !row.batch.empty());
            CHECK(row.station == "cam0" || row.station == "cam1");
            return true;
        });
        CHECK_EQ(n, 12);
        CHECK_EQ(ix.verdict_counts()["NG"], uint64_t(3));
        // 导出：cam0 的 NG，还原为原始格式
        Audit a;
        CHECK(a.open(c.data_dir + "\\audit", err));
        Filter f;
        f.station = "cam0";
        f.verdict = "NG";
        const std::string out = fresh_dir("export_out");
        auto res = export_files(ix, a, f, out, true, nullptr);
        CHECK_EQ(res.files, uint64_t(3));  // img0、img4、img8
        CHECK_EQ(res.failed, uint64_t(0));
        const std::string rn = U(P(root).filename());
        CHECK(read_file(out + "\\" + rn + "\\cam0\\B0\\NG\\img0.bmp") == files[root + "\\cam0\\B0\\NG\\img0.bmp"]);
        // 再导出一次：目标已存在不覆盖
        res = export_files(ix, a, f, out, true, nullptr);
        CHECK_EQ(res.failed, uint64_t(3));
        // 保持压缩：复制 .iis
        f = {};
        f.batch = "B1";
        const std::string out2 = fresh_dir("export_out2");
        res = export_files(ix, a, f, out2, false, nullptr);
        CHECK_EQ(res.files, uint64_t(4));
        CHECK(restore_equals(out2 + "\\" + rn + "\\cam1\\B1\\OK\\img1.bmp.iis", files[root + "\\cam1\\B1\\OK\\img1.bmp"]));
    }
    // 配置变更记审计；删掉索引后重启：由扫描到的 .iis 重建
    write_cfg(3);
    CHECK(load_config(cfg_path, c, err));
    stdfs::remove(P(c.data_dir + "\\index.db"));
    stdfs::remove(P(c.data_dir + "\\index.db-wal"));
    stdfs::remove(P(c.data_dir + "\\index.db-shm"));
    {
        Running r(c);
        Index ix;  // 另一个连接：要从库里查（内存合计只跟踪本连接的写入）
        CHECK(ix.open(c.data_dir + "\\index.db", err));
        auto live = [&] {
            int n = 0;
            ix.query({}, [&](const Row&) { return ++n, true; });
            return n;
        };
        CHECK(wait_for([&] { return live() == 12; }, 20));
        Row row;
        CHECK(ix.get(root + "\\cam0\\B0\\NG\\img0.bmp", row));
        CHECK(row.state == ST_COMPRESSED && row.verdict == "NG" && row.orig_size == files.begin()->second.size());
        // 外部删除：记 missing
        stdfs::remove(P(root + "\\cam1\\B1\\OK\\img1.bmp.iis"));
        CHECK(wait_for([&] { return live() == 11; }, 10));
    }
    const std::string audit = audit_text(c.data_dir + "\\audit");
    CHECK_EQ(count_of(audit, "\"act\":\"compress\""), size_t(12));
    CHECK_EQ(count_of(audit, "\"act\":\"export\""), size_t(7));
    CHECK_EQ(count_of(audit, "\"act\":\"export_summary\""), size_t(3));
    CHECK_EQ(count_of(audit, "\"act\":\"config\""), size_t(2));  // 首次启动 + 配置变更
    CHECK_EQ(count_of(audit, "\"act\":\"missing\""), size_t(1));
    CHECK_EQ(count_of(audit, "\"act\":\"start\""), size_t(2));
    CHECK_EQ(count_of(audit, "\"act\":\"stop\""), size_t(2));
    const auto v = Audit::verify(c.data_dir + "\\audit");
    CHECK(v.ok);
    std::printf("  审计 %llu 条，校验通过\n", (unsigned long long)v.entries);
}
