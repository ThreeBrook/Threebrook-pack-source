#include "testing.hpp"

#include "iis/iis.h"

#ifdef _WIN32
#  define NOMINMAX
#  include <windows.h>
#endif

int fault_child_main(int argc, char** argv);  // test_commit.cpp：故障注入子进程

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--fault-child") return fault_child_main(argc, argv);
    const std::string filter = argc > 1 ? argv[1] : "";  // 只跑名字含该子串的用例
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // 测试进程崩溃时也能看到之前的失败信息
    std::printf("%s\n", iis_build_info());
    for (const auto& c : testing::registry()) {
        if (std::string(c.name).find(filter) == std::string::npos) continue;
        std::printf("运行 %s\n", c.name);
        std::fflush(stdout);
        const int before = testing::failures();
        c.fn();
        std::printf("%s %s\n", testing::failures() == before ? "通过" : "失败", c.name);
    }
    std::printf("共 %zu 项，失败断言 %d 个\n", testing::registry().size(), testing::failures());
    return testing::failures() ? 1 : 0;
}
