# 安装包

三溪缩影（Threebrook Pack）的 Windows MSI 安装包，用 WiX 5 生成，适用于 Windows 10 / 11 64 位。用法像 ZIP / WinRAR：资源管理器里右键压缩、右键解压。
没有后台服务，不需要授权，不写数据目录。现场部署参数见 [安装说明.txt](安装说明.txt)，它随安装包一起放在 `dist\`；
用户说明见 [使用说明.txt](使用说明.txt)。

## 两种装法（双用途安装包）

同一个 MSI（`Scope="perUserOrMachine"`，即 `ALLUSERS=2`、`MSIINSTALLPERUSER=1`）：

| 装法 | 怎么装 | 程序目录 | 注册表 | PATH | 开始菜单 |
|---|---|---|---|---|---|
| 只装给当前用户（默认） | 双击，或 `msiexec /i … /qn`；**不需要管理员**，不弹 UAC | `%LOCALAPPDATA%\Programs\三溪缩影` | HKCU | 用户 PATH | 当前用户 |
| 装给所有用户 | `msiexec /i … ALLUSERS=1`（管理员） | `Program Files\三溪缩影` | HKLM | 系统 PATH | 所有用户 |

- 注册表一律写 `HKMU`（随装法落到 HKLM 或 HKCU）；ProgId、COM 类写的 HKCR 也随装法。PATH 分两个带条件的组件。
- 双击不弹 UAC 靠摘要信息“字数”的第 3 位（不需要提升权限）。WiX 5 的 `perUserOrMachine` 不设这一位，
  由 `build-installer.ps1` 在 `wix build` 之后用 Windows Installer 自己的接口补上（签名之前）。
- 机器上已经为所有用户装过（`HKLM\SOFTWARE\IIS\Install` 有记录，含 0.2 及更早的版本，那时只能装给所有用户）时，
  双击安装一开始就停下（启动条件），提示先在“设置 → 应用”里卸载旧版本（会请求管理员确认），或用管理员命令行加 `ALLUSERS=1` 升级。
  不能悄悄把 `ALLUSERS` 改成 1：双击的安装带“不需要提升权限”标志，不会中途请求管理员，会在执行时报 1925
  “没有足够的特权为该计算机所有用户完成此安装”（0.3.0 第一版就是这样，负责人 2026-11-03 遇到）。
  MSI 文件右键也没有“以管理员身份运行”。

## 生成

```powershell
powershell -ExecutionPolicy Bypass -File scripts\ci-windows.ps1
```

`ci-windows.ps1` 先构建、测试，再调用 `build-installer.ps1` 生成 MSI。也可以只打包：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-installer.ps1 -Version 0.3.0 -Manufacturer 公司名
```

输出 `dist\ThreebrookPack-<版本>-win-x64.msi` 与 `dist\安装说明.txt`。
WiX 5.0.2 作为 dotnet 工具装在 `.cache\tools\wix`，NuGet 缓存在 `.cache\nuget`，都在项目目录内。
WiX 5 是 MS-RL 许可；WiX 6 起二进制附带“开源维护费”条款，所以锁定在 5.0.2。

`build-installer.ps1` 生成后会运行 Windows Installer 的 ICE 一致性校验，剩 3 条警告，都是有意的：
- ICE61：允许同版本升级，见下文“升级”。
- ICE57（`ViewerExe`、`ThumbDll` 两个组件）：文件在 `ProgramFiles64Folder`（ICE 按“所有用户”看待），注册表写 `HKMU`。
  双用途安装包里两者随装法一起变，ICE57 不认识这种写法。

## 代码签名（预留，证书由负责人提供）

| 环境变量 | 作用 |
|---|---|
| `IIS_SIGN_PFX` + `IIS_SIGN_PASSWORD` | 用 PFX 文件签名 |
| `IIS_SIGN_THUMBPRINT` | 用已导入证书存储的证书签名（含 USB 令牌、EV 证书） |
| `IIS_SIGN_TIMESTAMP` | 时间戳服务器，默认 `http://timestamp.digicert.com`；`none` 表示不加 |
| `IIS_SIGNTOOL` | `signtool.exe` 路径，默认取 Windows SDK 中最新的 |
| `IIS_SIGN_TEST=1` | 用自签证书演练：不要求证书受信任，只检查已签名且内容一致 |

签名顺序：
1. 先签 `iis-viewer`、`iis_thumb.dll`、`iis-cli`、`iis-decode`，所以装到客户机上的每个程序都带签名。
2. 再签 MSI 本身。
3. 最后用 `signtool verify /pa` 逐个校验。

没有设置证书时跳过签名，并在输出中标明“未签名”。

所有程序都带版本信息（文件属性 → 详细信息）。版本号取 `project(VERSION)`，第 4 段是 CMake 变量 `IIS_BUILD_NUMBER`；
产品名“三溪缩影 Threebrook Pack”，公司名是 `IIS_COMPANY`（默认“三溪 Threebrook”）。

## 安装内容

| 位置 | 内容 |
|---|---|
| 程序目录（`INSTALLFOLDER`，见上表） | `iis-viewer.exe`（查看器与压缩 / 解压窗口）、`iis_thumb.dll`（缩略图）、`iis-cli.exe`（目录加入 PATH）、`iis-decode.exe`（开源解码器）、使用说明、`许可证\`（含 3 个界面字体的 OFL） |
| 右键文件夹 | “三溪缩影 压缩（打成 .iisz 包）”、“三溪缩影 解压其中的 .iis / .iisz” |
| 右键 BMP / DIB / TIFF / RAW | “三溪缩影 压缩”（生成 `文件名.iis`）；只加菜单项，不改这些格式原来的默认打开程序 |
| `.iisz` / `.iis` | 双击用三溪缩影打开；右键“三溪缩影 解压到此处”；各有图标（查看器里的资源 2、3） |
| `.iis` / `.iisz` 缩略图 | `iis_thumb.dll`（缩略图处理程序，CLSID `{6C71C38C-08F5-40DC-AD84-B8644B008F7A}`）：资源管理器“大图标”等视图里，`.iis` 显示图片本身，`.iisz` 显示前 4 张图的拼图。注册在 `.iis\ShellEx\{e357fccd-…}` 与 `.iisz\ShellEx\{e357fccd-…}` 下，并登记到 `Shell Extensions\Approved`。Windows 默认在隔离进程里运行缩略图处理程序，出错不影响资源管理器。包只读目录和用到的几个条目，不读整个包；包的缩略图右下角叠加包图标（ProgId 的 `TypeOverlay`），包里没有能显示的图像时显示包图标 |
| 开始菜单 | 三溪缩影、三溪缩影 压缩文件夹、三溪缩影 解压、使用说明 |
| `SOFTWARE\IIS\Install`（HKLM 或 HKCU） | 程序目录与版本（升级沿用程序目录） |

右键菜单、文件类型名（“类型”一栏）、开始菜单的文字随 Windows 显示语言为中文或英文：注册表里写 `MUIVerb`、`FriendlyTypeName`（`@iis-viewer.exe,-201` 等），
快捷方式用 `DisplayResourceDll/Id`，字符串在 `src/viewer/icons.rc.in`。

内部标识沿用旧名，改了会让已装的版本无法升级或已有文件失去关联：程序文件名（`iis-viewer.exe` 等）、扩展名 `.iis` / `.iisz`、
ProgId `IIS.CompressedImage` / `IIS.Package`、UpgradeCode、注册表 `SOFTWARE\IIS\Install`。

右键菜单是经典菜单项，Windows 11 在“显示更多选项”里。要出现在第一层需要实现 IExplorerCommand 并打包为稀疏包，暂不做。

资源管理器选中多项时会为每一项各启动一次 `iis-viewer`；第一个进程收集其余进程的路径，放进同一个窗口处理。
（默认最多 15 项，这是资源管理器对经典菜单项的限制。）

卸载删除上表中的全部内容，不碰任何图像、`.iis` 与 `.iisz` 文件。

## 升级

- 采用主要升级（major upgrade）：先完整卸载旧版本，再装新版本。从 0.1 版（带后台服务）升级时，旧版本的服务一并卸载；
  它留下的 `ProgramData\IIS`（配置、索引、日志、还原工具）按当时的设计保留，不由新版本删除。
- 装法与 `INSTALLFOLDER` 沿用上次的，记在 `SOFTWARE\IIS\Install` 中（所有用户在 HKLM，当前用户在 HKCU）。
- 不能降级。
- 允许同版本升级，便于现场替换同版本的修正构建。

## 测试

| 测试 | 内容 | 权限 |
|---|---|---|
| `build-installer.ps1` | 生成 MSI 并做 ICE 校验 | 普通用户 |
| `scripts/test-installer.ps1` | 两种装法各自的真实安装、升级、卸载与降级拦截：文件、PATH、右键菜单与关联、缩略图处理程序（注册与实际生成）、开始菜单都在该装法的位置、另一种位置没有；安装包标明不需要提升权限；为所有用户装过后不带参数再装（等于双击）一开始就被拦下并提示，加 ALLUSERS=1 才升级、只有一份；安装后的打包解包逐字节一致；卸载后图像与包不变。程序目录放在 `.tmp\` 下 | 管理员 |
