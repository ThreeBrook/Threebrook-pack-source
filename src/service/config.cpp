#include "config.hpp"

#include "iis/iis.h"

#include <nlohmann/json.hpp>

#include <algorithm>

#include <filesystem>
#include <fstream>
#include <set>

namespace iis::svc {

using nlohmann::json;

const std::vector<std::string>& default_include() {
    static const std::vector<std::string> v = {"*.bmp", "*.dib", "*.tif", "*.tiff", "*.raw"};
    return v;
}

std::string defaults_json() {
    using oj = nlohmann::ordered_json;
    auto n = [](double v) { return v == double(int64_t(v)) ? oj(int64_t(v)) : oj(v); };  // 整数不写成 3600.0
    const Config c;
    const WatchDir w;
    oj j;
    j["watch"] = oj::array();
    j["delay_s"] = n(c.delay_s);
    j["stable_s"] = n(c.stable_s);
    j["retry_s"] = n(c.retry_s);
    j["rescan_s"] = n(c.rescan_s);
    j["release"] = c.release;
    j["min_saving"] = n(c.min_saving);
    j["zstd_level"] = c.zstd_level;
    j["threads"] = c.threads;
    j["priority"] = c.priority;
    j["max_cores"] = c.max_cores;
    j["cpu_rate_percent"] = c.cpu_rate_percent;
    j["max_mb_s"] = n(c.max_mb_s);
    j["min_free_mb"] = n(c.min_free_mb);
    j["pause"] = {{"processes", oj::array()},
                  {"pause_percent", n(c.pause.pause_percent)},
                  {"resume_percent", n(c.pause.resume_percent)},
                  {"resume_after_s", n(c.pause.resume_after_s)},
                  {"sample_s", n(c.pause.sample_s)}};
    j["classify"] = {{"templates", oj::array()},
                     {"ng_globs", oj::array()},
                     {"ok_globs", oj::array()},
                     {"word_rule", c.classify.word_rule},
                     {"ng_words", c.classify.ng_words},
                     {"ok_words", c.classify.ok_words},
                     {"unknown_as", c.classify.unknown_as_ng ? "ng" : "ok"}};
    j["retention"] = oj::array();
    j["retention_interval_s"] = n(c.retention_interval_s);
    j["disk"] = {{"warn_free_percent", n(c.disk.warn_free_percent)},
                 {"warn_free_mb", n(c.disk.warn_free_mb)},
                 {"delete_free_percent", n(c.disk.delete_free_percent)},
                 {"delete_free_mb", n(c.disk.delete_free_mb)},
                 {"target_free_percent", n(c.disk.target_free_percent)},
                 {"target_free_mb", n(c.disk.target_free_mb)},
                 {"accelerate_threads", c.disk.accelerate_threads},
                 {"accelerate_delay_s", n(c.disk.accelerate_delay_s)},
                 {"check_s", n(c.disk.check_s)}};
    j["ui"] = {{"enabled", c.ui_enabled}, {"port", c.ui_port}};
    j["data_dir"] = "data";
    j["log_dir"] = "logs";
    j["log_keep_days"] = c.log_keep_days;
    j["status_s"] = n(c.status_s);
    const oj wd = {{"recursive", w.recursive}, {"include", default_include()}, {"exclude", oj::array()}, {"quota_mb", n(w.quota_mb)}};
    return oj{{"config", j}, {"watch", wd}}.dump();
}

namespace {

struct Bad {
    std::string what;
};

void check_keys(const json& j, const std::set<std::string>& known, const std::string& where) {
    if (!j.is_object()) throw Bad{where + " 必须是对象"};
    for (auto it = j.begin(); it != j.end(); ++it)
        if (!known.count(it.key())) throw Bad{where + " 中有未知字段 \"" + it.key() + "\""};
}

double num(const json& j, const char* key, double def, double lo, double hi, const std::string& where) {
    if (!j.contains(key)) return def;
    if (!j[key].is_number()) throw Bad{where + "." + key + " 必须是数字"};
    const double v = j[key].get<double>();
    if (!(v >= lo && v <= hi)) throw Bad{where + "." + key + " 超出范围 " + std::to_string(lo) + "~" + std::to_string(hi)};
    return v;
}

std::string str(const json& j, const char* key, const std::string& def, const std::string& where) {
    if (!j.contains(key)) return def;
    if (!j[key].is_string()) throw Bad{where + "." + key + " 必须是字符串"};
    return j[key].get<std::string>();
}

std::vector<std::string> strs(const json& j, const char* key, const std::string& where) {
    std::vector<std::string> v;
    if (!j.contains(key)) return v;
    if (!j[key].is_array()) throw Bad{where + "." + key + " 必须是字符串数组"};
    for (const auto& e : j[key]) {
        if (!e.is_string() || e.get<std::string>().empty()) throw Bad{where + "." + key + " 必须是非空字符串数组"};
        v.push_back(e.get<std::string>());
    }
    return v;
}

std::string absolute(const std::string& p, const std::string& base) {
    if (p.empty()) return p;
    const auto u = std::filesystem::u8path(p);
    if (u.is_absolute()) return p;
    const auto s = (std::filesystem::u8path(base) / u).lexically_normal().u8string();
    return std::string(s.begin(), s.end());
}

}  // namespace

bool parse_config(const std::string& text, const std::string& base_dir, Config& c, std::string& err) {
    c = Config{};
    try {
        const json j = json::parse(text);
        check_keys(j,
                   {"watch", "raw_rules_file", "delay_s", "stable_s", "retry_s", "rescan_s", "release", "min_saving",
                    "zstd_level", "threads", "max_cores", "cores", "cpu_rate_percent", "max_mb_s", "priority",
                    "min_free_mb", "pause", "log_dir", "log_keep_days", "status_file", "status_s", "data_dir",
                    "classify", "retention", "retention_interval_s", "disk", "ui", "license_file"},
                   "配置");
        // 允许空数组：刚安装、还没配置监控目录时服务照常运行（只是没有要压缩的目录），在管理界面添加
        if (!j.contains("watch") || !j["watch"].is_array()) throw Bad{"配置.watch 必须是数组"};
        for (size_t i = 0; i < j["watch"].size(); ++i) {
            const json& w = j["watch"][i];
            const std::string where = "配置.watch[" + std::to_string(i) + "]";
            check_keys(w, {"path", "recursive", "include", "exclude", "quota_mb"}, where);
            WatchDir d;
            d.path = absolute(str(w, "path", "", where), base_dir);
            if (d.path.empty()) throw Bad{where + ".path 不能为空"};
            if (w.contains("recursive")) {
                if (!w["recursive"].is_boolean()) throw Bad{where + ".recursive 必须是 true / false"};
                d.recursive = w["recursive"].get<bool>();
            }
            d.include = strs(w, "include", where);
            if (d.include.empty()) d.include = default_include();
            d.exclude = strs(w, "exclude", where);
            d.quota_mb = num(w, "quota_mb", 0, 0, 1e12, where);
            for (const auto& o : c.watch)
                if (o.path == d.path) throw Bad{where + ".path 重复：" + d.path};
            c.watch.push_back(std::move(d));
        }
        const std::string w = "配置";
        c.raw_rules_file = absolute(str(j, "raw_rules_file", "", w), base_dir);
        c.delay_s = num(j, "delay_s", c.delay_s, 0, 30 * 86400.0, w);
        c.stable_s = num(j, "stable_s", c.stable_s, 0, 3600, w);
        c.retry_s = num(j, "retry_s", c.retry_s, 0.1, 86400, w);
        c.rescan_s = num(j, "rescan_s", c.rescan_s, 0, 30 * 86400.0, w);
        if (j.contains("release")) {
            if (!j["release"].is_boolean()) throw Bad{"配置.release 必须是 true / false"};
            c.release = j["release"].get<bool>();
        }
        c.min_saving = num(j, "min_saving", c.min_saving, 0, 0.9, w);
        c.zstd_level = int(num(j, "zstd_level", c.zstd_level, 1, 19, w));
        c.threads = int(num(j, "threads", c.threads, 1, 64, w));
        c.max_cores = int(num(j, "max_cores", c.max_cores, 0, 64, w));
        if (j.contains("cores")) {
            if (!j["cores"].is_array()) throw Bad{"配置.cores 必须是整数数组"};
            for (const auto& e : j["cores"]) {
                if (!e.is_number_integer() || e.get<int>() < 0 || e.get<int>() > 63)
                    throw Bad{"配置.cores 必须是 0~63 的整数"};
                c.cores.push_back(e.get<int>());
            }
        }
        c.cpu_rate_percent = int(num(j, "cpu_rate_percent", c.cpu_rate_percent, 0, 100, w));
        c.max_mb_s = num(j, "max_mb_s", c.max_mb_s, 0, 1e6, w);
        c.priority = str(j, "priority", c.priority, w);
        if (c.priority != "background" && c.priority != "idle" && c.priority != "below_normal" && c.priority != "normal")
            throw Bad{"配置.priority 只能是 background / idle / below_normal / normal"};
        c.min_free_mb = num(j, "min_free_mb", c.min_free_mb, 0, 1e9, w);
        if (j.contains("pause")) {
            const json& p = j["pause"];
            check_keys(p, {"processes", "pause_percent", "resume_percent", "resume_after_s", "sample_s"}, "配置.pause");
            c.pause.processes = strs(p, "processes", "配置.pause");
            c.pause.pause_percent = num(p, "pause_percent", c.pause.pause_percent, 0, 100, "配置.pause");
            c.pause.resume_percent = num(p, "resume_percent", c.pause.resume_percent, 0, 100, "配置.pause");
            c.pause.resume_after_s = num(p, "resume_after_s", c.pause.resume_after_s, 0, 3600, "配置.pause");
            c.pause.sample_s = num(p, "sample_s", c.pause.sample_s, 0.1, 60, "配置.pause");
            if (c.pause.resume_percent > c.pause.pause_percent)
                throw Bad{"配置.pause.resume_percent 不能大于 pause_percent"};
        }
        if (j.contains("classify")) {
            const json& k = j["classify"];
            const std::string kw = "配置.classify";
            check_keys(k, {"templates", "ng_globs", "ok_globs", "ng_words", "ok_words", "word_rule", "unknown_as"}, kw);
            c.classify.templates = strs(k, "templates", kw);
            c.classify.ng_globs = strs(k, "ng_globs", kw);
            c.classify.ok_globs = strs(k, "ok_globs", kw);
            if (k.contains("ng_words")) c.classify.ng_words = strs(k, "ng_words", kw);
            if (k.contains("ok_words")) c.classify.ok_words = strs(k, "ok_words", kw);
            if (k.contains("word_rule")) {
                if (!k["word_rule"].is_boolean()) throw Bad{kw + ".word_rule 必须是 true / false"};
                c.classify.word_rule = k["word_rule"].get<bool>();
            }
            const std::string ua = str(k, "unknown_as", "ng", kw);
            if (ua != "ng" && ua != "ok") throw Bad{kw + ".unknown_as 只能是 ng 或 ok"};
            c.classify.unknown_as_ng = ua == "ng";
            Classifier test;
            std::string e;
            if (!test.init(c.classify, e)) throw Bad{kw + "：" + e};
        }
        if (j.contains("retention")) {
            if (!j["retention"].is_array()) throw Bad{"配置.retention 必须是规则数组"};
            for (size_t i = 0; i < j["retention"].size(); ++i) {
                const json& r = j["retention"][i];
                const std::string rw = "配置.retention[" + std::to_string(i) + "]";
                check_keys(r, {"station", "product", "verdict", "days"}, rw);
                RetentionRule rule;
                rule.station = str(r, "station", "*", rw);
                rule.product = str(r, "product", "*", rw);
                rule.verdict = str(r, "verdict", "*", rw);
                if (rule.verdict != "OK" && rule.verdict != "NG" && rule.verdict != "?" && rule.verdict != "*")
                    throw Bad{rw + ".verdict 只能是 OK / NG / ? / *"};
                if (!r.contains("days")) throw Bad{rw + ".days 必填"};
                rule.days = num(r, "days", 0, 0, 36500, rw);
                c.retention.push_back(rule);
            }
        }
        c.retention_interval_s = num(j, "retention_interval_s", c.retention_interval_s, 1, 86400, w);
        if (j.contains("disk")) {
            const json& d = j["disk"];
            const std::string dw = "配置.disk";
            check_keys(d, {"warn_free_mb", "warn_free_percent", "delete_free_mb", "delete_free_percent", "target_free_mb",
                           "target_free_percent", "accelerate_threads", "accelerate_delay_s", "check_s"}, dw);
            c.disk.warn_free_mb = num(d, "warn_free_mb", c.disk.warn_free_mb, 0, 1e12, dw);
            c.disk.warn_free_percent = num(d, "warn_free_percent", c.disk.warn_free_percent, 0, 90, dw);
            c.disk.delete_free_mb = num(d, "delete_free_mb", c.disk.delete_free_mb, 0, 1e12, dw);
            c.disk.delete_free_percent = num(d, "delete_free_percent", c.disk.delete_free_percent, 0, 90, dw);
            c.disk.target_free_mb = num(d, "target_free_mb", c.disk.target_free_mb, 0, 1e12, dw);
            c.disk.target_free_percent = num(d, "target_free_percent", c.disk.target_free_percent, 0, 90, dw);
            c.disk.accelerate_threads = int(num(d, "accelerate_threads", 0, 0, 64, dw));
            c.disk.accelerate_delay_s = num(d, "accelerate_delay_s", c.disk.accelerate_delay_s, 0, 30 * 86400.0, dw);
            c.disk.check_s = num(d, "check_s", c.disk.check_s, 0.2, 3600, dw);
        }
        // 百分比与 MB 各自比较（实际阈值取两者中较大者，各自满足即整体满足）
        if (c.disk.delete_free_percent > c.disk.warn_free_percent || c.disk.delete_free_mb > c.disk.warn_free_mb ||
            c.disk.target_free_percent < c.disk.delete_free_percent || c.disk.target_free_mb < c.disk.delete_free_mb)
            throw Bad{"配置.disk：须满足 删除阈值 ≤ 告警阈值，且 删除后目标 ≥ 删除阈值（MB 与百分比分别比较）"};
        if (j.contains("ui")) {
            const json& u = j["ui"];
            check_keys(u, {"enabled", "port", "password"}, "配置.ui");
            if (u.contains("password")) {
                const json& pw = u["password"];
                check_keys(pw, {"salt", "hash", "iterations"}, "配置.ui.password");
                auto is_hex = [](const std::string& h, size_t n) {
                    return h.size() == n && h.find_first_not_of("0123456789abcdef") == std::string::npos;
                };
                c.ui_password.salt = str(pw, "salt", "", "配置.ui.password");
                c.ui_password.hash = str(pw, "hash", "", "配置.ui.password");
                c.ui_password.iterations = int(num(pw, "iterations", 0, 1000, 1e7, "配置.ui.password"));
                if (!is_hex(c.ui_password.salt, 32) || !is_hex(c.ui_password.hash, 64))
                    throw Bad{"配置.ui.password 格式不对（请用 iis-service set-password 设置，不要手改）"};
            }
            if (u.contains("enabled")) {
                if (!u["enabled"].is_boolean()) throw Bad{"配置.ui.enabled 必须是 true / false"};
                c.ui_enabled = u["enabled"].get<bool>();
            }
            c.ui_port = int(num(u, "port", c.ui_port, 0, 65535, "配置.ui"));  // 0 = 由系统选空闲端口（测试用）
        }
        c.license_file = absolute(str(j, "license_file", "", w), base_dir);
        if (c.license_file.empty()) c.license_file = absolute("license.lic", base_dir);
        c.data_dir = absolute(str(j, "data_dir", "", w), base_dir);
        if (c.data_dir.empty()) c.data_dir = absolute("data", base_dir);
        c.log_dir = absolute(str(j, "log_dir", "", w), base_dir);
        if (c.log_dir.empty()) c.log_dir = absolute("logs", base_dir);
        c.log_keep_days = int(num(j, "log_keep_days", c.log_keep_days, 1, 3650, w));
        c.status_file = absolute(str(j, "status_file", "", w), base_dir);
        if (c.status_file.empty()) c.status_file = absolute("status.json", c.log_dir);
        c.status_s = num(j, "status_s", c.status_s, 0.2, 3600, w);
    } catch (const Bad& b) {
        err = b.what;
        return false;
    } catch (const json::exception& e) {
        err = std::string("配置不是合法 JSON：") + e.what();
        return false;
    }
    return true;
}

bool load_config(const std::string& path, Config& out, std::string& err) {
    const auto p = std::filesystem::u8path(path);
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        err = "无法读取配置文件：" + path;
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);  // 记事本保存的 BOM
    const auto base = std::filesystem::absolute(p).parent_path().u8string();
    if (!parse_config(text, std::string(base.begin(), base.end()), out, err)) return false;
    uint8_t d[32];
    iis_sha256(text.data(), text.size(), d);
    static const char* x = "0123456789abcdef";
    for (uint8_t b : d) out.config_sha256 += x[b >> 4], out.config_sha256 += x[b & 15];
    return true;
}

}  // namespace iis::svc
