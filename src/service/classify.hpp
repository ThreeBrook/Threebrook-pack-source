// 按相对路径识别工位、产品、批次与 OK / NG（T6）。
//
// 1. 路径模板（按顺序取第一个匹配的）：如 "{station}/{date}/{verdict}/*"、"{product}/{batch}/{station}_{verdict}_*"。
//    {名字} 匹配一个路径段内的任意文字（不跨 "/"），可用 station / product / batch / verdict，其他名字只占位；
//    * 匹配段内任意文字，** 可跨目录，? 匹配一个字符。不区分大小写。
// 2. 模板没有给出 OK/NG 时：ng_globs / ok_globs 通配符（相对路径）；
// 3. 再没有：把路径按 / _ - . 空格 切成词，含 NG 词（NG、NOK、FAIL、BAD、REJECT）为 NG，否则含 OK 词（OK、PASS、GOOD）为 OK。
// 都没有为“未知”。未知默认当作 NG 保护（不被自动删除），可配置为当作 OK。
// 工位默认取相对路径的第一级目录名（模板给出时以模板为准）。
#pragma once

#include <regex>
#include <string>
#include <vector>

namespace iis::svc {

struct ClassifyConfig {
    std::vector<std::string> templates;
    std::vector<std::string> ng_globs, ok_globs;
    std::vector<std::string> ng_words = {"NG", "NOK", "FAIL", "BAD", "REJECT"};
    std::vector<std::string> ok_words = {"OK", "PASS", "GOOD"};
    bool word_rule = true;       // 第 3 条：按词识别
    bool unknown_as_ng = true;   // 未知当作 NG（不自动删除）
};

struct Labels {
    std::string station, product, batch;
    std::string verdict;  // "OK" / "NG" / "?"（未知）
};

class Classifier {
public:
    Classifier() = default;
    // 模板语法有误时返回 false
    bool init(const ClassifyConfig& cfg, std::string& err);
    Labels classify(const std::string& rel) const;
    // 用于删除判断：未知按配置归类
    bool deletable_verdict(const std::string& verdict) const {
        return verdict == "OK" || (verdict == "?" && !cfg_.unknown_as_ng);
    }

private:
    struct Tpl {
        std::regex re;
        std::vector<std::string> names;
    };
    std::string norm(const std::string& v) const;
    ClassifyConfig cfg_;
    std::vector<Tpl> tpls_;
};

}  // namespace iis::svc
