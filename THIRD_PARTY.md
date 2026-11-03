# 第三方组件清单

产品原生组件（`src/`，CMake 工程）使用的第三方代码。版本与源码包 SHA-256 锁定在 `cmake/deps.cmake`，
两处须同步修改。内核与闭源组件**不得引入 GPL / AGPL 依赖**。

## 链入产品的库

| 名称 | 版本 | 许可证 | 用途 | 链接方式 | 来源与校验 |
|---|---|---|---|---|---|
| CharLS | 2.4.5 | BSD-3-Clause | JPEG-LS 编解码（默认像素编码器，D4） | 静态链入 `iis.dll` / `libiis.so` | `github.com/team-charls/charls` 标签 2.4.5 源码包，SHA-256 `b55c742ef96b5e9d63f813a75d0ba29944302abb5a6ab59756ad61fa017b1dd3` |
| zstd | 1.5.7 | BSD-3-Clause（上游为 BSD / GPLv2 双许可，本项目按 BSD 使用） | 非像素字节与兜底整文件压缩 | 静态链入 | `github.com/facebook/zstd` 发布包 `zstd-1.5.7.tar.gz`，SHA-256 `eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3` |
| nlohmann/json | 3.12.0 | MIT | 解析配方 JSON（容器读取校验；T3 版面配方） | 头文件，编译进库 | `github.com/nlohmann/json` 发布附件 `json.hpp`，SHA-256 `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63` |
| zlib | 1.3.2 | zlib 许可 | TIFF ZIP 条带解压；复现 libtiff / OpenCV 写出的 ZIP 条带（经典 zlib `compress2`） | 静态链入 | `github.com/madler/zlib` 发布包 `zlib-1.3.2.tar.gz`，SHA-256 `bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16` |
| libdeflate | 1.26 | MIT | 复现 tifffile / imagecodecs 写出的 ZIP 条带 | 静态链入 | `github.com/ebiggers/libdeflate` 标签 v1.26 源码包，SHA-256 `bba03fffc5538576213675ce6968fcff6ce2e67d82e4d5febea2d05f9f13cf85` |
| SQLite | 3.54.1 | 公有领域 | 后台服务的本地索引（T6） | 合并版源码静态链入 `iis-service`（不进 `iis.dll`） | sqlite.org 发布包 `sqlite-amalgamation-3540100.zip`，官方 SHA3-256 `92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30` 已核对；锁定 SHA-256 `0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9` |
| Dear ImGui | 1.92.9b | MIT | 免安装查看器的界面（含 Win32 / Direct3D 11 后端） | 源码静态链入 `iis-viewer.exe`（只在 Windows 构建） | `github.com/ocornut/imgui` 标签 v1.92.9b 源码包（2026-07-31），SHA-256 `21d8a0a565e85dce943e375db00812c2f3f0ab21f3f0f7964e364a63422d7f99` |
| 站酷小薇 ZCOOL XiaoWei | 2018 | SIL OFL 1.1 | 查看器的中文标题字体（三溪外观规范） | 嵌在 `iis-viewer.exe` 资源里；许可证随安装包 | 复制自三溪寻影 `app/assets/fonts/`（来源 googlefonts/zcool-xiaowei），SHA-256 `a42b620140f493db42f741351dfbf343c0936d58588ee8004b8b2a218d997ff1` |
| Bricolage Grotesque | 可变字体 | SIL OFL 1.1 | 查看器的英文标题字体（关于框） | 同上 | 复制自三溪寻影 `app/assets/fonts/`，SHA-256 `413e7357809ddd12fd80a96a8a396de0e401638d4acd3cb3e37532f0472ac682` |
| JetBrains Mono | 可变字体 | SIL OFL 1.1 | 查看器里的路径、校验值、像素值 | 同上 | 复制自三溪寻影 `app/assets/fonts/`，SHA-256 `48715a42ec242c21e9f02692891e147d022299a52e48d5e413e1a942193ffeda` |
| Monocypher | 4.0.3 | CC0-1.0 / BSD-2-Clause 双许可（按 BSD-2 使用） | 离线授权的 Ed25519 签名与校验（RFC 8032） | 源码静态链入 `iis-service`、`iis-cli`、`iis-license` | `github.com/LoupVaillant/Monocypher` 发布包 `monocypher-4.0.3.tar.gz`（2026-06-15，修复 EdDSA / Ed25519 签名的计时泄露），SHA-256 `8cc9bc341a66249016db9bd70e9142d8d0aef9945973744b1ac05dbc55d8ee66` |

## 移植的算法代码

| 来源 | 许可证 | 移植内容 | 位置 |
|---|---|---|---|
| imagecodecs `imcd.c`（v2026.10.10，© 2008-2026 Christoph Gohlke） | BSD-3-Clause | `imcd_packbits_encode` 与 `imcd_lzw_encode` 的算法，用于逐字节复现 tifffile 写出的 PackBits / LZW 条带 | `src/core/src/tiffcodec.cpp` |
| libtiff `tif_lzw.c` 的编码行为（经 `bench/layout/lzw_libtiff.py` 转写） | libtiff 许可（BSD 类，允许闭源） | LZW 编码的码表重置、码宽、压缩率检查规则，用于复现 OpenCV / libtiff 写出的 LZW 条带 | 同上 |

BSD-3-Clause 要求：二进制分发时在文档中保留版权声明与许可条款。T10 安装包须附带上述各项的许可证全文。

开源解码器 `decoder/`（iis-decode）本身以 BSD-3-Clause 发布（`decoder/LICENSE`，含移植的 imagecodecs 算法的版权声明），只依赖 CharLS、zstd、zlib、libdeflate、nlohmann/json，版本与上表相同；可脱离本仓库单独构建。

版本说明：

- **CharLS 不得低于 2.4.5。** 2.4.5（2026-10-12）修复了“码流在一元码中间截断时死循环”，
  并修复了影响 2.4.4 及以前版本的安全漏洞（GHSA-vx55-856r-qqxw）。阶段 0 的 imagecodecs 2026.10.10 内置 2.4.4，
  编码输出与 2.4.5 逐字节相同（已对照），见 `FINDINGS-阶段1.md`。
- zstd 1.5.7 与阶段 0 imagecodecs 内置版本相同，级别 19 输出逐字节一致。
- 构建选项：zstd 关闭多线程与旧格式支持；CharLS 不构建测试与示例；libdeflate 只构建静态库、不含 gzip；SQLite 关闭扩展加载与旧接口（`SQLITE_OMIT_LOAD_EXTENSION`、`SQLITE_OMIT_DEPRECATED`、`SQLITE_DQS=0`）。
- **zlib 与 libdeflate 的版本不是随意可升的。** 复现式还原要求重编码与原文件逐字节一致，压缩器实现变了，
  已有容器里记录为 `zlib` / `libdeflate` 的条带就可能无法还原。升级前必须用全部回归样本验证输出不变，
  否则须同时保留旧版本实现。zlib 1.3.2 与 libdeflate 1.26 与阶段 0 的 imagecodecs 内置版本相同，已逐字节对照。

## 运行库

| 平台 | 运行库 | 说明 |
|---|---|---|
| Windows | MSVC 静态 CRT（`/MT`） | `iis.dll` 只依赖 `KERNEL32.dll`，干净系统无需安装 VC++ 运行库 |
| Linux | libstdc++、libgcc_s、glibc（动态） | libstdc++ / libgcc 适用 GCC Runtime Library Exception，可用于闭源产品 |

## 仅构建与测试使用（不随产品分发）

| 名称 | 版本 | 许可证 | 用途 |
|---|---|---|---|
| CMake | ≥ 3.21（本机 VS 自带 3.31.6；Debian bookworm 3.25） | BSD-3-Clause | 构建 |
| MSVC（Visual Studio 2022） | 19.44 | 商业 | Windows 构建 |
| GCC | 12.2（Debian bookworm） | GPL-3.0（编译器本身，不链入产品） | Linux 构建 |
| debian:bookworm-slim | Docker 官方镜像 | 各软件包自身许可 | Linux 本地 CI 容器 |
| WiX Toolset | 5.0.2（dotnet 工具，装在 `.cache/tools/wix`） | MS-RL | 生成 MSI 安装包（T10）；安装包中不含 WiX 的运行时代码，也没有自定义操作。WiX 6 起二进制附带“开源维护费”条款，故锁定 5.x |
| Windows SDK signtool | 10.0.26100 | 随 Windows SDK | 代码签名（证书由负责人提供） |
| jsonschema（Python） | 4.26.0（依赖 referencing 0.37.0、jsonschema-specifications 2025.9.1、rpds-py 2026.9.1、attrs 26.1.0） | 均为 MIT | 测试中校验配方 JSON Schema |

单元测试框架为项目自写（`src/core/tests/testing.hpp`），无第三方依赖。
阶段 0 评估工具（`bench/`，Python）的依赖见 `requirements.txt`，不属于产品。
