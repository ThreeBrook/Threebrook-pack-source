# 第三方依赖：版本与源码包 SHA-256 锁定在这里，改动须同步 THIRD_PARTY.md。
#
# 源码包先找项目内缓存 .cache/deps/（离线构建、Docker 构建共用），没有再从官方地址下载到该目录，
# 下载后按 SHA-256 校验，不一致即失败。不使用系统中已安装的同名库。
include(FetchContent)

set(IIS_DEPS_DIR "${CMAKE_SOURCE_DIR}/.cache/deps" CACHE PATH "第三方源码包缓存目录")
set(FETCHCONTENT_QUIET ON)
set(FETCHCONTENT_TRY_FIND_PACKAGE_MODE NEVER)

function(iis_fetch_archive name file url sha256)
  set(path "${IIS_DEPS_DIR}/${file}")
  if(NOT EXISTS "${path}")
    message(STATUS "下载 ${file} ← ${url}")
    file(DOWNLOAD "${url}" "${path}" EXPECTED_HASH SHA256=${sha256} TLS_VERIFY ON STATUS st)
    list(GET st 0 code)
    if(NOT code EQUAL 0)
      file(REMOVE "${path}")
      message(FATAL_ERROR "下载 ${file} 失败：${st}")
    endif()
  endif()
  set(${name}_ARCHIVE "${path}" PARENT_SCOPE)
endfunction()

# ---- CharLS（JPEG-LS，BSD-3-Clause）----
# 2.4.5 修复了截断码流上的死循环与 ≤2.4.4 的安全漏洞（GHSA-vx55-856r-qqxw），不得回退
set(IIS_CHARLS_VERSION 2.4.5)
set(IIS_CHARLS_SHA256 b55c742ef96b5e9d63f813a75d0ba29944302abb5a6ab59756ad61fa017b1dd3)
iis_fetch_archive(CHARLS "charls-${IIS_CHARLS_VERSION}.tar.gz"
  "https://github.com/team-charls/charls/archive/refs/tags/${IIS_CHARLS_VERSION}.tar.gz"
  ${IIS_CHARLS_SHA256})
set(CHARLS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CHARLS_BUILD_FUZZ_TEST OFF CACHE BOOL "" FORCE)
set(CHARLS_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(CHARLS_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(charls URL "${CHARLS_ARCHIVE}" URL_HASH SHA256=${IIS_CHARLS_SHA256}
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

# ---- zstd（BSD-3-Clause，双许可中选 BSD）----
set(IIS_ZSTD_VERSION 1.5.7)
set(IIS_ZSTD_SHA256 eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3)
iis_fetch_archive(ZSTD "zstd-${IIS_ZSTD_VERSION}.tar.gz"
  "https://github.com/facebook/zstd/releases/download/v${IIS_ZSTD_VERSION}/zstd-${IIS_ZSTD_VERSION}.tar.gz"
  ${IIS_ZSTD_SHA256})
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)   # 单文件单线程，并行在文件之间做
set(ZSTD_USE_STATIC_RUNTIME ON CACHE BOOL "" FORCE)
FetchContent_Declare(zstd URL "${ZSTD_ARCHIVE}" URL_HASH SHA256=${IIS_ZSTD_SHA256}
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR build/cmake)

FetchContent_MakeAvailable(charls zstd)

# zstd 的 CMake 不给静态库目标挂头文件目录，这里补上
FetchContent_GetProperties(zstd SOURCE_DIR IIS_ZSTD_SOURCE_DIR)
target_include_directories(libzstd_static INTERFACE "$<BUILD_INTERFACE:${IIS_ZSTD_SOURCE_DIR}/lib>")

# ---- nlohmann/json（MIT，单头文件）：配方 JSON 解析 ----
set(IIS_JSON_VERSION 3.12.0)
set(IIS_JSON_SHA256 aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63)
iis_fetch_archive(JSON "json-${IIS_JSON_VERSION}.hpp"
  "https://github.com/nlohmann/json/releases/download/v${IIS_JSON_VERSION}/json.hpp"
  ${IIS_JSON_SHA256})
configure_file("${JSON_ARCHIVE}" "${CMAKE_BINARY_DIR}/_deps/json-include/nlohmann/json.hpp" COPYONLY)
add_library(iis_json INTERFACE)
target_include_directories(iis_json SYSTEM INTERFACE "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/_deps/json-include>")

# ---- zlib（zlib 许可）：TIFF ZIP 条带解压与复现式重编码（经典 zlib，与 OpenCV / libtiff 写出的条带逐字节一致）----
set(IIS_ZLIB_VERSION 1.3.2)
set(IIS_ZLIB_SHA256 bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16)
iis_fetch_archive(ZLIB "zlib-${IIS_ZLIB_VERSION}.tar.gz"
  "https://github.com/madler/zlib/releases/download/v${IIS_ZLIB_VERSION}/zlib-${IIS_ZLIB_VERSION}.tar.gz"
  ${IIS_ZLIB_SHA256})
set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZLIB_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zlib URL "${ZLIB_ARCHIVE}" URL_HASH SHA256=${IIS_ZLIB_SHA256} DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

# ---- libdeflate（MIT）：复现 tifffile / imagecodecs 写出的 ZIP 条带 ----
set(IIS_LIBDEFLATE_VERSION 1.26)
set(IIS_LIBDEFLATE_SHA256 bba03fffc5538576213675ce6968fcff6ce2e67d82e4d5febea2d05f9f13cf85)
iis_fetch_archive(LIBDEFLATE "libdeflate-${IIS_LIBDEFLATE_VERSION}.tar.gz"
  "https://github.com/ebiggers/libdeflate/archive/refs/tags/v${IIS_LIBDEFLATE_VERSION}.tar.gz"
  ${IIS_LIBDEFLATE_SHA256})
set(LIBDEFLATE_BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_GZIP_SUPPORT OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_GZIP OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libdeflate URL "${LIBDEFLATE_ARCHIVE}" URL_HASH SHA256=${IIS_LIBDEFLATE_SHA256}
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

FetchContent_MakeAvailable(zlib libdeflate)
FetchContent_GetProperties(zlib SOURCE_DIR IIS_ZLIB_SOURCE_DIR BINARY_DIR IIS_ZLIB_BINARY_DIR)
FetchContent_GetProperties(libdeflate SOURCE_DIR IIS_LIBDEFLATE_SOURCE_DIR)
add_library(iis_zlib INTERFACE)
target_link_libraries(iis_zlib INTERFACE zlibstatic)
target_include_directories(iis_zlib INTERFACE "$<BUILD_INTERFACE:${IIS_ZLIB_SOURCE_DIR}>" "$<BUILD_INTERFACE:${IIS_ZLIB_BINARY_DIR}>")
add_library(iis_libdeflate INTERFACE)
target_link_libraries(iis_libdeflate INTERFACE libdeflate_static)
target_include_directories(iis_libdeflate INTERFACE "$<BUILD_INTERFACE:${IIS_LIBDEFLATE_SOURCE_DIR}>")

# ---- SQLite（公有领域，合并版单文件）：服务的本地索引（T6）----
# 官方下载页公布 SHA3-256 92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30，已核对；这里锁 SHA-256
set(IIS_SQLITE_VERSION 3540100)
set(IIS_SQLITE_SHA256 0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9)
iis_fetch_archive(SQLITE "sqlite-amalgamation-${IIS_SQLITE_VERSION}.zip"
  "https://www.sqlite.org/2026/sqlite-amalgamation-${IIS_SQLITE_VERSION}.zip"
  ${IIS_SQLITE_SHA256})
FetchContent_Declare(sqlite URL "${SQLITE_ARCHIVE}" URL_HASH SHA256=${IIS_SQLITE_SHA256} DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(sqlite)  # 包里没有 CMakeLists.txt，只解压
FetchContent_GetProperties(sqlite SOURCE_DIR IIS_SQLITE_SOURCE_DIR)
add_library(iis_sqlite STATIC "${IIS_SQLITE_SOURCE_DIR}/sqlite3.c")
target_include_directories(iis_sqlite SYSTEM PUBLIC "$<BUILD_INTERFACE:${IIS_SQLITE_SOURCE_DIR}>")
target_compile_definitions(iis_sqlite PRIVATE
  SQLITE_THREADSAFE=1 SQLITE_DQS=0 SQLITE_OMIT_LOAD_EXTENSION SQLITE_OMIT_DEPRECATED
  SQLITE_DEFAULT_WAL_SYNCHRONOUS=1 SQLITE_DEFAULT_FOREIGN_KEYS=1)
if(MSVC)
  target_compile_options(iis_sqlite PRIVATE /W0)
endif()

# ---- Dear ImGui（MIT）：免安装查看器的界面（T8，只在 Windows 构建）----
# v1.92.9b（2026-07-31）；1.92 起字体字形按需生成，中文无需预先指定字符范围。GitHub 标签源码包，锁定 SHA-256
if(WIN32)
  set(IIS_IMGUI_VERSION 1.92.9b)
  set(IIS_IMGUI_SHA256 21d8a0a565e85dce943e375db00812c2f3f0ab21f3f0f7964e364a63422d7f99)
  iis_fetch_archive(IMGUI "imgui-${IIS_IMGUI_VERSION}.tar.gz"
    "https://github.com/ocornut/imgui/archive/refs/tags/v${IIS_IMGUI_VERSION}.tar.gz" ${IIS_IMGUI_SHA256})
  FetchContent_Declare(imgui URL "${IMGUI_ARCHIVE}" URL_HASH SHA256=${IIS_IMGUI_SHA256} DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(imgui)  # 包里没有 CMakeLists.txt，只解压
  FetchContent_GetProperties(imgui SOURCE_DIR IIS_IMGUI_DIR)
  add_library(iis_imgui STATIC
    "${IIS_IMGUI_DIR}/imgui.cpp" "${IIS_IMGUI_DIR}/imgui_draw.cpp" "${IIS_IMGUI_DIR}/imgui_tables.cpp"
    "${IIS_IMGUI_DIR}/imgui_widgets.cpp" "${IIS_IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp"
    "${IIS_IMGUI_DIR}/backends/imgui_impl_win32.cpp" "${IIS_IMGUI_DIR}/backends/imgui_impl_dx11.cpp")
  target_include_directories(iis_imgui SYSTEM PUBLIC "${IIS_IMGUI_DIR}" "${IIS_IMGUI_DIR}/backends")
  target_compile_definitions(iis_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
  target_link_libraries(iis_imgui PUBLIC d3d11 d3dcompiler dxgi dwmapi)
  if(MSVC)
    target_compile_options(iis_imgui PRIVATE /W0)
  endif()
endif()

# ---- Monocypher（CC0 / BSD-2-Clause 双许可，按 BSD-2 使用）：离线授权的 Ed25519 签名（T9）----
# 4.0.3（2026-06-15）修复了 EdDSA / Ed25519 签名的计时泄露；只用其 Ed25519（RFC 8032，SHA-512）可选模块
set(IIS_MONOCYPHER_VERSION 4.0.3)
set(IIS_MONOCYPHER_SHA256 8cc9bc341a66249016db9bd70e9142d8d0aef9945973744b1ac05dbc55d8ee66)
iis_fetch_archive(MONOCYPHER "monocypher-${IIS_MONOCYPHER_VERSION}.tar.gz"
  "https://github.com/LoupVaillant/Monocypher/releases/download/${IIS_MONOCYPHER_VERSION}/monocypher-${IIS_MONOCYPHER_VERSION}.tar.gz"
  ${IIS_MONOCYPHER_SHA256})
FetchContent_Declare(monocypher URL "${MONOCYPHER_ARCHIVE}" URL_HASH SHA256=${IIS_MONOCYPHER_SHA256} DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR _none)  # 不用它自带的 Makefile 构建
FetchContent_MakeAvailable(monocypher)
FetchContent_GetProperties(monocypher SOURCE_DIR IIS_MONOCYPHER_DIR)
add_library(iis_monocypher STATIC "${IIS_MONOCYPHER_DIR}/src/monocypher.c" "${IIS_MONOCYPHER_DIR}/src/optional/monocypher-ed25519.c")
target_include_directories(iis_monocypher SYSTEM PUBLIC "${IIS_MONOCYPHER_DIR}/src" "${IIS_MONOCYPHER_DIR}/src/optional")
if(MSVC)
  target_compile_options(iis_monocypher PRIVATE /W0)
endif()
