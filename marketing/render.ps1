# 把 marketing\posters\*.html 渲染成 PNG（2 倍像素）到 marketing\out\，用本机 Edge 的无界面模式，不联网。
#
#   powershell -ExecutionPolicy Bypass -File marketing\render.ps1 [海报名（不含 .html），省略为全部]
#
# 海报尺寸写在每个 html 的 body 宽高里，这里按文件名查表。截图素材由 marketing\shots.ps1 生成。
param([string]$Only = '')
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$edge = "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe"
if (-not (Test-Path $edge)) { $edge = "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe" }
if (-not (Test-Path $edge)) { throw '找不到 Microsoft Edge' }
$size = @{ '1-主海报' = '1080,1440'; '2-横幅' = '1920,1080'; '3-功能' = '1080,1440'; '4-数据安全' = '1080,1440'
          '5-Hero-EN' = '1080,1440'; '6-Banner-EN' = '1920,1080'; '7-Features-EN' = '1080,1440'; '8-Safety-EN' = '1080,1440' }
New-Item -ItemType Directory -Force "$here\out" | Out-Null
$profile = Join-Path (Split-Path $here -Parent) '.tmp\edge-render'   # 临时浏览器配置放项目目录，不落 C 盘
foreach ($f in Get-ChildItem "$here\posters\*.html") {
    $n = $f.BaseName
    if ($Only -and $n -ne $Only) { continue }
    if (-not $size[$n]) { Write-Host "跳过 $n（没有登记尺寸）"; continue }
    $out = "$here\out\$n.png"
    $url = ([Uri]$f.FullName).AbsoluteUri
    $p = Start-Process $edge -ArgumentList '--headless=new', '--disable-gpu', '--hide-scrollbars', "--user-data-dir=`"$profile`"",
        '--force-device-scale-factor=2', "--window-size=$($size[$n])", '--virtual-time-budget=3000', "--screenshot=`"$out`"", $url -Wait -PassThru
    if (-not (Test-Path $out)) { throw "渲染失败：$n" }
    Write-Host "$out"
}
