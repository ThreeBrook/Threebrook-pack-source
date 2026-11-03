# 生成三溪缩影安装包 dist\ThreebrookPack-<版本>-win-x64.msi（WiX 5），旁边放一份 安装说明.txt。
#
#   powershell -ExecutionPolicy Bypass -File scripts\build-installer.ps1 [-Version 0.3.0] [-Manufacturer 公司名]
#
# 先运行 scripts\ci-windows.ps1 构建并测试；本脚本只收集文件、签名、打包、校验。
# WiX 5.0.2 装在 .cache\tools\wix（dotnet 工具，没有时自动安装到项目目录，不装到系统）。
#
# 代码签名（证书由负责人提供；没提供时跳过，打包结果标明“未签名”）：
#   $env:IIS_SIGN_PFX = 'D:\证书\代码签名.pfx'; $env:IIS_SIGN_PASSWORD = '…'      PFX 文件
#   或 $env:IIS_SIGN_THUMBPRINT = '证书 SHA-1 指纹'                                已导入证书存储的证书（含 USB 令牌 / EV 证书）
#   $env:IIS_SIGN_TIMESTAMP   时间戳服务器，默认 http://timestamp.digicert.com（证书过期后签名仍有效）；none 表示不加时间戳（离线测试用）
#   $env:IIS_SIGN_TEST = '1'  用自签测试证书演练签名流程：不要求证书受信任，只检查已签名且内容一致
#   $env:IIS_SIGNTOOL         signtool.exe 路径，默认取 Windows SDK 中最新的
# 签名顺序：先签每个程序（打包进 msi 的是已签名的文件），再签 msi 本身，最后逐个校验。
param(
    [string]$Version = '',
    [string]$Manufacturer = '三溪 Threebrook',
    [string]$OutName = ''
)
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot -Parent)
$root = (Get-Location).Path

# 临时文件放项目目录，不落 C 盘
New-Item -ItemType Directory -Force .tmp | Out-Null
$env:TEMP = "$root\.tmp"; $env:TMP = "$root\.tmp"

if (-not $Version) { $Version = (Select-String -Path CMakeLists.txt -Pattern 'project\(iis VERSION ([0-9.]+)').Matches[0].Groups[1].Value }
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "版本号应为三段数字（MSI 只比较前三段）：$Version" }
if (-not $OutName) { $OutName = "ThreebrookPack-$Version-win-x64" }

$build = 'build\win-x64'
$files = [ordered]@{
    'iis-cli.exe'       = "$build\src\cli\Release\iis-cli.exe"
    'iis-viewer.exe'    = "$build\src\viewer\Release\iis-viewer.exe"
    'iis-decode.exe'    = "$build\decoder\Release\iis-decode.exe"
    'iis_thumb.dll'     = "$build\src\viewer\Release\iis_thumb.dll"
}
foreach ($f in $files.Values) { if (-not (Test-Path $f)) { throw "缺少 $f，请先运行 scripts\ci-windows.ps1" } }

# ---- WiX ----
$wix = "$root\.cache\tools\wix\wix.exe"
if (-not (Test-Path $wix)) {
    Write-Host '安装 WiX 5.0.2 到 .cache\tools\wix …'
    $env:NUGET_PACKAGES = "$root\.cache\nuget"; $env:DOTNET_CLI_HOME = "$root\.cache\dotnet-home"
    $env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'; $env:DOTNET_NOLOGO = '1'
    dotnet tool install wix --version 5.0.2 --tool-path "$root\.cache\tools\wix"
    if ($LASTEXITCODE) { throw 'WiX 安装失败' }
}

# ---- 签名（预留：证书由负责人提供） ----
$signArgs = $null
if ($env:IIS_SIGN_PFX) {
    $signArgs = @('/f', $env:IIS_SIGN_PFX)
    if ($env:IIS_SIGN_PASSWORD) { $signArgs += @('/p', $env:IIS_SIGN_PASSWORD) }
} elseif ($env:IIS_SIGN_THUMBPRINT) {
    $signArgs = @('/sha1', $env:IIS_SIGN_THUMBPRINT)
}
$signtool = $env:IIS_SIGNTOOL
if ($signArgs -and -not $signtool) {
    $signtool = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin\*\x64\signtool.exe' -ErrorAction SilentlyContinue |
        Sort-Object FullName | Select-Object -Last 1 -ExpandProperty FullName
    if (-not $signtool) { throw '找不到 signtool.exe（Windows SDK），可用 $env:IIS_SIGNTOOL 指定' }
}
$timestamp = if ($env:IIS_SIGN_TIMESTAMP) { $env:IIS_SIGN_TIMESTAMP } else { 'http://timestamp.digicert.com' }

function Sign-Files([string[]]$paths, [string]$desc) {
    if (-not $signArgs) { return }
    $tsArgs = if ($timestamp -eq 'none') { @() } else { @('/tr', $timestamp, '/td', 'sha256') }
    & $signtool sign /fd sha256 @tsArgs /d $desc @signArgs @paths
    if ($LASTEXITCODE) { throw "签名失败：$($paths -join ', ')" }
    if ($env:IIS_SIGN_TEST -eq '1') {
        # 自签测试证书不受信任：只检查确实已签名且内容与签名一致
        foreach ($p in $paths) {
            $sig = Get-AuthenticodeSignature $p
            if (-not $sig.SignerCertificate -or $sig.Status -in 'NotSigned', 'HashMismatch') { throw "签名校验失败：$p（$($sig.Status)）" }
        }
    } else {
        & $signtool verify /pa /q @paths
        if ($LASTEXITCODE) { throw "签名校验失败：$($paths -join ', ')" }
    }
}

# ---- 收集文件 ----
$stage = "$build\installer\stage"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null
foreach ($k in $files.Keys) { Copy-Item $files[$k] "$stage\$k" }
# 文本带 BOM：Windows 记事本（含旧版）都能正确显示中文
foreach ($t in @('使用说明.txt')) {
    [IO.File]::WriteAllText("$root\$stage\$t", [IO.File]::ReadAllText("$root\installer\$t", [Text.Encoding]::UTF8).Replace("`r`n", "`n").Replace("`n", "`r`n"), (New-Object Text.UTF8Encoding $true))
}
Copy-Item assets\icons\iis-app.ico "$stage\iis-app.ico"   # 控制面板“程序和功能”里的图标
. "$PSScriptRoot\licenses.ps1"
Copy-ThirdPartyLicenses -Build $build -Dest "$stage\许可证"

Sign-Files ($files.Keys | ForEach-Object { "$root\$stage\$_" }) '三溪缩影'

# ---- 打包 ----
New-Item -ItemType Directory -Force dist | Out-Null
$msi = "dist\$OutName.msi"
if (Test-Path $msi) { Remove-Item $msi }
& $wix build installer\iis.wxs -arch x64 -d "Version=$Version" -d "Manufacturer=$Manufacturer" -d "Stage=$root\$stage" `
    -intermediatefolder "$build\installer\obj" -pdbtype none -o $msi
if ($LASTEXITCODE) { throw 'wix build 失败' }
# 双用途安装包：摘要信息“字数”加第 3 位（不需要提升权限），双击时只装给当前用户、不弹 UAC。
# WiX 5 的 Scope="perUserOrMachine" 只设 ALLUSERS=2、MSIINSTALLPERUSER=1，不设这一位
$wi = New-Object -ComObject WindowsInstaller.Installer
$db = $wi.GetType().InvokeMember('OpenDatabase', 'InvokeMethod', $null, $wi, @("$root\$msi", 1))
$si = $db.GetType().InvokeMember('SummaryInformation', 'GetProperty', $null, $db, @(1))
$wc = [int]$si.GetType().InvokeMember('Property', 'GetProperty', $null, $si, @(15))
$si.GetType().InvokeMember('Property', 'SetProperty', $null, $si, @(15, ($wc -bor 8))) | Out-Null
$si.GetType().InvokeMember('Persist', 'InvokeMethod', $null, $si, $null) | Out-Null
$db.GetType().InvokeMember('Commit', 'InvokeMethod', $null, $db, $null) | Out-Null
[Runtime.InteropServices.Marshal]::ReleaseComObject($si) | Out-Null
[Runtime.InteropServices.Marshal]::ReleaseComObject($db) | Out-Null
Sign-Files @("$root\$msi") "三溪缩影 $Version 安装包"

# ICE 校验（Windows Installer 自带的一致性检查）
& $wix msi validate $msi
if ($LASTEXITCODE) { throw 'MSI 校验（ICE）未通过' }

[IO.File]::WriteAllText("$root\dist\安装说明.txt", [IO.File]::ReadAllText("$root\installer\安装说明.txt", [Text.Encoding]::UTF8).Replace("`r`n", "`n").Replace("`n", "`r`n"), (New-Object Text.UTF8Encoding $true))

$hash = (Get-FileHash $msi -Algorithm SHA256).Hash.ToLower()
$state = if ($signArgs) { '已签名' } else { '未签名（没有提供证书：设置 IIS_SIGN_PFX 或 IIS_SIGN_THUMBPRINT 后重新运行）' }
Write-Host ("安装包：{0}（{1:N1} MB，{2}）" -f (Resolve-Path $msi), ((Get-Item $msi).Length / 1MB), $state)
Write-Host "SHA-256：$hash"
