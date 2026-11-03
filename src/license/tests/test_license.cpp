// 离线授权测试：Ed25519 标准向量、签发 / 校验 / 篡改、机器码、指纹容错、有效期、系统时间回拨。
#include "testing.hpp"

#include "license.hpp"

#include "iis/iis.h"

#include <monocypher-ed25519.h>

#include <cstring>

#ifdef _WIN32
#  define NOMINMAX
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

using namespace iis::lic;

namespace {

constexpr int64_t kDay = 86400;
const int64_t kNow = 1793600000;  // 2026-11 前后的固定时刻，测试与真实时钟无关

std::vector<FpItem> machine6() {
    return {fp_item("board_uuid", "u-1"), fp_item("board_serial", "s-1"), fp_item("cpu", "c-1"),
            fp_item("disk", "d-1"),       fp_item("mac", "m-1"),          fp_item("os", "o-1")};
}

License sample(const std::vector<FpItem>& m) {
    License l;
    l.id = "L-测试-0001";
    l.customer = "某设备厂商";
    l.device = "产线3-AOI-01";
    l.issued = kNow - 10 * kDay;
    l.not_before = kNow - 11 * kDay;
    l.expires = kNow + 30 * kDay;
    l.machine = m;
    l.features = {"compress"};
    return l;
}

}  // namespace

TEST(ed25519_rfc8032_vectors) {
    struct V {
        const char *seed, *pub, *msg, *sig;
    } vs[] = {
        {"9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
         "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
         "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
        {"4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
         "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
         "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
        {"c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
         "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
         "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"},
    };
    for (const auto& v : vs) {
        uint8_t seed[32], pub[32], sk[64], pk[32], sig[64], want[64];
        std::vector<uint8_t> msg(std::strlen(v.msg) / 2);
        CHECK(from_hex(v.seed, seed, 32) && from_hex(v.pub, pub, 32) && from_hex(v.sig, want, 64));
        if (!msg.empty()) CHECK(from_hex(v.msg, msg.data(), msg.size()));
        crypto_ed25519_key_pair(sk, pk, seed);
        CHECK(std::memcmp(pk, pub, 32) == 0);
        crypto_ed25519_sign(sig, sk, msg.data(), msg.size());
        CHECK(std::memcmp(sig, want, 64) == 0);
        CHECK(crypto_ed25519_check(want, pub, msg.data(), msg.size()) == 0);
        want[10] ^= 1;
        CHECK(crypto_ed25519_check(want, pub, msg.data(), msg.size()) != 0);
    }
}

TEST(license_issue_verify_tamper) {
    Secret64 sk;
    Key32 pk;
    keygen(sk, pk);
    const std::string text = issue(sample(machine6()), sk);
    License got;
    std::string err;
    CHECK(verify(text, pk, got, err));
    CHECK(got.customer == "某设备厂商" && got.device == "产线3-AOI-01" && got.machine.size() == 6);
    CHECK(got.expires == kNow + 30 * kDay);
    // 改任何字段：签名无效
    for (const char* what : {"\"customer\": \"某设备厂商\"", "\"tier\": \"standard\""}) {
        std::string t = text;
        const size_t at = t.find(what);
        CHECK(at != std::string::npos);
        t.replace(at, std::strlen(what), std::string(what).replace(std::strlen(what) - 3, 2, "XX"));
        CHECK(!verify(t, pk, got, err));
    }
    {
        nlohmann::json j = nlohmann::json::parse(text);
        j["iis_license"]["expires"] = 0;  // 改成永久
        CHECK(!verify(j.dump(), pk, got, err));
        std::printf("  篡改：%s\n", err.c_str());
    }
    // 别的密钥签发的：无效
    Secret64 sk2;
    Key32 pk2;
    keygen(sk2, pk2);
    CHECK(!verify(issue(sample(machine6()), sk2), pk, got, err));
    // 格式问题
    CHECK(!verify("不是 JSON", pk, got, err));
    CHECK(!verify("{}", pk, got, err));
    // 键顺序 / 空白不影响（规范化后签名）
    const nlohmann::json reparsed = nlohmann::json::parse(text);
    CHECK(verify(reparsed.dump(4), pk, got, err));
}

TEST(license_machine_code) {
    const auto m = machine6();
    const std::string code = machine_code(m);
    CHECK(code.rfind("IIS1-", 0) == 0);
    std::vector<FpItem> back;
    std::string err;
    CHECK(parse_machine_code(code, back, err));
    CHECK(back.size() == 6 && back[2].kind == "cpu" && back[2].hash == m[2].hash);
    CHECK(parse_machine_code("  " + code.substr(0, 20) + "\n" + code.substr(20) + " ", back, err));  // 复制时混进空白
    CHECK(!parse_machine_code("IIS2-abc", back, err));
    CHECK(!parse_machine_code("IIS1-!!!", back, err));
    // 指纹只含哈希，不含原始值
    CHECK(code.find("u-1") == std::string::npos);
    // 本机指纹：能取到若干项，两次结果相同
    const auto a = fingerprint(), b = fingerprint();
    std::printf("  本机指纹 %zu 项：", a.size());
    for (const auto& i : a) std::printf("%s ", i.kind.c_str());
    std::printf("\n");
#ifdef _WIN32
    CHECK(a.size() >= 2);
#else
    CHECK(a.size() >= 1);  // Linux 容器里取不到 DMI、machine-id，网卡为随机地址：只剩 CPU（Linux 网关版在阶段 3 再定指纹来源）
#endif
    CHECK(match_count(a, b) == int(a.size()));
}

TEST(license_states) {
    Secret64 sk;
    Key32 pk;
    keygen(sk, pk);
    const auto m = machine6();
    const std::string text = issue(sample(m), sk);
    auto st = [&](const std::string* t, const std::vector<FpItem>& here, int64_t now, int64_t hw = 0) {
        return evaluate(t, pk, here, now, hw);
    };
    Status s = st(&text, m, kNow);
    CHECK(s.state == State::Valid && s.can_compress());
    CHECK_EQ(s.days_left, int64_t(30));
    std::printf("  有效：%s\n", s.text.c_str());
    CHECK(st(nullptr, m, kNow).state == State::None);
    const std::string empty;
    CHECK(st(&empty, m, kNow).state == State::None);
    // 到期、未生效
    CHECK(st(&text, m, kNow + 30 * kDay).state == State::Expired);
    CHECK(st(&text, m, kNow + 30 * kDay - 1).state == State::Valid);
    CHECK(st(&text, m, kNow - 12 * kDay, 0).state == State::ClockRollback);  // 早于签发时间一天以上
    // 换了一项硬件：仍有效；换了两项：与本机不符
    auto one = m, two = m;
    one[4] = fp_item("mac", "m-2");
    two[4] = fp_item("mac", "m-2");
    two[3] = fp_item("disk", "d-2");
    s = st(&text, one, kNow);
    CHECK(s.state == State::Valid && s.matched == 5 && s.total == 6);
    s = st(&text, two, kNow);
    CHECK(s.state == State::WrongMachine && !s.can_compress());
    std::printf("  换硬件：%s\n", s.text.c_str());
    auto missing = m;  // 某项取不到（如网卡被禁用）也算一项变化
    missing.pop_back();
    CHECK(st(&text, missing, kNow).state == State::Valid);
    // 系统时间回拨：比此前见过的时间早 2 小时以上
    s = st(&text, m, kNow, kNow + 3 * kDay);
    CHECK(s.state == State::ClockRollback);
    std::printf("  回拨：%s\n", s.text.c_str());
    CHECK(st(&text, m, kNow, kNow + 3600).state == State::Valid);  // 对时误差内
    // 永久授权
    License p = sample(m);
    p.expires = 0;
    const std::string perm = issue(p, sk);
    s = st(&perm, m, kNow + 3650 * kDay);
    CHECK(s.state == State::Valid && s.days_left == -1);
    // 未到生效日
    License f = sample(m);
    f.not_before = kNow + 5 * kDay;
    const std::string future = issue(f, sk);
    CHECK(st(&future, m, kNow).state == State::NotYet);
    // 签名无效优先于其他判断
    std::string bad = text;
    bad[bad.find("某设备厂商")] = 'X';
    CHECK(st(&bad, m, kNow).state == State::Invalid);
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    const std::string filter = argc > 1 ? argv[1] : "";
    for (const auto& c : testing::registry()) {
        if (std::string(c.name).find(filter) == std::string::npos) continue;
        std::printf("运行 %s\n", c.name);
        const int before = testing::failures();
        c.fn();
        std::printf("%s %s\n", testing::failures() == before ? "通过" : "失败", c.name);
    }
    std::printf("共 %zu 项，失败断言 %d 个\n", testing::registry().size(), testing::failures());
    return testing::failures() ? 1 : 0;
}
