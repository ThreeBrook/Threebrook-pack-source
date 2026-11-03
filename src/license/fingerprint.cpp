// 机器指纹：取不到的项省略；所有值只以哈希形式进入机器码与授权文件。
// 不需要管理员权限（服务以 LocalSystem 运行，普通用户运行的工具也能取到相同的值）。
#include "license.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#  define NOMINMAX
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <windows.h>
#  include <intrin.h>
#  include <iphlpapi.h>
#  include <winioctl.h>
#  pragma comment(lib, "iphlpapi.lib")
#  pragma comment(lib, "advapi32.lib")
#endif

namespace iis::lic {

namespace {

std::string trim(std::string s) {
    while (!s.empty() && (isspace(uint8_t(s.back())) || s.back() == 0)) s.pop_back();
    size_t i = 0;
    while (i < s.size() && isspace(uint8_t(s[i]))) ++i;
    return s.substr(i);
}

// 厂商没填的占位值不能当指纹
bool meaningful(const std::string& v) {
    if (v.size() < 3) return false;
    std::string l = v;
    for (auto& c : l) c = char(tolower(uint8_t(c)));
    for (const char* bad : {"default string", "to be filled", "not specified", "none", "system serial number",
                            "0123456789", "not applicable", "oem", "n/a"})
        if (l.find(bad) != std::string::npos) return false;
    return l.find_first_not_of("0f-") != std::string::npos;  // 全 0 / 全 F
}

#ifdef _WIN32

void smbios(std::vector<FpItem>& out) {
    const UINT size = GetSystemFirmwareTable('RSMB', 0, nullptr, 0);
    if (!size) return;
    std::vector<uint8_t> b(size);
    if (GetSystemFirmwareTable('RSMB', 0, b.data(), size) != size || size < 8) return;
    const uint32_t len = *reinterpret_cast<const uint32_t*>(&b[4]);
    const uint8_t* p = &b[8];
    const uint8_t* end = p + std::min<size_t>(len, size - 8);
    while (p + 4 <= end) {
        const uint8_t type = p[0], flen = p[1];
        if (flen < 4 || p + flen > end) break;
        // 字符串区：每个字符串以 0 结尾，整组再以一个 0 结束；没有字符串时为两个 0
        std::vector<std::string> strs;
        const uint8_t* s = p + flen;
        while (s < end) {
            if (*s == 0) {
                ++s;
                break;
            }
            const char* c = reinterpret_cast<const char*>(s);
            const size_t n = strnlen(c, size_t(end - s));
            strs.emplace_back(c, n);
            s += n + 1;
        }
        if (strs.empty() && s < end && *s == 0) ++s;
        auto str = [&](uint8_t idx) { return idx && idx <= strs.size() ? trim(strs[idx - 1]) : std::string(); };
        if (type == 1 && flen >= 0x19) {
            const std::string uuid = to_hex(p + 8, 16);
            if (meaningful(uuid)) out.push_back(fp_item("board_uuid", uuid));
        } else if (type == 2 && flen >= 8) {
            const std::string serial = str(p[7]);
            if (meaningful(serial)) out.push_back(fp_item("board_serial", serial));
        } else if (type == 127) {
            break;
        }
        p = s;
    }
}

void cpu(std::vector<FpItem>& out) {
    int r[4];
    __cpuid(r, 0x80000000);
    std::string brand;
    if (unsigned(r[0]) >= 0x80000004u) {
        char b[49] = {};
        for (int i = 0; i < 3; ++i) __cpuid(reinterpret_cast<int*>(b + 16 * i), 0x80000002 + i);
        brand = trim(b);
    }
    __cpuid(r, 1);
    out.push_back(fp_item("cpu", brand + "|" + std::to_string(unsigned(r[0]))));  // 型号 + 家族 / 步进签名
}

void disk(std::vector<FpItem>& out) {
    wchar_t win[MAX_PATH];
    if (!GetWindowsDirectoryW(win, MAX_PATH)) return;
    const std::wstring vol = std::wstring(LR"(\\.\)") + win[0] + L":";
    HANDLE h = CreateFileW(vol.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    VOLUME_DISK_EXTENTS ext{};
    DWORD n = 0;
    const BOOL ok = DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, &ext, sizeof ext, &n, nullptr);
    CloseHandle(h);
    if (!ok || ext.NumberOfDiskExtents < 1) return;
    const std::wstring drive = LR"(\\.\PhysicalDrive)" + std::to_wstring(ext.Extents[0].DiskNumber);
    h = CreateFileW(drive.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    std::vector<uint8_t> buf(4096);
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, buf.data(), DWORD(buf.size()), &n, nullptr)) {
        const auto* d = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buf.data());
        if (d->SerialNumberOffset && d->SerialNumberOffset < n) {
            const std::string serial = trim(reinterpret_cast<const char*>(buf.data() + d->SerialNumberOffset));
            if (meaningful(serial)) out.push_back(fp_item("disk", serial));
        }
    }
    CloseHandle(h);
}

void mac(std::vector<FpItem>& out) {
    ULONG size = 32 * 1024;
    std::vector<uint8_t> buf(size);
    auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, a,
                             &size) != NO_ERROR)
        return;
    std::vector<std::string> macs;
    for (; a; a = a->Next) {
        if (a->PhysicalAddressLength != 6 || (a->IfType != IF_TYPE_ETHERNET_CSMACD && a->IfType != IF_TYPE_IEEE80211)) continue;
        const std::wstring d = a->Description ? a->Description : L"";
        bool virt = false;
        for (const wchar_t* v : {L"Virtual", L"Hyper-V", L"VMware", L"VirtualBox", L"VPN", L"TAP", L"Bluetooth", L"Loopback"})
            virt = virt || d.find(v) != std::wstring::npos;
        if (virt || (a->PhysicalAddress[0] & 0x02)) continue;  // 本地管理地址（随机 / 虚拟）
        macs.push_back(to_hex(a->PhysicalAddress, 6));
    }
    if (macs.empty()) return;
    std::sort(macs.begin(), macs.end());
    out.push_back(fp_item("mac", macs.front()));
}

void os(std::vector<FpItem>& out) {
    wchar_t v[64];
    DWORD n = sizeof v;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", L"MachineGuid",
                     RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, v, &n) == ERROR_SUCCESS) {
        char a[64];
        WideCharToMultiByte(CP_UTF8, 0, v, -1, a, sizeof a, nullptr, nullptr);
        out.push_back(fp_item("os", a));
    }
}

#else  // Linux（阶段 3 网关版；这里给出等价来源）

std::string read1(const char* path) {
    std::ifstream f(path);
    std::string s;
    std::getline(f, s);
    return trim(s);
}

#endif

}  // namespace

std::vector<FpItem> fingerprint() {
    std::vector<FpItem> out;
#ifdef _WIN32
    smbios(out);
    cpu(out);
    disk(out);
    mac(out);
    os(out);
#else
    if (const auto u = read1("/sys/class/dmi/id/product_uuid"); meaningful(u)) out.push_back(fp_item("board_uuid", u));
    if (const auto s = read1("/sys/class/dmi/id/board_serial"); meaningful(s)) out.push_back(fp_item("board_serial", s));
    {
        std::ifstream f("/proc/cpuinfo");
        std::string line;
        while (std::getline(f, line))
            if (line.rfind("model name", 0) == 0) {
                out.push_back(fp_item("cpu", trim(line.substr(line.find(':') + 1))));
                break;
            }
    }
    {  // 物理网卡 MAC：跳过回环、虚拟网卡与本地管理地址（容器、虚拟机里的随机地址）
        std::vector<std::string> macs;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator("/sys/class/net", ec)) {
            const std::string n = e.path().filename().string();
            if (n == "lo" || n.rfind("veth", 0) == 0 || n.rfind("docker", 0) == 0 || n.rfind("br-", 0) == 0 || n.rfind("virbr", 0) == 0)
                continue;
            const std::string m = read1((e.path() / "address").c_str());
            if (m.size() == 17 && !(std::stoi(m.substr(0, 2), nullptr, 16) & 0x02) && m != "00:00:00:00:00:00") macs.push_back(m);
        }
        std::sort(macs.begin(), macs.end());
        if (!macs.empty()) out.push_back(fp_item("mac", macs.front()));
    }
    if (const auto m = read1("/etc/machine-id"); meaningful(m)) out.push_back(fp_item("os", m));
#endif
    return out;
}

}  // namespace iis::lic
