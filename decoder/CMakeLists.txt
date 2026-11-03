# iis-decode：独立开源解码器。可作为本仓库的子目录构建（复用上层已锁定的依赖），
# 也可单独构建：cmake -S decoder -B build && cmake --build build（按下面的地址与 SHA-256 下载依赖源码包）。
cmake_minimum_required(VERSION 3.21)
project(iis_decode VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT TARGET charls)
  # 单独构建：取与 IIS 相同版本的标准编码库
  include(FetchContent)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  set(CHARLS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(CHARLS_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
  set(CHARLS_BUILD_FUZZ_TEST OFF CACHE BOOL "" FORCE)
  set(CHARLS_INSTALL OFF CACHE BOOL "" FORCE)
  set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
  set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  set(LIBDEFLATE_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
  set(LIBDEFLATE_BUILD_GZIP OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(charls URL https://github.com/team-charls/charls/archive/refs/tags/2.4.5.tar.gz
    URL_HASH SHA256=b55c742ef96b5e9d63f813a75d0ba29944302abb5a6ab59756ad61fa017b1dd3)
  FetchContent_Declare(zstd URL https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
    URL_HASH SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3 SOURCE_SUBDIR build/cmake)
  FetchContent_Declare(zlib URL https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz
    URL_HASH SHA256=bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16)
  FetchContent_Declare(libdeflate URL https://github.com/ebiggers/libdeflate/archive/refs/tags/v1.26.tar.gz
    URL_HASH SHA256=bba03fffc5538576213675ce6968fcff6ce2e67d82e4d5febea2d05f9f13cf85)
  FetchContent_MakeAvailable(charls zstd zlib libdeflate)
  FetchContent_GetProperties(zstd SOURCE_DIR zstd_src)
  FetchContent_GetProperties(zlib SOURCE_DIR zlib_src BINARY_DIR zlib_bin)
  FetchContent_GetProperties(libdeflate SOURCE_DIR libdeflate_src)
  file(DOWNLOAD https://github.com/nlohmann/json/releases/download/v3.12.0/json.hpp
    "${CMAKE_BINARY_DIR}/json/nlohmann/json.hpp"
    EXPECTED_HASH SHA256=aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63)
  add_library(iis_json INTERFACE)
  target_include_directories(iis_json INTERFACE "${CMAKE_BINARY_DIR}/json")
  target_include_directories(libzstd_static INTERFACE "${zstd_src}/lib")
  add_library(iis_zlib INTERFACE)
  target_link_libraries(iis_zlib INTERFACE zlibstatic)
  target_include_directories(iis_zlib INTERFACE "${zlib_src}" "${zlib_bin}")
  add_library(iis_libdeflate INTERFACE)
  target_link_libraries(iis_libdeflate INTERFACE libdeflate_static)
  target_include_directories(iis_libdeflate INTERFACE "${libdeflate_src}")
endif()

add_library(iis_decode_lib STATIC src/iis_decode.cpp)
target_include_directories(iis_decode_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(iis_decode_lib PRIVATE charls libzstd_static iis_zlib iis_libdeflate iis_json)

add_executable(iis-decode src/main.cpp)
if(COMMAND iis_version_info)  # 作为 IIS 子目录构建时写入版本信息
  iis_version_info(iis-decode "IIS 开源解码器")
endif()
target_link_libraries(iis-decode PRIVATE iis_decode_lib)
if(MSVC)
  target_compile_options(iis_decode_lib PRIVATE /utf-8 /W4)
  target_compile_options(iis-decode PRIVATE /utf-8 /W4)
else()
  target_compile_options(iis_decode_lib PRIVATE -Wall -Wextra)
endif()
