#include "license.hpp"

#include "iis/iis.h"

#include <monocypher-ed25519.h>
#include <monocypher.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <random>

#ifdef _WIN32
#  define NOMINMAX
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <bcrypt.h>
#  pragma comment(lib, "bcrypt.lib")
#endif

#ifndef IIS_LICENSE_PUBKEY
#  error "需要 IIS_LICENSE_PUBKEY（由 CMake 定义）"
#endif

namespace iis::lic {

using nlohmann::json;

namespace {

constexpr const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string b64url(const std::string& in) {
    std::string out;
    uint32_t v = 0;
    int bits = 0;
    for (uint8_t c : in) {
        v = (v << 8) | c;
        bits += 8;
        while (bits >= 6) out += kB64[(v >> (bits -= 6)) & 63];
    }
    if (bits) out += kB64[(v << (6 - bits)) & 63];
    return out;
}

bool unb64url(const std::string& in, std::string& out) {
    out.clear();
    uint32_t v = 0;
    int bits = 0;
    for (char c : in) {
        const char* p = std::strchr(kB64, c);
        if (!p || !c) return false;
        v = (v << 6) | uint32_t(p - kB64);
        bits += 6;
        if (bits >= 8) out += char((v >> (bits -= 8)) & 0xFF);
    }
    return true;
}

void random_bytes(uint8_t* p, size_t n) {
#ifdef _WIN32
    BCryptGenRandom(nullptr, p, ULONG(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
#else
    std::random_device rd;  // libstdc++：/dev/urandom
    for (size_t i = 0; i < n; ++i) p[i] = uint8_t(rd());
#endif
}

json items_json(const std::vector<FpItem>& v) {
    json a = json::array();
    for (const auto& i : v) a.push_back({i.kind, i.hash});
    return a;
}

bool items_from(const json& a, std::vector<FpItem>& out) {
    out.clear();
    if (!a.is_array()) return false;
    for (const auto& e : a) {
        if (!e.is_array() || e.size() != 2 || !e[0].is_string() || !e[1].is_string()) return false;
        out.push_back({e[0].get<std::string>(), e[1].get<std::string>()});
    }
    return true;
}

json license_json(const License& l) {
    return {{"id", l.id},       {"customer", l.customer}, {"device", l.device},       {"tier", l.tier},
            {"issued", l.issued}, {"not_before", l.not_before}, {"expires", l.expires}, {"machine", items_json(l.machine)},
            {"features", l.features}, {"format", 1}};
}

constexpr int64_t kDay = 86400;
constexpr int64_t kClockSlack = 2 * 3600;  // 允许 2 小时的对时误差

}  // namespace

std::string to_hex(const uint8_t* d, size_t n) {
    static const char* x = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) s += x[d[i] >> 4], s += x[d[i] & 15];
    return s;
}

bool from_hex(const std::string& h, uint8_t* out, size_t n) {
    if (h.size() != n * 2) return false;
    for (size_t i = 0; i < n; ++i) {
        auto v = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
        const int a = v(h[2 * i]), b = v(h[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        out[i] = uint8_t(a << 4 | b);
    }
    return true;
}

std::string date_text(int64_t t) {
    const std::time_t tt = std::time_t(t);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char b[16];
    std::strftime(b, sizeof b, "%Y-%m-%d", &tm);
    return b;
}

FpItem fp_item(const std::string& kind, const std::string& value) {
    const std::string s = "iis-fp-v1|" + kind + "|" + value;
    uint8_t d[32];
    iis_sha256(s.data(), s.size(), d);
    return {kind, to_hex(d, 8)};
}

std::string machine_code(const std::vector<FpItem>& items) {
    return "IIS1-" + b64url(json{{"v", 1}, {"items", items_json(items)}}.dump());
}

bool parse_machine_code(const std::string& code, std::vector<FpItem>& out, std::string& err) {
    std::string c;
    for (char ch : code)
        if (!isspace(uint8_t(ch))) c += ch;  // 容忍复制时带进来的空白与换行
    std::string raw;
    if (c.rfind("IIS1-", 0) != 0 || !unb64url(c.substr(5), raw)) {
        err = "机器码格式不对（应以 IIS1- 开头）";
        return false;
    }
    const json j = json::parse(raw, nullptr, false);
    if (j.is_discarded() || !j.is_object() || j.value("v", 0) != 1 || !j.contains("items") || !items_from(j["items"], out) ||
        out.size() < 2) {
        err = "机器码内容损坏";
        return false;
    }
    return true;
}

int match_count(const std::vector<FpItem>& licensed, const std::vector<FpItem>& here) {
    int n = 0;
    for (const auto& a : licensed)
        for (const auto& b : here)
            if (a.kind == b.kind && a.hash == b.hash) {
                ++n;
                break;
            }
    return n;
}

bool machine_ok(const std::vector<FpItem>& licensed, const std::vector<FpItem>& here) {
    const int m = match_count(licensed, here);
    return m >= 2 && m >= int(licensed.size()) - 1;
}

void keygen(Secret64& secret, Key32& pub) {
    uint8_t seed[32];
    random_bytes(seed, 32);
    crypto_ed25519_key_pair(secret.data(), pub.data(), seed);  // 会擦除 seed
}

std::string issue(const License& lic, const Secret64& secret) {
    const json body = license_json(lic);
    const std::string msg = body.dump();
    uint8_t sig[64];
    crypto_ed25519_sign(sig, secret.data(), reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
    Key32 pub;
    std::copy(secret.begin() + 32, secret.end(), pub.begin());
    const json file = {{"iis_license", body}, {"signature", to_hex(sig, 64)}, {"key", to_hex(pub.data(), 8)}};
    return file.dump(2) + "\n";
}

bool verify(const std::string& text, const Key32& pub, License& out, std::string& err) {
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("iis_license") || !j["iis_license"].is_object() ||
        !j.contains("signature") || !j["signature"].is_string()) {
        err = "不是 IIS 授权文件";
        return false;
    }
    uint8_t sig[64];
    if (!from_hex(j["signature"].get<std::string>(), sig, 64)) {
        err = "授权文件签名格式不对";
        return false;
    }
    const json& body = j["iis_license"];
    const std::string msg = body.dump();  // 与签发时相同的规范化：键排序、紧凑
    if (crypto_ed25519_check(sig, pub.data(), reinterpret_cast<const uint8_t*>(msg.data()), msg.size()) != 0) {
        err = "授权文件签名无效（被改动过，或不是本产品签发的）";
        return false;
    }
    try {
        if (body.at("format").get<int>() != 1) throw std::runtime_error("format");
        out.id = body.at("id").get<std::string>();
        out.customer = body.at("customer").get<std::string>();
        out.device = body.at("device").get<std::string>();
        out.tier = body.at("tier").get<std::string>();
        out.issued = body.at("issued").get<int64_t>();
        out.not_before = body.at("not_before").get<int64_t>();
        out.expires = body.at("expires").get<int64_t>();
        out.features = body.at("features").get<std::vector<std::string>>();
        if (!items_from(body.at("machine"), out.machine) || out.machine.size() < 2) throw std::runtime_error("machine");
    } catch (const std::exception&) {
        err = "授权文件内容不完整";
        return false;
    }
    return true;
}

const char* state_name(State s) {
    switch (s) {
        case State::Valid: return "有效";
        case State::None: return "未授权";
        case State::Invalid: return "授权无效";
        case State::WrongMachine: return "与本机不符";
        case State::NotYet: return "未到生效日";
        case State::Expired: return "已过期";
        case State::ClockRollback: return "系统时间异常";
    }
    return "?";
}

Status evaluate(const std::string* text, const Key32& pub, const std::vector<FpItem>& here, int64_t now, int64_t high_water) {
    Status st;
    if (!text || text->empty()) {
        st.state = State::None;
        st.text = "没有授权文件";
        return st;
    }
    std::string err;
    if (!verify(*text, pub, st.lic, err)) {
        st.state = State::Invalid;
        st.text = err;
        return st;
    }
    st.total = int(st.lic.machine.size());
    st.matched = match_count(st.lic.machine, here);
    if (!machine_ok(st.lic.machine, here)) {
        st.state = State::WrongMachine;
        st.text = "授权与本机不符：指纹 " + std::to_string(st.total) + " 项中只有 " + std::to_string(st.matched) +
                  " 项一致（最多允许 1 项变化），请把新的机器码发给我方重新签发";
        return st;
    }
    // 系统时间回拨：比以前见过的时间早，或早于授权签发时间
    if ((high_water && now + kClockSlack < high_water) || now + kDay < st.lic.issued) {
        st.state = State::ClockRollback;
        st.text = "系统时间早于" + std::string(high_water && now + kClockSlack < high_water ? "此前记录的时间（" + date_text(high_water) + "）"
                                                                                          : "授权签发时间（" + date_text(st.lic.issued) + "）") +
                  "，疑似被调回；请把系统时间改正";
        return st;
    }
    if (now < st.lic.not_before) {
        st.state = State::NotYet;
        st.text = "授权从 " + date_text(st.lic.not_before) + " 起生效";
        return st;
    }
    if (st.lic.expires && now >= st.lic.expires) {
        st.state = State::Expired;
        st.text = "授权已于 " + date_text(st.lic.expires) + " 到期";
        return st;
    }
    st.state = State::Valid;
    st.days_left = st.lic.expires ? (st.lic.expires - now + kDay - 1) / kDay : -1;
    st.text = st.lic.expires ? "有效期至 " + date_text(st.lic.expires - 1) + "（剩余 " + std::to_string(st.days_left) + " 天）" : "永久有效";
    return st;
}

const Key32& product_key() {
    static const Key32 k = [] {
        Key32 v{};
        from_hex(IIS_LICENSE_PUBKEY, v.data(), 32);
        return v;
    }();
    return k;
}

bool dev_key() {
#ifdef IIS_LICENSE_DEV_KEY
    return true;
#else
    return false;
#endif
}

}  // namespace iis::lic
