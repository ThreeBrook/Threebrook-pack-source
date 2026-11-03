# libiis 内核库：
#   iis_static —— 静态库，供单元测试与将来的服务、查看器、命令行直接链接
#   iis        —— 动态库（Windows iis.dll / Linux libiis.so），对外只导出 iis.h 中的 C 接口
find_package(Threads REQUIRED)

set(IIS_CORE_SOURCES
  src/api.cpp
  src/commit.cpp
  src/container.cpp
  src/container_api.cpp
  src/crc32c.cpp
  src/detect.cpp
  src/formats.cpp
  src/fs.cpp
  src/io.cpp
  src/jpegls.cpp
  src/layout.cpp
  src/package.cpp
  src/pipeline.cpp
  src/pool.cpp
  src/rawguess.cpp
  src/sha256.cpp
  src/tiffcodec.cpp
  src/zstd.cpp)

function(iis_core_options target)
  target_include_directories(${target} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
                                       PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_definitions(${target} PRIVATE IIS_VERSION_STRING="${PROJECT_VERSION}")
  target_link_libraries(${target} PRIVATE charls libzstd_static iis_json iis_zlib iis_libdeflate)
  target_link_libraries(${target} PUBLIC Threads::Threads)
  if(MSVC)
    target_compile_options(${target} PRIVATE /utf-8 /W4 /permissive-)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra)
  endif()
endfunction()

add_library(iis_static STATIC ${IIS_CORE_SOURCES})
iis_core_options(iis_static)
target_compile_definitions(iis_static PUBLIC IIS_STATIC)

add_library(iis SHARED ${IIS_CORE_SOURCES})
iis_core_options(iis)
target_compile_definitions(iis PRIVATE IIS_BUILDING)
set_target_properties(iis PROPERTIES
  C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
  VERSION ${PROJECT_VERSION} SOVERSION 1)
if(NOT WIN32 AND NOT APPLE)
  # 不把静态链入的 CharLS / zstd 符号再导出去，避免与宿主进程里的同名库冲突
  target_link_options(iis PRIVATE "LINKER:--exclude-libs,ALL" "LINKER:--no-undefined"
                                  "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/iis.map")
  set_target_properties(iis PROPERTIES LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/iis.map")
endif()

if(IIS_BUILD_TESTS)
  add_subdirectory(tests)
endif()
