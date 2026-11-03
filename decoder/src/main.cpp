// iis-decode 命令行：
//   iis-decode 文件.iis…              还原到同目录（去掉 .iis；已存在不覆盖）
//   iis-decode 文件.iis -o 输出文件     还原到指定文件
//   iis-decode --check 文件.iis…       只校验能否还原（不写文件）
// 退出码：0 全部成功；1 有失败；2 用法错误。
#include "iis_decode.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
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

namespace {

std::string u8(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

int run(const std::vector<std::string>& args) {
    bool check = false;
    std::string out;
    std::vector<std::string> files;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--check") check = true;
        else if (args[i] == "-o" && i + 1 < args.size()) out = args[++i];
        else files.push_back(args[i]);
    }
    if (files.empty() || (!out.empty() && files.size() != 1)) {
        std::printf("用法：iis-decode 文件.iis… | iis-decode 文件.iis -o 输出文件 | iis-decode --check 文件.iis…\n");
        return 2;
    }
    int bad = 0;
    for (const auto& f : files) {
        const fs::path in = fs::u8path(f);
        std::ifstream s(in, std::ios::binary);
        const std::vector<uint8_t> data((std::istreambuf_iterator<char>(s)), std::istreambuf_iterator<char>());
        if (!s.good() && data.empty()) {
            std::printf("%s：无法读取\n", f.c_str());
            ++bad;
            continue;
        }
        std::vector<uint8_t> orig;
        std::string err;
        if (!iis_decode::restore(data.data(), data.size(), orig, err)) {
            std::printf("%s：%s\n", f.c_str(), err.c_str());
            ++bad;
            continue;
        }
        const std::string sha = iis_decode::sha256_hex(orig.data(), orig.size());
        if (check) {
            std::printf("%s：可还原，%zu 字节，SHA-256 %s\n", f.c_str(), orig.size(), sha.c_str());
            continue;
        }
        fs::path target = out.empty() ? in : fs::u8path(out);
        if (out.empty()) target.replace_extension();
        std::error_code ec;
        if (fs::exists(target, ec)) {
            std::printf("%s：目标已存在，不覆盖\n", u8(target).c_str());
            ++bad;
            continue;
        }
        const fs::path tmp = target.native() + fs::path(".partial").native();
        {
            std::ofstream o(tmp, std::ios::binary);
            o.write(reinterpret_cast<const char*>(orig.data()), std::streamsize(orig.size()));
            if (!o) {
                std::printf("%s：写入失败\n", u8(target).c_str());
                fs::remove(tmp, ec);
                ++bad;
                continue;
            }
        }
        fs::rename(tmp, target, ec);
        if (ec) {
            std::printf("%s：写入失败\n", u8(target).c_str());
            fs::remove(tmp, ec);
            ++bad;
            continue;
        }
        std::printf("%s → %s（SHA-256 %s）\n", f.c_str(), u8(target).c_str(), sha.c_str());
    }
    return bad ? 1 : 0;
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
