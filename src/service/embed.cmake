# 把 ui/index.html 转成 C++ 字节数组（服务程序单文件发布，不依赖外部网页文件）
# 用法：cmake -DIN=index.html -DOUT=ui_embed.cpp -P embed.cmake
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" n)
math(EXPR size "${n} / 2")
set(body "")
set(pos 0)
while(pos LESS n)
  string(SUBSTRING "${hex}" ${pos} 64 chunk)  # 每行 32 字节
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," line "${chunk}")
  string(APPEND body "${line}\n")
  math(EXPR pos "${pos} + 64")
endwhile()
file(WRITE "${OUT}" "// 由 src/service/ui/index.html 在构建时生成，勿手改\n#include <cstddef>\nnamespace iis::svc {\nextern const unsigned char kIndexHtml[] = {\n${body}0};\nextern const size_t kIndexHtmlSize = ${size};\n}  // namespace iis::svc\n")
