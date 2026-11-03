# 离线授权（T9）：iis_license 库（指纹、机器码、签发与校验、状态判定），iis-license 签发工具（内部使用）
#
# 授权校验公钥编译进产品。默认是开发用公钥（私钥只在开发机 .cache/license-dev/，不入库）；
# 正式发布必须用负责人离线保管的生产密钥：iis-license keygen 生成后，构建时传 -DIIS_LICENSE_PUBKEY=<公钥>。
set(IIS_LICENSE_DEV_PUBKEY "f65a0d87912c515a930b83ad3286fc169a955289f4fa29a55b7c89e6cc6a38df")
set(IIS_LICENSE_PUBKEY "${IIS_LICENSE_DEV_PUBKEY}" CACHE STRING "授权校验公钥（32 字节十六进制）")

add_library(iis_license STATIC license.cpp fingerprint.cpp)
target_include_directories(iis_license PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(iis_license PUBLIC iis_static iis_json PRIVATE iis_monocypher)
target_compile_definitions(iis_license PRIVATE IIS_LICENSE_PUBKEY="${IIS_LICENSE_PUBKEY}" _CRT_SECURE_NO_WARNINGS)
if(IIS_LICENSE_PUBKEY STREQUAL IIS_LICENSE_DEV_PUBKEY)
  target_compile_definitions(iis_license PRIVATE IIS_LICENSE_DEV_KEY)
endif()

add_executable(iis-license tool.cpp)
target_link_libraries(iis-license PRIVATE iis_license)
target_compile_definitions(iis-license PRIVATE _CRT_SECURE_NO_WARNINGS)

if(MSVC)
  target_compile_options(iis_license PRIVATE /W4)
  target_compile_options(iis-license PRIVATE /W4)
else()
  target_compile_options(iis_license PRIVATE -Wall -Wextra)
  target_compile_options(iis-license PRIVATE -Wall -Wextra)
endif()

if(IIS_BUILD_TESTS)
  add_executable(iis_license_tests tests/test_license.cpp)
  target_link_libraries(iis_license_tests PRIVATE iis_license iis_monocypher)
  target_include_directories(iis_license_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/core/tests")
  add_test(NAME iis_license_tests COMMAND iis_license_tests)
endif()
