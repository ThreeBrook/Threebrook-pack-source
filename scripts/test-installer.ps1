# 三溪缩影安装包验收测试：两种装法（只装给当前用户 / 装给所有用户）各自的安装、升级、卸载，降级拦截（压缩文件不受影响）。
#
#   以管理员身份打开 PowerShell，在项目目录运行：
#   powershell -ExecutionPolicy Bypass -File scripts\test-installer.ps1
#
# A 组：只装给当前用户（双击安装的默认方式，不需要管理员）：注册表在 HKCU、PATH 为用户 PATH、开始菜单在当前用户下，HKLM 不留任何东西。
# B 组：装给所有用户（ALLUSERS=1，需要管理员）；之后不带参数再装新版本（等于在这台机器上双击）应一开始就拦下并说明，加 ALLUSERS=1 才升级。
# 程序目录放在 .tmp\installer-test\ 下（不写 Program Files），结束后删除测试留下的 HKLM / HKCU\SOFTWARE\IIS。
# 机器上已装有本软件时拒绝运行（不碰正式安装）。
# 需要已构建（scripts\ci-windows.ps1）；两个版本的安装包由本脚本调用 build-installer.ps1 生成（同一套程序，版本号不同）。
# 结果写入 .tmp\installer-test\run-时间\结果.txt。右键菜单与缩略图的实际效果请按结尾提示手动检查。
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot -Parent)
$root = (Get-Location).Path
$env:TEMP = "$root\.tmp"; $env:TMP = "$root\.tmp"

$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) { throw '需要以管理员身份运行（B 组装给所有用户）' }

# 本测试留下的安装（上次中途出错）：程序目录在 .tmp\installer-test 下的才卸载，别的一律不碰
$upgradeCode = '{27934C50-BC0F-407D-8ADC-74434C576DCC}'
function Related { $wi = New-Object -ComObject WindowsInstaller.Installer; @($wi.GetType().InvokeMember('RelatedProducts', 'GetProperty', $null, $wi, @($upgradeCode))) }
function Remove-TestInstall {
    foreach ($hive in 'HKLM:', 'HKCU:') {
        $inst = (Get-ItemProperty "$hive\SOFTWARE\IIS\Install" -ErrorAction SilentlyContinue).InstallFolder
        if ($inst -and -not $inst.StartsWith("$root\.tmp\installer-test\", [StringComparison]::OrdinalIgnoreCase)) { return $false }
    }
    foreach ($pc in Related) {
        Write-Host "卸载上次测试留下的安装 $pc"
        Start-Process msiexec.exe -ArgumentList "/x $pc /qn" -Wait | Out-Null
    }
    Remove-Item 'HKLM:\SOFTWARE\IIS' -Recurse -ErrorAction SilentlyContinue
    Remove-Item 'HKCU:\Software\IIS' -Recurse -ErrorAction SilentlyContinue
    return $true
}
$installed = (Related).Count -gt 0 -or (Test-Path 'HKLM:\SOFTWARE\IIS\Install') -or (Test-Path 'HKCU:\SOFTWARE\IIS\Install')
if ($installed -and -not (Remove-TestInstall)) {
    throw '本机已装有三溪缩影（或旧版 IIS，设置 → 应用里能看到），请先卸载再运行测试；测试不碰正式安装'
}

# ---- 两个版本的安装包 ----
$v1 = (Select-String -Path CMakeLists.txt -Pattern 'project\(iis VERSION ([0-9.]+)').Matches[0].Groups[1].Value
$p = $v1.Split('.'); $v2 = "$($p[0]).$($p[1]).$([int]$p[2] + 1)"
$msi1 = "$root\dist\ThreebrookPack-$v1-win-x64.msi"
$msi2 = "$root\dist\ThreebrookPack-$v2-win-x64-升级测试.msi"
& "$PSScriptRoot\build-installer.ps1" -Version $v1
& "$PSScriptRoot\build-installer.ps1" -Version $v2 -OutName "ThreebrookPack-$v2-win-x64-升级测试"

$T = "$root\.tmp\installer-test\run-$(Get-Date -Format yyyyMMdd-HHmmss)"
New-Item -ItemType Directory -Force $T | Out-Null
$img = "$T\图像 文件夹"
$script:fails = 0
$report = New-Object System.Collections.Generic.List[string]
function Check([bool]$ok, [string]$what) {
    $line = $(if ($ok) { '通过  ' } else { '失败  ' }) + $what
    if (-not $ok) { $script:fails++ }
    $report.Add($line); Write-Host $line -ForegroundColor $(if ($ok) { 'Green' } else { 'Red' })
}
function Step([string]$t) { $report.Add(''); $report.Add("== $t"); Write-Host "`n== $t" -ForegroundColor Cyan }
# 1618 = 另一个安装正在进行（别的软件的安装或更新占着 Windows Installer）：等它结束再试，最多约 5 分钟
function Msi([string]$verb, [string]$msi, [string]$log, [string]$props = '') {
    $a = "$verb `"$msi`" /qn /l*v `"$log`" $props"
    for ($i = 0; ; $i++) {
        $rc = (Start-Process msiexec.exe -ArgumentList $a -Wait -PassThru).ExitCode
        if ($rc -ne 1618 -or $i -ge 60) { return $rc }
        if ($i -eq 0) { Write-Host '另一个安装正在进行（1618），等它结束后重试…' -ForegroundColor Yellow }
        Start-Sleep 5
    }
}
function Hashes([string]$dir) {
    $h = @{}
    Get-ChildItem $dir -Recurse -File | ForEach-Object { $h[$_.FullName.Substring($dir.Length)] = (Get-FileHash $_.FullName -Algorithm SHA256).Hash }
    $h
}
function Same($a, $b) {
    if ($a.Count -ne $b.Count -or $a.Count -eq 0) { return $false }
    foreach ($k in $a.Keys) { if ($b[$k] -ne $a[$k]) { return $false } }
    return $true
}
# 某种装法下的位置：注册表根、PATH 的范围、开始菜单
function Scope([bool]$machine) {
    if ($machine) { @{ Hive = 'HKLM:'; Other = 'HKCU:'; Env = 'Machine'; Menu = "$env:ProgramData\Microsoft\Windows\Start Menu\Programs\三溪缩影"; Name = '所有用户' } }
    else { @{ Hive = 'HKCU:'; Other = 'HKLM:'; Env = 'User'; Menu = "$env:APPDATA\Microsoft\Windows\Start Menu\Programs\三溪缩影"; Name = '当前用户' } }
}
function Reg([string]$hive, [string]$key) { (Get-ItemProperty "$hive\SOFTWARE\Classes\$key" -ErrorAction SilentlyContinue).'(default)' }
$thumbClsid = '{6C71C38C-08F5-40DC-AD84-B8644B008F7A}'
$thumbKey = '{e357fccd-a995-4576-b01f-234630154e96}'

# ---- 测试数据：24 位 BMP（渐变 + 图形，可压缩）与一个子目录 ----
Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Force "$img\工位1" | Out-Null
for ($i = 0; $i -lt 6; $i++) {
    $bmp = New-Object System.Drawing.Bitmap (640 + 16 * $i), 480
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $br = New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.Rectangle 0, 0, $bmp.Width, 480), ([System.Drawing.Color]::FromArgb(20, 20 * $i, 40)), ([System.Drawing.Color]::FromArgb(230, 200, 255 - 20 * $i)), 30.0
    $g.FillRectangle($br, 0, 0, $bmp.Width, 480)
    $g.FillEllipse([System.Drawing.Brushes]::DarkGray, 100 + 20 * $i, 120, 200, 160)
    $g.Dispose()
    $bmp.Save("$img\$(if ($i % 2) { "工位1\$i.bmp" } else { "img_$i.bmp" })", [System.Drawing.Imaging.ImageFormat]::Bmp)
    $bmp.Dispose()
}
$orig = Hashes $img

function Write-Report {
    $os = Get-CimInstance Win32_OperatingSystem
    $report.Insert(0, "三溪缩影安装包测试  $(Get-Date -Format 'yyyy-MM-dd HH:mm')  $($os.Caption) $($os.Version)  安装包 $v1 / $v2")
    $report.Add(''); $report.Add($(if ($script:fails) { "失败 $($script:fails) 项" } else { '全部通过' }))
    $report | Out-File -Encoding utf8 "$T\结果.txt"
    Write-Host "`n结果：$T\结果.txt（各步安装日志在同一目录）"
}
# 测试脚本本身出错：记下错误，卸载本次测试的安装，写出结果
trap {
    Check $false "测试中断：$($_.Exception.Message)（$($_.InvocationInfo.PositionMessage -replace '\s+', ' ')）"
    Remove-TestInstall | Out-Null
    Write-Report
    exit 1
}

# 装好后的检查：文件、PATH、开始菜单、关联、右键菜单、缩略图都在 $s 这种装法的位置，另一种位置没有
function Check-Installed($s, [string]$prog, [string]$ver) {
    $hive = $s.Hive
    foreach ($f in 'iis-viewer.exe', 'iis_thumb.dll', 'iis-cli.exe', 'iis-decode.exe', '使用说明.txt') { Check (Test-Path "$prog\$f") "已安装 $f" }
    Check ((Get-ChildItem "$prog\许可证" -File).Count -ge 11) '第三方许可证（含 3 个字体的 OFL）'
    Check (([Environment]::GetEnvironmentVariable('PATH', $s.Env)) -like "*$prog\*") "$($s.Name) PATH 含程序目录"
    $r = Get-ItemProperty "$hive\SOFTWARE\IIS\Install" -ErrorAction SilentlyContinue
    Check ($r.InstallFolder -eq "$prog\" -and $r.Version -eq $ver) "记下程序目录与版本 $ver（$hive）"
    Check (-not (Get-ItemProperty "$($s.Other)\SOFTWARE\IIS\Install" -ErrorAction SilentlyContinue).InstallFolder) "另一种装法的位置（$($s.Other)）没有安装记录"
    foreach ($m in '三溪缩影', '三溪缩影 压缩文件夹', '三溪缩影 解压', '使用说明') { Check (Test-Path "$($s.Menu)\$m.lnk") "开始菜单（$($s.Name)）：$m" }
    $viewer = "$prog\iis-viewer.exe"
    Check ((Reg $hive '.iis') -eq 'IIS.CompressedImage' -and (Reg $hive '.iisz') -eq 'IIS.Package') ".iis / .iisz 关联到查看器（$hive）"
    Check ((Reg $hive 'IIS.CompressedImage\DefaultIcon') -like "*$viewer*-2*" -and (Reg $hive 'IIS.Package\DefaultIcon') -like "*$viewer*-3*") '.iis / .iisz 有自己的图标'
    Check ((Reg $hive 'IIS.Package\shell\IisExtract') -eq '三溪缩影 解压到此处' -and (Reg $hive 'IIS.Package\shell\IisExtract\command') -like "*$viewer*--extract*%1*") '右键 .iisz：三溪缩影 解压到此处'
    Check ((Reg $hive 'IIS.CompressedImage\shell\IisExtract\command') -like "*$viewer*--extract*%1*") '右键 .iis：三溪缩影 解压到此处'
    Check ((Reg $hive 'Directory\shell\IisCompress') -like '三溪缩影 压缩*' -and (Reg $hive 'Directory\shell\IisCompress\command') -like "*$viewer*--compress*%1*") '右键文件夹：三溪缩影 压缩'
    Check ((Reg $hive 'Directory\shell\IisExtract\command') -like "*$viewer*--extract*%1*") '右键文件夹：三溪缩影 解压其中的 .iis / .iisz'
    $mui = (Get-ItemProperty "$hive\SOFTWARE\Classes\Directory\shell\IisCompress").MUIVerb
    $ftn = (Get-ItemProperty "$hive\SOFTWARE\Classes\IIS.Package").FriendlyTypeName
    Check ($mui -like "@*iis-viewer.exe,-201" -and $ftn -like "@*iis-viewer.exe,-207") '右键菜单与文件类型的文字取自程序资源（随 Windows 显示语言为中文或英文）'
    $ok = $true; foreach ($e in 'bmp', 'dib', 'tif', 'tiff', 'raw') { if ((Reg $hive "SystemFileAssociations\.$e\shell\IisCompress\command") -notlike "*$viewer*--compress*") { $ok = $false } }
    Check $ok '右键 BMP / DIB / TIFF / RAW：三溪缩影 压缩'
    Check ((Reg $hive "CLSID\$thumbClsid\InprocServer32") -like "*$prog\iis_thumb.dll" -and (Reg $hive ".iis\ShellEx\$thumbKey") -eq $thumbClsid -and
           (Reg $hive ".iisz\ShellEx\$thumbKey") -eq $thumbClsid) ".iis / .iisz 缩略图处理程序已注册（$hive）"
    Check ((Get-ItemProperty "$hive\SOFTWARE\Classes\IIS.Package").TypeOverlay -like "*$viewer*,-3") '.iisz 缩略图右下角叠加包图标'
    Check (-not (Test-Path "$($s.Other)\SOFTWARE\Classes\Directory\shell\IisCompress") -and -not (Test-Path "$($s.Other)\SOFTWARE\Classes\CLSID\$thumbClsid")) "另一种装法的位置（$($s.Other)）没有右键菜单与缩略图注册"
}
function Check-Removed($s, [string]$prog) {
    $hive = $s.Hive
    Check (-not (Test-Path "$prog\iis-viewer.exe") -and -not (Test-Path "$prog\iis-cli.exe")) '程序已删除'
    Check (-not (([Environment]::GetEnvironmentVariable('PATH', $s.Env)) -like "*$prog\*")) "$($s.Name) PATH 已恢复"
    Check (-not (Test-Path $s.Menu)) '开始菜单已删除'
    Check (-not (Test-Path "$hive\SOFTWARE\Classes\Directory\shell\IisCompress") -and -not (Test-Path "$hive\SOFTWARE\Classes\Directory\shell\IisExtract") -and
           -not (Test-Path "$hive\SOFTWARE\Classes\SystemFileAssociations\.bmp\shell\IisCompress") -and
           -not (Test-Path "$hive\SOFTWARE\Classes\IIS.CompressedImage") -and -not (Test-Path "$hive\SOFTWARE\Classes\IIS.Package")) '右键菜单与文件关联已删除'
    Check (-not (Test-Path "$hive\SOFTWARE\Classes\CLSID\$thumbClsid") -and -not (Test-Path "$hive\SOFTWARE\Classes\.iis\ShellEx") -and
           -not (Test-Path "$hive\SOFTWARE\Classes\.iisz\ShellEx")) '缩略图处理程序已注销'
    Check (-not (Get-ItemProperty "$hive\SOFTWARE\IIS\Install" -ErrorAction SilentlyContinue).InstallFolder) '安装记录已清理'
    Check ((Related).Count -eq 0) '已没有任何版本'
}

# ================================================================
Step '0. 安装包是“双用途”的：双击默认只装给当前用户，不要求管理员'
$wi = New-Object -ComObject WindowsInstaller.Installer
$db = $wi.GetType().InvokeMember('OpenDatabase', 'InvokeMethod', $null, $wi, @($msi1, 0))
$si = $db.GetType().InvokeMember('SummaryInformation', 'GetProperty', $null, $db, @(0))
Check (([int]$si.GetType().InvokeMember('Property', 'GetProperty', $null, $si, @(15)) -band 8) -ne 0) '摘要信息标明不需要提升权限（双击不弹 UAC）'
$prop = { param($n) $v = $db.GetType().InvokeMember('OpenView', 'InvokeMethod', $null, $db, @("SELECT Value FROM Property WHERE Property='$n'"))
          $v.GetType().InvokeMember('Execute', 'InvokeMethod', $null, $v, $null); $r = $v.GetType().InvokeMember('Fetch', 'InvokeMethod', $null, $v, $null)
          if ($r) { $r.GetType().InvokeMember('StringData', 'GetProperty', $null, $r, 1) } }
Check ((& $prop 'ALLUSERS') -eq '2' -and (& $prop 'MSIINSTALLPERUSER') -eq '1') 'ALLUSERS=2、MSIINSTALLPERUSER=1（默认当前用户，可改为所有用户）'
$si = $null; $db = $null

# ================================================================ A 组
$su = Scope $false
$progU = "$T\当前用户 程序"
Step "A1. 只装给当前用户：静默安装 $v1（自定义程序目录，含中文与空格）"
$rc = Msi '/i' $msi1 "$T\A1-安装.log" "INSTALLFOLDER=`"$progU\`""
Check ($rc -eq 0) "msiexec 退出码 $rc"
if ($rc -ne 0) { throw "安装失败（退出码 $rc），后面的检查没有意义；原因见 $T\A1-安装.log" }
Check-Installed $su $progU $v1
& "$root\build\win-x64\src\viewer\Release\iis_thumb_check.exe" "$progU\iis_thumb.dll" | Out-Null
Check ($LASTEXITCODE -eq 0) '安装的缩略图处理程序能生成缩略图（.iis 与 .iisz 拼图）'
$shot = "$T\压缩窗口.bmp"
$pr = Start-Process "$progU\iis-viewer.exe" -ArgumentList "--render", "`"$shot`"", "--compress", "`"$img`"" -Wait -PassThru
Check ($pr.ExitCode -eq 0 -and (Test-Path $shot)) '安装后的查看器能打开压缩窗口'
# 用安装好的命令行打包、解包（与右键菜单是同一套代码）
& "$progU\iis-cli.exe" pack $img | Out-Null
Check ($LASTEXITCODE -eq 0 -and (Test-Path "$img.iisz")) '打包为 图像 文件夹.iisz'
Check (Same $orig (Hashes $img)) '默认保留原文件'
& "$progU\iis-cli.exe" unpack "$img.iisz" -o "$T\解出" | Out-Null
Check ($LASTEXITCODE -eq 0 -and (Same $orig (Hashes "$T\解出\图像 文件夹"))) '解出的文件与原文件逐字节一致'
$pkgHash = (Get-FileHash "$img.iisz").Hash

Step "A2. 静默升级到 $v2（只装给当前用户）"
$rc = Msi '/i' $msi2 "$T\A2-升级.log"
Check ($rc -eq 0) "msiexec 退出码 $rc"
Check ((Get-ItemProperty 'HKCU:\SOFTWARE\IIS\Install').Version -eq $v2 -and (Test-Path "$progU\iis-viewer.exe")) "已是 $v2，沿用记下的程序目录（没有传 INSTALLFOLDER）"
Check ((Related).Count -eq 1) '只有一份安装'
Check ((Get-FileHash "$img.iisz").Hash -eq $pkgHash -and (Same $orig (Hashes $img))) '图像与包不受影响'

Step "A3. 降级（装 $v1）应被拒绝"
$rc = Msi '/i' $msi1 "$T\A3-降级.log"
Check ($rc -ne 0) "msiexec 拒绝（退出码 $rc）"
Check ((Get-ItemProperty 'HKCU:\SOFTWARE\IIS\Install').Version -eq $v2) "仍是 $v2"

Step 'A4. 静默卸载：只删程序、右键菜单与开始菜单，不碰图像与压缩文件'
$rc = Msi '/x' $msi2 "$T\A4-卸载.log"
Check ($rc -eq 0) "msiexec 退出码 $rc"
Check-Removed $su $progU
Check ((Get-FileHash "$img.iisz").Hash -eq $pkgHash -and (Same $orig (Hashes $img))) '图像与包逐字节不变'

# ================================================================ B 组
$sm = Scope $true
$progM = "$T\所有用户 程序"
Step "B1. 装给所有用户（ALLUSERS=1）：静默安装 $v1"
$rc = Msi '/i' $msi1 "$T\B1-安装.log" "ALLUSERS=1 INSTALLFOLDER=`"$progM\`""
Check ($rc -eq 0) "msiexec 退出码 $rc"
if ($rc -ne 0) { throw "安装失败（退出码 $rc），原因见 $T\B1-安装.log" }
Check-Installed $sm $progM $v1

Step "B2. 不带参数安装 $v2（等于在这台机器上双击）：应一开始就拦下并说明怎么办，不装出第二份"
$rc = Msi '/i' $msi2 "$T\B2-双击.log"
Check ($rc -eq 1603) "msiexec 拒绝（退出码 $rc）"
$b = [IO.File]::ReadAllBytes("$T\B2-双击.log")  # 安装日志可能是 UTF-16 或系统代码页
$logText = [Text.Encoding]::Unicode.GetString($b) + [Text.Encoding]::Default.GetString($b)
Check ($logText -like '*已经为所有用户装了旧版本*') '提示先卸载旧版本或用 ALLUSERS=1'
Check ((Related).Count -eq 1 -and (Get-ItemProperty 'HKLM:\SOFTWARE\IIS\Install').Version -eq $v1) "仍是 $v1，只有一份安装"
Check (-not (Get-ItemProperty 'HKCU:\SOFTWARE\IIS\Install' -ErrorAction SilentlyContinue).InstallFolder) '当前用户下没有安装记录'

Step "B3. 管理员命令行加 ALLUSERS=1 升级到 $v2"
$rc = Msi '/i' $msi2 "$T\B3-升级.log" 'ALLUSERS=1'
Check ($rc -eq 0) "msiexec 退出码 $rc"
Check ((Related).Count -eq 1) '只有一份安装'
Check ((Get-ItemProperty 'HKLM:\SOFTWARE\IIS\Install').Version -eq $v2 -and (Test-Path "$progM\iis-viewer.exe")) "已是 $v2，沿用程序目录（没有传 INSTALLFOLDER）"

Step 'B4. 静默卸载'
$rc = Msi '/x' $msi2 "$T\B4-卸载.log"
Check ($rc -eq 0) "msiexec 退出码 $rc"
Check-Removed $sm $progM
Check ((Get-FileHash "$img.iisz").Hash -eq $pkgHash -and (Same $orig (Hashes $img))) '图像与包逐字节不变'

Remove-Item 'HKLM:\SOFTWARE\IIS' -Recurse -ErrorAction SilentlyContinue
Remove-Item 'HKCU:\Software\IIS' -Recurse -ErrorAction SilentlyContinue

Write-Report
Write-Host @"

请再用普通（非管理员）账户手动检查（约 3 分钟）：
  1. 双击 $msi1：不弹管理员确认（UAC），直接装好。
  2. 在资源管理器里右键一个图像文件夹 →“三溪缩影 压缩（打成 .iisz 包）”→“开始压缩”：旁边出现 文件夹名.iisz（柿橙色图标）。
     Windows 11 先点“显示更多选项”。可以同时选中几个文件夹或 BMP，一起压缩。
  3. 把视图切到“大图标”：.iis 显示图片本身，.iisz 显示前 4 张图的拼图（右下角带包图标）。双击 .iisz：查看器里像文件夹一样浏览、看图。
  4. 查看器：标题栏、浅色 / 深色（视图 → 外观，默认跟随系统）、拖动标题栏、双击最大化、贴边分屏。
  5. 右键 .iisz →“三溪缩影 解压到此处”：解出 文件夹名 (2)，与原文件夹内容相同。
  6. 设置 → 应用 → 三溪缩影 → 卸载：右键菜单消失，.iisz 与图像都还在。
"@
if ($script:fails) { exit 1 }
