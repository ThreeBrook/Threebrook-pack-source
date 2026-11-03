#include "classify.hpp"

#include "formats.hpp"

namespace iis::svc {

namespace {

std::string upper(std::string s) {
    for (auto& c : s)
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    return s;
}

bool in(const std::vector<std::string>& v, const std::string& s) {
    for (const auto& x : v)
        if (upper(x) == s) return true;
    return false;
}

}  // namespace

bool Classifier::init(const ClassifyConfig& cfg, std::string& err) {
    cfg_ = cfg;
    tpls_.clear();
    for (const auto& t : cfg.templates) {
        std::string re = "^";
        Tpl tpl;
        for (size_t i = 0; i < t.size();) {
            const char c = t[i];
            if (c == '{') {
                const size_t e = t.find('}', i);
                if (e == std::string::npos || e == i + 1) {
                    err = "路径模板的 {} 不完整：" + t;
                    return false;
                }
                tpl.names.push_back(t.substr(i + 1, e - i - 1));
                re += "([^/]*?)";
                i = e + 1;
            } else if (c == '*' && i + 1 < t.size() && t[i + 1] == '*') {
                re += ".*";
                i += 2;
            } else if (c == '*') {
                re += "[^/]*";
                ++i;
            } else if (c == '?') {
                re += "[^/]";
                ++i;
            } else {
                if (std::string("\\^$.|+()[]{}").find(c) != std::string::npos) re += '\\';
                re += c;
                ++i;
            }
        }
        re += "$";
        try {
            tpl.re = std::regex(re, std::regex::ECMAScript | std::regex::icase);
        } catch (const std::regex_error&) {
            err = "路径模板无法解析：" + t;
            return false;
        }
        tpls_.push_back(std::move(tpl));
    }
    return true;
}

std::string Classifier::norm(const std::string& v) const {
    const std::string u = upper(v);
    if (in(cfg_.ng_words, u)) return "NG";
    if (in(cfg_.ok_words, u)) return "OK";
    return "?";
}

Labels Classifier::classify(const std::string& rel) const {
    Labels l;
    const size_t slash = rel.find('/');
    if (slash != std::string::npos) l.station = rel.substr(0, slash);
    std::string verdict;
    for (const auto& t : tpls_) {
        std::smatch m;
        if (!std::regex_match(rel, m, t.re)) continue;
        for (size_t i = 0; i < t.names.size(); ++i) {
            const std::string v = m[i + 1].str();
            if (t.names[i] == "station") l.station = v;
            else if (t.names[i] == "product") l.product = v;
            else if (t.names[i] == "batch") l.batch = v;
            else if (t.names[i] == "verdict") verdict = norm(v);
        }
        break;
    }
    if (verdict.empty() || verdict == "?") {
        for (const auto& g : cfg_.ng_globs)
            if (fmt::glob_match(g, rel)) verdict = "NG";
        if (verdict.empty() || verdict == "?")
            for (const auto& g : cfg_.ok_globs)
                if (fmt::glob_match(g, rel)) verdict = "OK";
    }
    if ((verdict.empty() || verdict == "?") && cfg_.word_rule) {
        bool ng = false, ok = false;
        std::string word;
        auto flush = [&] {
            if (!word.empty()) {
                const std::string u = upper(word);
                ng = ng || in(cfg_.ng_words, u);
                ok = ok || in(cfg_.ok_words, u);
            }
            word.clear();
        };
        for (const char c : rel) {
            if (c == '/' || c == '_' || c == '-' || c == '.' || c == ' ') flush();
            else word += c;
        }
        flush();
        verdict = ng ? "NG" : ok ? "OK" : "?";  // 同时出现时按 NG 保护
    }
    l.verdict = verdict.empty() ? "?" : verdict;
    return l;
}

}  // namespace iis::svc
