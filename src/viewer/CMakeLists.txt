# 查看器（T8）与右键压缩 / 解压窗口：iis_view 为图像读取、显示准备与压缩解压任务（可测试），
# iis-viewer 为界面（Dear ImGui + D3D11）
add_library(iis_view STATIC image.cpp tasks.cpp i18n.cpp)
target_link_libraries(iis_view PUBLIC iis_static iis_json)
target_include_directories(iis_view PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" PRIVATE "${CMAKE_SOURCE_DIR}/src/core/src")
target_compile_options(iis_view PRIVATE /W4)

# 图标：程序、.iis 文件、.iisz 包（tools/make_icons.py 生成）；界面字体（三溪外观规范，SIL OFL）
set(IIS_ICON_DIR "${CMAKE_SOURCE_DIR}/assets/icons")
set(IIS_FONT_DIR "${CMAKE_SOURCE_DIR}/assets/fonts")
configure_file(icons.rc.in "${CMAKE_CURRENT_BINARY_DIR}/iis-viewer_icons.rc" @ONLY)

add_executable(iis-viewer WIN32 main.cpp theme.cpp "${CMAKE_CURRENT_BINARY_DIR}/iis-viewer_icons.rc")
target_link_libraries(iis-viewer PRIVATE iis_view iis_imgui)
target_include_directories(iis-viewer PRIVATE "${CMAKE_SOURCE_DIR}/src/core/src")
target_compile_options(iis-viewer PRIVATE /W4)

# 资源管理器缩略图处理程序（.iis 显示图片本身的预览，.iisz 包显示前 4 张图的拼图），由安装包注册
add_library(iis_thumb MODULE thumb_provider.cpp thumb_provider.def)
target_link_libraries(iis_thumb PRIVATE iis_view)
target_include_directories(iis_thumb PRIVATE "${CMAKE_SOURCE_DIR}/src/core/src")
target_compile_options(iis_thumb PRIVATE /W4)

if(IIS_BUILD_TESTS)
  # 像资源管理器那样调用缩略图处理程序（不注册、不需要管理员）
  add_executable(iis_thumb_check tests/thumb_check.cpp)
  target_link_libraries(iis_thumb_check PRIVATE iis_static)
  target_include_directories(iis_thumb_check PRIVATE "${CMAKE_SOURCE_DIR}/src/core/src")
  target_compile_options(iis_thumb_check PRIVATE /W4)
  add_dependencies(iis_thumb_check iis_thumb)
  add_test(NAME iis_thumb_check COMMAND iis_thumb_check $<TARGET_FILE:iis_thumb>)

  add_executable(iis_viewer_tests tests/test_viewer.cpp)
  target_link_libraries(iis_viewer_tests PRIVATE iis_view)
  target_include_directories(iis_viewer_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/core/tests" "${CMAKE_SOURCE_DIR}/src/core/src")
  target_compile_options(iis_viewer_tests PRIVATE /W4)
  add_test(NAME iis_viewer_tests COMMAND iis_viewer_tests)
endif()
