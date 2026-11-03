# Windows 后台服务（阶段 1 T5）：
#   iis_svc         —— 引擎静态库（配置、日志、监控、调度、资源控制），供服务与测试链接
#   iis-service     —— 服务程序（控制台运行 / 注册服务 / 由服务控制管理器启动）
# 管理界面网页嵌入程序：ui/index.html → ui_embed.cpp
set(IIS_UI_HTML "${CMAKE_CURRENT_SOURCE_DIR}/ui/index.html")
set(IIS_UI_CPP "${CMAKE_CURRENT_BINARY_DIR}/ui_embed.cpp")
add_custom_command(OUTPUT "${IIS_UI_CPP}"
  COMMAND ${CMAKE_COMMAND} "-DIN=${IIS_UI_HTML}" "-DOUT=${IIS_UI_CPP}" -P "${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake"
  DEPENDS "${IIS_UI_HTML}" "${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake"
  COMMENT "嵌入管理界面网页")

add_library(iis_svc STATIC audit.cpp auth.cpp classify.cpp config.cpp engine.cpp host.cpp http.cpp index.cpp log.cpp storage.cpp
            "${IIS_UI_CPP}")
target_link_libraries(iis_svc PUBLIC iis_static iis_json iis_sqlite iis_license)
target_include_directories(iis_svc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
                                   PRIVATE "${CMAKE_SOURCE_DIR}/src/core/src")
target_compile_options(iis_svc PRIVATE /W4 /permissive-)

add_executable(iis-service main.cpp)
target_link_libraries(iis-service PRIVATE iis_svc)
target_compile_options(iis-service PRIVATE /W4)

if(IIS_BUILD_TESTS)
  add_executable(iis_service_tests tests/test_service.cpp)
  target_link_libraries(iis_service_tests PRIVATE iis_svc)
  target_include_directories(iis_service_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/core/tests")
  target_compile_options(iis_service_tests PRIVATE /W4)
  target_compile_definitions(iis_service_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
  add_test(NAME iis_service_tests COMMAND iis_service_tests)
  set_tests_properties(iis_service_tests PROPERTIES TIMEOUT 600)
endif()
