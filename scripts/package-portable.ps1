# 打包免安装查看器（单目录绿色版）：dist\ThreebrookPack-<版本>-免安装版-win-x64\ 与同名 .zip
#
#   powershell -ExecutionPolicy Bypass -File scripts\package-portable.ps1
#
# 目录内容：iis-viewer.exe（查看器）、iis-cli.exe（命令行）、iis-decode.exe（开源解码器）、使用说明.txt、许可证\。
# 三个程序只依赖 Windows 自带 DLL，拷到任何位置（含 U 盘）即可运行；不写注册表、不写配置文件。
# 先运行 scripts\ci-windows.ps1 构建并测试；本脚本只收集文件、做一次查看器自检、打 zip。
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot -Parent)

$version = (Select-String -Path CMakeLists.txt -Pattern 'project\(iis VERSION ([0-9.]+)').Matches[0].Groups[1].Value
$build = 'build\win-x64'
$name = "ThreebrookPack-$version-免安装版-win-x64"
$out = Join-Path 'dist' $name
$exes = @("$build\src\viewer\Release\iis-viewer.exe", "$build\src\cli\Release\iis-cli.exe", "$build\decoder\Release\iis-decode.exe")
foreach ($e in $exes) { if (-not (Test-Path $e)) { throw "缺少 $e，请先运行 scripts\ci-windows.ps1" } }

if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force "$out\许可证" | Out-Null
foreach ($e in $exes) { Copy-Item $e $out }

# 第三方许可证（BSD / MIT / zlib 要求随二进制附带）
. "$PSScriptRoot\licenses.ps1"
Copy-ThirdPartyLicenses -Build $build -Dest "$out\许可证"

@"
三溪缩影 $version（免安装版）

本目录可整个拷到任何位置（含 U 盘）直接运行，不需要安装，不写注册表，不写配置文件。
需要 Windows 10 / 11（64 位）；没有独立显卡也能用。

iis-viewer.exe   图形查看器
    打开 .iis 压缩文件（自动还原并核对 SHA-256）、.iisz 包（像文件夹一样浏览）以及 BMP、TIFF、RAW、PNG、JPEG；
    缩放（滚轮）、平移（拖动）、16 位窗宽窗位、Bayer 去马赛克预览；
    左侧浏览目录与缩略图；“文件 → 批量还原”把整个目录的 .iis 还原为原文件（不覆盖已有文件）；
    打开 .iis 后可“另存为原文件”。也可以把文件拖进窗口，或在命令行写：iis-viewer 文件或目录

iis-cli.exe      命令行工具（在命令提示符或 PowerShell 中运行）
    iis-cli pack 文件夹                            打包为 文件夹.iisz（默认保留原文件）
    iis-cli unpack 包.iisz                         解到包旁边（不覆盖）
    iis-cli list 包.iisz                           列出包内文件
    iis-cli info 文件.iis                          查看压缩文件信息
    iis-cli verify 目录 -r --deep                  校验（--deep 完整还原并比对 SHA-256）
    iis-cli restore 目录 -r -o 输出目录             批量还原为原文件
    iis-cli compress 目录 -r                       手动压缩（默认保留原文件）
    iis-cli export 源目录 输出目录 --from 2026-11-01 --to 2026-11-02   按日期导出并还原

iis-decode.exe   开源解码器（BSD-3-Clause，源码公开）
    iis-decode 文件.iis              还原到同目录
    iis-decode --check 文件.iis      只校验能否还原
    不依赖 IIS 设备或其他软件，也能把 .iis 还原为与原文件逐字节相同的文件。

所有还原都会核对原文件的 SHA-256，不一致时报错且不留下文件。
“许可证”目录为所用开源组件的许可证。
"@ | Out-File -Encoding utf8 "$out\使用说明.txt"

# 自检：从打包目录运行查看器（无窗口渲染一帧并截图），确认没有遗漏文件
New-Item -ItemType Directory -Force .tmp | Out-Null
$shot = Join-Path (Resolve-Path .tmp) 'portable-selftest.bmp'
Remove-Item $shot -ErrorAction SilentlyContinue
$p = Start-Process (Resolve-Path "$out\iis-viewer.exe") -ArgumentList '--render', "`"$shot`"" -WorkingDirectory $out -Wait -PassThru
if ($p.ExitCode -ne 0 -or -not (Test-Path $shot)) { throw '打包目录中的查看器自检失败' }
Remove-Item $shot
# 自检后目录里不应多出任何文件（证明不写配置）
$extra = Get-ChildItem $out -File | Where-Object { $_.Name -notin @('iis-viewer.exe', 'iis-cli.exe', 'iis-decode.exe', '使用说明.txt') }
if ($extra) { throw "查看器在自己的目录里写了文件：$($extra.Name -join ', ')" }

$zip = "dist\$name.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path $out -DestinationPath $zip
$size = (Get-ChildItem $out -Recurse -File | Measure-Object Length -Sum).Sum / 1MB
Write-Host ("绿色版：{0}（{1:N1} MB）" -f (Resolve-Path $out), $size)
Write-Host ("压缩包：{0}（{1:N1} MB）" -f (Resolve-Path $zip), ((Get-Item $zip).Length / 1MB))
