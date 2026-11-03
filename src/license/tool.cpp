// iis-license：授权签发工具（我方内部使用，不随产品发布）。
//
//   iis-license keygen --out 私钥.key                 生成生产密钥对：私钥写入文件（不覆盖），公钥打印出来供构建使用
//   iis-license pubkey --key 私钥.key                 打印私钥对应的公钥
//   iis-license machine-code                          本机机器码（测试用；客户在设备的管理界面或 iis-service machine-code 取）
//   iis-license decode 机器码                          显示机器码里的指纹项
//   iis-license issue --key 私钥.key --machine 机器码 --customer 客户 --device 设备
//                     [--tier standard] [--days 365 | --until 2027-12-31 | --perpetual] [--from 2026-11-02]
//                     [--id 编号] [--feature 名称]… --out 授权.lic
//                                                     签发；同时在私钥旁的“签发记录.csv”追加一行
//   iis-license verify 授权.lic [--pub 公钥] [--machine 机器码]
//                                                     校验签名并按机器码（默认本机）判定状态；--pub 默认为本程序编译进的公钥
#include "license.hpp"

#include "iis/iis.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#  define NOMINMAX
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <shellapi.h>
#  pragma comment(lib, "shell32.lib")
#endif

namespace fs = std::filesystem;
using namespace iis::lic;

namespace {

[[maybe_unused]] std::string u8(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string slurp(const std::string& p) {
    std::ifstream f(fs::u8path(p), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

int64_t now() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

// 本地日期 0 点 → Unix 秒
bool day(const std::string& s, int64_t& t) {
    std::tm tm{};
    if (std::sscanf(s.c_str(), "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) != 3) return false;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    tm.tm_isdst = -1;
    const std::time_t v = std::mktime(&tm);
    if (v == -1) return false;
    t = int64_t(v);
    return true;
}

bool load_secret(const std::string& path, Secret64& sk, std::string& err) {
    std::string t = slurp(path);
    while (!t.empty() && isspace(uint8_t(t.back()))) t.pop_back();
    if (!from_hex(t, sk.data(), 64)) {
        err = "私钥文件格式不对：" + path;
        return false;
    }
    return true;
}

void show(const License& l) {
    std::printf("  编号 %s，客户 %s，设备 %s，档位 %s\n", l.id.c_str(), l.customer.c_str(), l.device.c_str(), l.tier.c_str());
    std::printf("  签发 %s，生效 %s，到期 %s\n", date_text(l.issued).c_str(), date_text(l.not_before).c_str(),
                l.expires ? date_text(l.expires - 1).c_str() : "永久");
    std::printf("  绑定指纹 %zu 项：", l.machine.size());
    for (const auto& i : l.machine) std::printf("%s ", i.kind.c_str());
    std::printf("\n");
}

int run(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        std::printf("用法见源码开头：keygen / pubkey / machine-code / decode / issue / verify\n");
        return 2;
    }
    const std::string cmd = args[1];
    std::map<std::string, std::string> o;
    std::vector<std::string> pos, features;
    bool perpetual = false;
    for (size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--perpetual") perpetual = true;
        else if (args[i] == "--feature" && i + 1 < args.size()) features.push_back(args[++i]);
        else if (args[i].rfind("--", 0) == 0 && i + 1 < args.size()) o[args[i].substr(2)] = args[i + 1], ++i;
        else pos.push_back(args[i]);
    }
    std::string err;
    if (cmd == "keygen") {
        if (o["out"].empty()) return std::printf("keygen 需要 --out\n"), 2;
        if (fs::exists(fs::u8path(o["out"]))) return std::printf("%s 已存在，不覆盖\n", o["out"].c_str()), 1;
        Secret64 sk;
        Key32 pk;
        keygen(sk, pk);
        std::ofstream f(fs::u8path(o["out"]), std::ios::binary);
        f << to_hex(sk.data(), 64) << "\n";
        if (!f) return std::printf("写入失败\n"), 1;
        std::printf("私钥已写入 %s——必须离线妥善保管，丢失则无法再签发、泄露则任何人都能签发\n", o["out"].c_str());
        std::printf("公钥（构建时传给 CMake：-DIIS_LICENSE_PUBKEY=…）：\n%s\n", to_hex(pk.data(), 32).c_str());
        return 0;
    }
    if (cmd == "pubkey") {
        Secret64 sk;
        if (!load_secret(o["key"], sk, err)) return std::printf("%s\n", err.c_str()), 1;
        std::printf("%s\n", to_hex(sk.data() + 32, 32).c_str());
        return 0;
    }
    if (cmd == "machine-code") {
        std::printf("%s\n", machine_code(fingerprint()).c_str());
        return 0;
    }
    if (cmd == "decode") {
        std::vector<FpItem> items;
        if (pos.empty() || !parse_machine_code(pos[0], items, err)) return std::printf("%s\n", err.empty() ? "缺少机器码" : err.c_str()), 1;
        for (const auto& i : items) std::printf("%-12s %s\n", i.kind.c_str(), i.hash.c_str());
        return 0;
    }
    if (cmd == "issue") {
        Secret64 sk;
        if (!load_secret(o["key"], sk, err)) return std::printf("%s\n", err.c_str()), 1;
        License l;
        if (!parse_machine_code(o["machine"], l.machine, err)) return std::printf("%s\n", err.c_str()), 1;
        l.customer = o["customer"];
        l.device = o["device"];
        if (l.customer.empty() || l.device.empty() || o["out"].empty()) return std::printf("issue 需要 --customer、--device、--out\n"), 2;
        l.tier = o.count("tier") ? o["tier"] : "standard";
        l.features = features;
        l.issued = now();
        l.not_before = l.issued - 86400;  // 允许设备时钟比签发机慢一天
        if (o.count("from") && !day(o["from"], l.not_before)) return std::printf("--from 应为 YYYY-MM-DD\n"), 2;
        int64_t until = 0;
        if (perpetual) l.expires = 0;
        else if (o.count("until")) {
            if (!day(o["until"], until)) return std::printf("--until 应为 YYYY-MM-DD\n"), 2;
            l.expires = until + 86400;  // 含当天
        } else {
            const int days = o.count("days") ? std::atoi(o["days"].c_str()) : 365;
            if (days <= 0) return std::printf("--days 必须为正数\n"), 2;
            l.expires = l.issued + int64_t(days) * 86400;
        }
        char idb[64];
        std::snprintf(idb, sizeof idb, "L-%s-%04d", date_text(l.issued).c_str(), int(now() % 10000));
        l.id = o.count("id") ? o["id"] : idb;
        if (fs::exists(fs::u8path(o["out"]))) return std::printf("%s 已存在，不覆盖\n", o["out"].c_str()), 1;
        const std::string text = issue(l, sk);
        {
            std::ofstream f(fs::u8path(o["out"]), std::ios::binary);
            f << text;
            if (!f) return std::printf("写入失败\n"), 1;
        }
        // 签发记录：编号、日期、客户、设备、档位、到期、机器码
        const fs::path ledger = fs::u8path(o["key"]).parent_path() / fs::u8path(u8"签发记录.csv");
        const bool fresh = !fs::exists(ledger);
        std::ofstream lg(ledger, std::ios::binary | std::ios::app);
        if (fresh) lg << "\xEF\xBB\xBF" << "编号,签发,客户,设备,档位,到期,机器码\n";
        lg << l.id << "," << date_text(l.issued) << "," << l.customer << "," << l.device << "," << l.tier << ","
           << (l.expires ? date_text(l.expires - 1) : "永久") << "," << o["machine"] << "\n";
        std::printf("已签发 %s：\n", o["out"].c_str());
        show(l);
        return 0;
    }
    if (cmd == "verify") {
        if (pos.empty()) return std::printf("verify 需要授权文件\n"), 2;
        Key32 pub = product_key();
        if (o.count("pub") && !from_hex(o["pub"], pub.data(), 32)) return std::printf("--pub 应为 64 位十六进制\n"), 2;
        std::vector<FpItem> here = fingerprint();
        if (o.count("machine") && !parse_machine_code(o["machine"], here, err)) return std::printf("%s\n", err.c_str()), 1;
        const std::string text = slurp(pos[0]);
        const Status st = evaluate(&text, pub, here, now(), 0);
        std::printf("状态：%s——%s\n", state_name(st.state), st.text.c_str());
        if (st.state != State::None && st.state != State::Invalid) {
            show(st.lic);
            std::printf("  指纹一致 %d / %d\n", st.matched, st.total);
        }
        return st.can_compress() ? 0 : 1;
    }
    std::printf("不认识的命令 %s\n", cmd.c_str());
    return 2;
}

}  // namespace

#ifdef _WIN32
int main() {
    SetConsoleOutputCP(CP_UTF8);
    int n = 0;
    LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
    std::vector<std::string> args;
    for (int i = 0; i < n; ++i) args.push_back(u8(fs::path(w[i])));
    LocalFree(w);
    return run(args);
}
#else
int main(int argc, char** argv) { return run(std::vector<std::string>(argv, argv + argc)); }
#endif
