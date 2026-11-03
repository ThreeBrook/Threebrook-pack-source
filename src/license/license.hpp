// 离线授权（T9）：机器指纹、机器码、授权文件签发与校验、授权状态判定。
//
// 流程：设备上取“机器码”（指纹各项的哈希，不含原始序列号）→ 发给我方 → 我方用私钥签发授权文件（Ed25519，RFC 8032）
// → 设备导入；服务用编译进程序的公钥校验签名，并核对本机指纹（允许一项变化）、有效期与系统时间。
// 未授权 / 过期 / 时间异常时只是不压缩新文件；已压缩文件的读取、还原、导出永远可用（查看器、iis-cli、iis-decode 不检查授权）。
#pragma once

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace iis::lic {

using Key32 = std::array<uint8_t, 32>;
using Secret64 = std::array<uint8_t, 64>;

// ---- 机器指纹 ----
struct FpItem {
    std::string kind;  // board_uuid / board_serial / cpu / disk / mac / os
    std::string hash;  // SHA-256("iis-fp-v1|kind|值") 前 16 位十六进制
};
std::vector<FpItem> fingerprint();  // 本机（取不到的项省略）
FpItem fp_item(const std::string& kind, const std::string& value);
std::string machine_code(const std::vector<FpItem>& items);  // "IIS1-" + base64url(JSON)
bool parse_machine_code(const std::string& code, std::vector<FpItem>& out, std::string& err);
// 授权里的指纹与本机比对：一致项数 / 授权记录的项数
int match_count(const std::vector<FpItem>& licensed, const std::vector<FpItem>& here);
bool machine_ok(const std::vector<FpItem>& licensed, const std::vector<FpItem>& here);  // 最多一项不一致，且至少 2 项一致

// ---- 授权文件 ----
struct License {
    std::string id, customer, device, tier = "standard";
    int64_t issued = 0, not_before = 0, expires = 0;  // Unix 秒（UTC）；expires 为 0 表示永久
    std::vector<FpItem> machine;
    std::vector<std::string> features;
};
// 生成密钥对（秘钥 64 字节 = 种子 32 + 公钥 32）
void keygen(Secret64& secret, Key32& pub);
// 签发：返回授权文件文本（JSON）。签名覆盖 license 对象的规范化 JSON（键排序、紧凑）
std::string issue(const License& lic, const Secret64& secret);
// 校验签名与格式（不判断有效期与机器）
bool verify(const std::string& text, const Key32& pub, License& out, std::string& err);

// ---- 状态 ----
enum class State { Valid, None, Invalid, WrongMachine, NotYet, Expired, ClockRollback };
struct Status {
    State state = State::None;
    std::string text;   // 中文说明
    License lic;
    int matched = 0, total = 0;
    int64_t days_left = -1;  // 永久为 -1
    bool can_compress() const { return state == State::Valid; }
};
// now 与 high_water 为 Unix 秒；high_water 为此前观察到的最大时间（防系统时间回拨），没有为 0
Status evaluate(const std::string* text, const Key32& pub, const std::vector<FpItem>& here, int64_t now, int64_t high_water);
const char* state_name(State s);

// 编译进产品的公钥（CMake 选项 IIS_LICENSE_PUBKEY）；dev_key() 为真表示仍是开发用公钥，正式发布前必须替换
const Key32& product_key();
bool dev_key();

std::string to_hex(const uint8_t* d, size_t n);
bool from_hex(const std::string& h, uint8_t* out, size_t n);
std::string date_text(int64_t unix_seconds);  // 本地日期 YYYY-MM-DD

}  // namespace iis::lic
