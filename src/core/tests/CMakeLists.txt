add_executable(iis_core_tests test_main.cpp test_checksum.cpp test_codecs.cpp test_container.cpp test_formats.cpp test_commit.cpp test_package.cpp test_rawguess.cpp)
target_link_libraries(iis_core_tests PRIVATE iis_static charls iis_json iis_decode_lib)  # 含开源解码器对照
target_include_directories(iis_core_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
if(MSVC)
  target_compile_options(iis_core_tests PRIVATE /utf-8 /W4)
endif()
target_compile_definitions(iis_core_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME iis_core_tests COMMAND iis_core_tests)

add_executable(iis_c_abi_smoke c_abi_smoke.c)
target_link_libraries(iis_c_abi_smoke PRIVATE iis)
add_test(NAME iis_c_abi_smoke COMMAND iis_c_abi_smoke)
if(WIN32)
  # 测试运行时要能找到 iis.dll
  add_custom_command(TARGET iis_c_abi_smoke POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:iis> $<TARGET_FILE_DIR:iis_c_abi_smoke>)
endif()
set_tests_properties(iis_core_tests iis_c_abi_smoke PROPERTIES TIMEOUT 300)  # 损坏数据解码卡死也会被判失败
