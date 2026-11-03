add_executable(iis-perf iis_perf.cpp)
target_link_libraries(iis-perf PRIVATE iis_static)
if(MSVC)
  target_compile_options(iis-perf PRIVATE /utf-8 /W4)
endif()

# 对照测试用的开发工具：直接链接内核内部函数
add_executable(iis-devtool iis_devtool.cpp)
target_link_libraries(iis-devtool PRIVATE iis_static iis_json)
target_include_directories(iis-devtool PRIVATE "${CMAKE_SOURCE_DIR}/src/core/src")
if(MSVC)
  target_compile_options(iis-devtool PRIVATE /utf-8 /W4)
endif()

# T5 验收用的负载模拟（检测节拍 + 写图 + 完整性核对），只在 Windows 构建
if(WIN32)
  add_executable(iis-loadsim iis_loadsim.cpp)
  target_link_libraries(iis-loadsim PRIVATE iis_static)
  target_compile_options(iis-loadsim PRIVATE /utf-8 /W4)
endif()
