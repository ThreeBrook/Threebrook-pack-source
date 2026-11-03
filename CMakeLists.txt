# IIS 产品原生工程（阶段 1 起）。阶段 0 的 Python 评估工具在 bench/，不参与这里的构建。
cmake_minimum_required(VERSION 3.21)

project(iis VERSION 0.3.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 第三方库一律静态链入 iis.dll / libiis.so；Windows 用静态 CRT，干净系统无需装 VC++ 运行库
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(IIS_BUILD_TESTS "构建单元测试" ON)
option(IIS_BUILD_TOOLS "构建开发工具（iis-perf）" ON)

if(MSVC)
  add_compile_options(/utf-8)   # 源码与头文件均为 UTF-8（含中文注释与提示）
endif()

include(cmake/deps.cmake)
include(cmake/version.cmake)

enable_testing()
add_subdirectory(src/core)
add_subdirectory(src/license)  # 离线授权
add_subdirectory(src/cli)
add_subdirectory(decoder)   # 独立开源解码器（复用上面锁定的依赖）
if(WIN32)
  add_subdirectory(src/service)   # 设备内嵌版后台服务（阶段 1 只做 Windows）
  add_subdirectory(src/viewer)    # 免安装查看器
endif()
if(IIS_BUILD_TOOLS)
  add_subdirectory(src/tools)
endif()
if(WIN32)
  # 程序版本信息（文件属性、安装包、代码签名）；iis-decode 在 decoder/ 里用它自己的版本号
  iis_version_info(iis "三溪缩影 压缩内核库")
  iis_version_info(iis-cli "三溪缩影 命令行工具")
  iis_version_info(iis-service "IIS 后台压缩服务")
  iis_version_info(iis-viewer "三溪缩影")
  iis_version_info(iis_thumb "三溪缩影 资源管理器缩略图")
  iis_version_info(iis-license "IIS 授权签发工具（内部）")
  # 安装包由 scripts/build-installer.ps1 用 WiX 生成（installer/iis.wxs），不经过 CMake
endif()
