# 第三方许可证（BSD / MIT / zlib 等要求随二进制附带）。由 package-portable.ps1、build-installer.ps1 引用：
#   . "$PSScriptRoot\licenses.ps1"; Copy-ThirdPartyLicenses -Build build\win-x64 -Dest 目录
# SQLite 为公有领域，无须附带许可证（见 THIRD_PARTY.md）。
function Copy-ThirdPartyLicenses([string]$Build, [string]$Dest) {
    New-Item -ItemType Directory -Force $Dest | Out-Null
    $deps = "$Build\_deps"
    Copy-Item "$deps\charls-src\LICENSE.md" "$Dest\CharLS.txt"
    Copy-Item "$deps\zstd-src\LICENSE" "$Dest\zstd.txt"
    Copy-Item "$deps\zlib-src\LICENSE" "$Dest\zlib.txt"
    Copy-Item "$deps\libdeflate-src\COPYING" "$Dest\libdeflate.txt"
    Copy-Item "$deps\imgui-src\LICENSE.txt" "$Dest\Dear-ImGui.txt"
    Copy-Item "$deps\monocypher-src\LICENCE.md" "$Dest\Monocypher.txt"
    Copy-Item 'decoder\LICENSE' "$Dest\iis-decode（BSD-3-Clause，含 imagecodecs 算法声明）.txt"
    # 界面字体（SIL OFL 1.1，嵌在 iis-viewer.exe 里）
    Copy-Item 'assets\fonts\OFL-ZCOOLXiaoWei.txt' "$Dest\字体-站酷小薇 ZCOOL XiaoWei（OFL）.txt"
    Copy-Item 'assets\fonts\OFL-BricolageGrotesque.txt' "$Dest\字体-Bricolage Grotesque（OFL）.txt"
    Copy-Item 'assets\fonts\OFL-JetBrainsMono.txt' "$Dest\字体-JetBrains Mono（OFL）.txt"
    @'
MIT License

Copyright (c) 2013-2025 Niels Lohmann <https://nlohmann.me>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
'@ | Out-File -Encoding utf8 "$Dest\nlohmann-json.txt"
}
