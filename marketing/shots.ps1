﻿# 生成宣传截图：合成演示图像（tools\make_demo_images.py，不含客户样本）→ 打包 / 压缩 → 查看器无界面截图（2 倍像素）
#   中文界面 → marketing\assets\shots\*.png；英文界面（英文文件名）→ marketing\assets\shots-en\*.png
# 之后运行 marketing\render.ps1 出海报。
#
#   powershell -ExecutionPolicy Bypass -File marketing\shots.ps1
#
# 需要已构建（scripts\ci-windows.ps1）与 .venv（numpy、tifffile）。演示数据放 .tmp\marketing-demo（-en）。
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
$cli = "$root\build\win-x64\src\cli\Release\iis-cli.exe"
$v = "$root\build\win-x64\src\viewer\Release\iis-viewer.exe"
Add-Type -AssemblyName System.Drawing

function Shots([bool]$en) {
    $demo = if ($en) { "$root\.tmp\marketing-demo-en" } else { "$root\.tmp\marketing-demo" }
    if (Test-Path $demo) { Remove-Item -LiteralPath $demo -Recurse -Force }
    $genArgs = @('-I', 'tools\make_demo_images.py', $demo) + $(if ($en) { @('--en') } else { @() })
    & "$root\.venv\Scripts\python.exe" @genArgs
    if ($LASTEXITCODE) { throw '生成演示图像失败' }
    # 文件名：文件夹、第一张（PCB）、第二张（16 位晶圆 TIFF）
    $n = if ($en) { @('Line images 2026-11-03', 'A01 PCB.bmp', 'A02 Wafer dies.tif') } else { @('产线图像 2026-11-03', 'A01 PCB 板.bmp', 'A02 晶圆 die 阵列.tif') }
    $f = "$demo\$($n[0])"
    & $cli pack $f | Out-Null
    Copy-Item -LiteralPath "$f\$($n[1])" $demo
    & $cli compress "$demo\$($n[1])" | Out-Null
    $pk = "$demo\$($n[0]).iisz"
    $lang = if ($en) { '--lang en' } else { '--lang zh' }
    # 包内条目序号：0 顶层文件夹，1 子文件夹，2~5 为 A01~A04，6~7 为子文件夹里的两张
    $shots = [ordered]@{
        '看图-浅色'     = "--size 1440x900 --light `"$demo\$($n[1]).iis`""
        '看图-深色'     = "--size 1440x900 --dark `"$f\$($n[2])`""
        '包内浏览'      = "--size 1440x900 --light `"$pk|4`""
        '包内浏览-深色' = "--size 1440x900 --dark `"$pk|6`""
        '空白-浅色'     = "--size 1440x900 --light"
        '压缩窗口'      = "--size 640x460 --light --compress `"$f`" `"$demo\$($n[1])`""
        '解压完成'      = "--size 640x460 --light --extract `"$pk`""
    }
    $names = @{ '看图-浅色' = 'view-light'; '看图-深色' = 'view-dark'; '包内浏览' = 'package'; '包内浏览-深色' = 'package-dark';
                '空白-浅色' = 'empty-light'; '压缩窗口' = 'compress'; '解压完成' = 'extract-done' }
    $tmp = "$root\.tmp\marketing-shots"
    $dest = if ($en) { "$PSScriptRoot\assets\shots-en" } else { "$PSScriptRoot\assets\shots" }
    New-Item -ItemType Directory -Force $tmp, $dest | Out-Null
    foreach ($k in $shots.Keys) {
        $name = if ($en) { $names[$k] } else { $k }
        $bmp = "$tmp\$name.bmp"
        Start-Process $v -ArgumentList "--render `"$bmp`" --scale 2 $lang $($shots[$k])" -Wait
        $img = [System.Drawing.Image]::FromFile($bmp)
        $img.Save("$dest\$name.png", [System.Drawing.Imaging.ImageFormat]::Png)
        $img.Dispose()
        Write-Host "$dest\$name.png"
    }
}
Shots $false
Shots $true
