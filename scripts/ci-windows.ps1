# Windows x64（MSVC）构建 + 单元测试 + 导出与依赖检查（本地 CI）。
#
#   powershell -ExecutionPolicy Bypass -File scripts\ci-windows.ps1 [-Parity]
#
# -Parity：再跑与阶段 0 Python 参考实现的逐字节对照（需要 .venv 与 bench/out/synth 合成样本，约 3 分钟）。
param([switch]$Parity)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw '未找到带 C++ 工具集的 Visual Studio 2022' }
$bin = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) { $cmake = "$bin\cmake.exe" }
$ctest = Join-Path (Split-Path $cmake) 'ctest.exe'
$build = 'build\win-x64'

& $cmake -S . -B $build -G 'Visual Studio 17 2022' -A x64 -Wno-dev
if ($LASTEXITCODE) { throw '配置失败' }
& $cmake --build $build --config Release -- /m /v:m
if ($LASTEXITCODE) { throw '构建失败' }
& $ctest --test-dir $build -C Release --output-on-failure
if ($LASTEXITCODE) { throw '单元测试失败' }

# dumpbin：只导出 iis_*；不依赖 VC++ 运行库（静态 CRT），干净系统可直接加载
$dumpbin = Get-ChildItem "$vs\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe" | Select-Object -Last 1
$dll = "$build\src\core\Release\iis.dll"
$exports = & $dumpbin.FullName /exports $dll | Select-String '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]{8}\s+(\S+)' |
    ForEach-Object { $_.Matches[0].Groups[1].Value }
$bad = $exports | Where-Object { $_ -notlike 'iis_*' }
if ($bad) { throw "iis.dll 导出了非 iis_ 符号：$bad" }
$deps = & $dumpbin.FullName /dependents $dll | Select-String '\.dll' | ForEach-Object { $_.Line.Trim() } |
    Where-Object { $_ -notlike '*iis.dll*' -and $_ -notlike 'Dump of*' }
$crt = $deps | Where-Object { $_ -match '^(vcruntime|msvcp|ucrtbase|api-ms-win-crt)' }
if ($crt) { throw "iis.dll 依赖 VC++ 运行库：$crt" }
Write-Host "导出检查通过：$($exports.Count) 个 iis_* 符号；依赖：$($deps -join ', ')"

# 免安装工具：只依赖 Windows 自带 DLL（不依赖 VC++ 运行库）
foreach ($exe in "$build\src\viewer\Release\iis-viewer.exe", "$build\src\cli\Release\iis-cli.exe",
                 "$build\decoder\Release\iis-decode.exe", "$build\src\service\Release\iis-service.exe",
                 "$build\src\viewer\Release\iis_thumb.dll") {
    $d = & $dumpbin.FullName /dependents $exe | Select-String '\.dll' | ForEach-Object { $_.Line.Trim() } |
        Where-Object { $_ -notlike 'Dump of*' }
    $bad = $d | Where-Object { $_ -match '^(vcruntime|msvcp|ucrtbase|api-ms-win-crt)' }
    if ($bad) { throw "$exe 依赖 VC++ 运行库：$bad" }
}
# 查看器自检：不显示窗口渲染一帧并截图（无显卡时走 WARP）
New-Item -ItemType Directory -Force .tmp | Out-Null  # 临时文件放项目内
$shot = Join-Path (Resolve-Path .tmp) "iis-viewer-ci.bmp"
Remove-Item $shot -ErrorAction SilentlyContinue
$p = Start-Process "$build\src\viewer\Release\iis-viewer.exe" -ArgumentList "--render", "`"$shot`"" -Wait -PassThru
if ($p.ExitCode -ne 0 -or -not (Test-Path $shot)) { throw '查看器自检失败' }
Remove-Item $shot
# 压缩窗口（右键“三溪缩影 压缩”）也能画出来
$p = Start-Process "$build\src\viewer\Release\iis-viewer.exe" -ArgumentList "--render", "`"$shot`"", "--compress", "`"$root\src`"" -Wait -PassThru
if ($p.ExitCode -ne 0 -or -not (Test-Path $shot)) { throw '压缩窗口自检失败' }
Remove-Item $shot
Write-Host '免安装工具检查通过：只依赖系统 DLL；查看器自检通过'

# 包（.iisz）是标准 ZIP：用 Python 自带的 zipfile 独立核对结构与 CRC，原样存放的文件内容与原文件一致
& .venv\Scripts\python -I -X utf8 tools\pkg_zip_check.py "$build\src\cli\Release\iis-cli.exe"
if ($LASTEXITCODE) { throw '.iisz 包的 ZIP 兼容性检查失败' }

# 安装包：生成 MSI 并做 ICE 校验（真实安装 / 升级 / 卸载需要管理员：scripts\test-installer.ps1）
if (Get-Command dotnet -ErrorAction SilentlyContinue) {
    & "$PSScriptRoot\build-installer.ps1"
} else {
    Write-Warning '没有 dotnet，跳过安装包生成（WiX 是 dotnet 工具）'
}

if ($Parity) {
    & .venv\Scripts\python tools\native_parity.py bench\out\synth --raw-config bench\out\synth\raw_config.yaml --repeat 1
    if ($LASTEXITCODE) { throw '与 Python 参考实现对照不一致' }
}
Write-Host 'Windows CI 通过'
