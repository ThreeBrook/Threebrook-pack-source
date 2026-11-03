# libiis 内核库

产品内核：格式识别、版面整理、编解码、容器读写、写入自检的原生实现（C++17），对外只暴露 C 接口
`include/iis/iis.h`，产出 Windows `iis.dll` 与 Linux `libiis.so`。阶段 0 的 Python 实现（`bench/`）是它的参考实现，
同一输入的结果必须与之一致。

当前进度：
- T1 编码库集成与基础接口——JPEG-LS（CharLS）、zstd、SHA-256、CRC32C；
- T2 容器 v1 读写（规格 `spec/container-v1.md`，配方 JSON Schema `spec/recipe-v1.schema.json`）；
- T3 格式处理器：BMP、TIFF（未压缩 / 多页 / 条带 / 图块 / 8~16 位 / II/MM / BigTIFF / LZW·ZIP·PackBits 复现式还原）、
  RAW（含 Mono10p / Mono12p / Mono12Packed，参数来自规则或同名参数文件）、Bayer 检测与四相位拆分；单文件内存内压缩管线；
- T4 文件压缩的原子提交、启动恢复、线程池；故障注入测试；
- 无文件头 RAW 的参数自动识别（`src/rawguess.cpp`，由 `raw_auto` 打开）；
- 包（`.iisz`，`src/package.cpp`，规格 `spec/package-v1.md`）：文件夹 ↔ 一个标准 ZIP 包，每个文件按单文件规则压缩并自检，
  写临时文件、读回核对后才改名；解包核对 CRC-32 与 SHA-256、不覆盖、拒绝不安全的名称。C++ 接口（命令行与查看器直接链接
  `iis_static` 使用），不经 C 接口导出。

## 构建

依赖源码包会先在 `.cache/deps/` 里找，没有才下载并按 SHA-256 校验（见 `cmake/deps.cmake`、`THIRD_PARTY.md`）。

Windows（Visual Studio 2022，含“使用 C++ 的桌面开发”）：

```bash
powershell -ExecutionPolicy Bypass -File scripts\ci-windows.ps1
```

脚本完成：配置 → Release 构建 → 单元测试 → 检查 `iis.dll` 只导出 `iis_*`、不依赖 VC++ 运行库。
加 `-Parity` 再跑与 Python 参考实现的全量对照。产物在 `build\win-x64\src\core\Release\`。

Linux（用本机 Docker 的 `debian:bookworm-slim`，容器用完即删，apt 包缓存在 `.cache/apt/`）：

```bash
sh scripts/ci-linux.sh
```

在 Linux 主机上也可直接构建：

```bash
cmake -S . -B build/linux-x64 -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build/linux-x64 -j
```

## 接口约定（摘要，详见 `iis.h` 注释）

- 返回 `int` 状态码：`IIS_OK`（0）成功，负数为 `IIS_E_*`；`iis_last_error()` 给出本线程最近一次失败的中文说明。
- 库分配的输出放在 `iis_buffer`，必须用 `iis_buffer_free()` 释放。
- 像素紧密排列；有效位 ≤ 8 每样本 1 字节，9~16 位每样本 2 字节（本机字节序）；多分量按像素交织。
- 任何 C++ 异常都不越过接口边界；损坏输入返回 `IIS_E_CORRUPT`，不崩溃、不卡死。
- JPEG-LS 只做无损：编码固定 NEAR = 0，解码遇到有损码流返回 `IIS_E_UNSUPPORTED`；
  编码前检查样本值不超过位深（CharLS 本身不检查，超出会产生还原不一致的码流）。
- 3 分量按像素交织、4 分量按行交织，与 imagecodecs 一致；不支持 2 分量。
- 版本：`iis_version()`、`iis_abi_version()`（C 接口不兼容变更时加一）、`iis_build_info()`（含编码库版本）。

## 容器接口（T2）

| 用途 | 函数 |
|---|---|
| 写 | `iis_writer_create_file`（只新建，已存在不覆盖）/ `iis_writer_create_memory` → `iis_writer_add`、`iis_writer_add_skeleton` 逐段追加 → `iis_writer_finish`（可落盘）→ `iis_writer_free`（未完成时删除不完整文件） |
| 读 | `iis_reader_open_file` / `iis_reader_open_memory`（结构校验）→ `iis_reader_info`、`iis_reader_segment`、`iis_reader_find` → `iis_reader_read`（取校验后的原样内容）、`iis_reader_decode`（解码）、`iis_reader_ranges`、`iis_reader_recipe` |
| 校验 | `iis_reader_verify(r, deep)`：每段 CRC、区间表、配方外层；deep 再还原并比对 SHA-256 |
| 还原 | `iis_reader_restore` / `iis_reader_restore_file`（先在内存还原并比对 SHA-256，通过后才建文件；目标已存在返回 `IIS_E_CONFLICT`） |
| 兜底 | `iis_pack_bytes(data, size, zstd_level)`：整文件 zstd（或原样）容器 |

规则：段数据在 CRC32C 校验通过后才交给解码器；损坏一律返回 `IIS_E_CORRUPT` 并给出位置说明。

## 识别与压缩接口（T3）

| 用途 | 函数 |
|---|---|
| RAW 规则 | `iis_raw_rules_parse(json)` / `iis_raw_rules_free`：`{"raw": [{"match": "cam3/*.raw", "width": …, "height": …, "pixel_format": "Mono12p", "stride": 可选, "offset": 可选}]}`，按顺序取第一条匹配（通配语义同 Python fnmatch，`*` 可跨目录，Windows 不区分大小写） |
| 识别 | `iis_detect(data, size, opt, &json)`：类别、容器、尺寸、位深、有效位、移位、压缩、Bayer 候选等 |
| 压缩 | `iis_compress(data, size, opt, &container, &result)`：识别 → 拆分 → JPEG-LS → 写容器 → 自检；`result.method` 为 1 图像 / 2 整文件 zstd / 3 保留原文件，`detail` 给出类别或兜底原因，另报三段耗时 |
| 还原 | 容器接口的 `iis_reader_restore`：原样字节 + 按配方版面重组像素，SHA-256 比对通过才交付 |

`iis_compress_options`：`fallback_zstd_level`（默认 19）、`raw_rules`、`rel_path_utf8`（匹配规则用的相对路径）、
`raw_sidecar_json`（同名参数文件 `xxx.json` 的内容，规则未命中时使用；只支持 JSON）、`min_saving`（收益阈值）、
`raw_auto`（C 接口版本 3 起：规则与参数文件都没有的无文件头数据，按内容自动识别 RAW 参数，见下）。

处理规则与阶段 0 参考实现一致：无文件头 RAW 只有命中规则或参数文件才按图像压缩（不按文件大小猜参数）；
`raw_auto` 打开时（打包、右键压缩、命令行与查看器都打开，后台服务不打开）再自动识别（`src/rawguess.cpp`）：
对 Mono8、16 位小端、Mono10p、Mono12p、Mono12Packed 与每个能整除像素数的行宽（64~65535，至少 8 行），
抽样 48 行，用 JPEG-LS 同款 MED 预测估计压缩后大小，取最小的；Bayer 图另试同色相邻的预测；
行尾每行都是 0 的若干列视为行填充；估计压不到存储位数的 90% 时认为不是图像。猜错不影响正确性，
只影响压缩比，所以用猜出的参数压缩后再试 zstd，取小的。评估见 `tools/rawguess_eval.py`；
已压缩 TIFF 只有找到能逐字节重编码的编码器才拆分；拆分、编码或自检任何一步失败都整文件 zstd 兜底；
按图像压缩后不小于原文件时也兜底，zstd 仍不能缩小则保留原文件。
Python 参考实现 `bench/container.py` 与内核逐字节对照（同样的段顺序写出相同的字节、互相读取）。

## 文件压缩、原子提交与恢复（T4）

| 用途 | 函数 |
|---|---|
| 压缩一个文件 | `iis_compress_file(path, opt, &result)`：X → X.iis（同目录），按 `opt.release` 保留或释放原文件 |
| 启动恢复 | `iis_recover_dir(dir, recursive, opt, 回调, user, &report)`：在开始压缩之前调用 |
| 多线程 | `iis_pool_create(线程数, opt, 回调, user)` → `iis_pool_submit` → `iis_pool_wait` → `iis_pool_destroy`；`iis_pool_pause` 暂停取新文件 |

提交步骤（任何一步失败都不动原文件，并删除临时文件）：

1. 读原文件（共享读写删除，不妨碍视觉软件），计算 SHA-256；内容已是 IIS 容器、名字是 `.iis` / `.iistmp` 的跳过；
2. 已有 X.iis：深度校验一致 → 不重复压缩；内容是旧的（原文件被同名覆盖）→ 原文件优先，稍后原子替换（`keep_stale` 时报冲突）；
   内容相同但已损坏 → 用原文件重新生成；不是 IIS 容器 → `IIS_E_CONFLICT`，不碰任何文件；
3. 内存内压缩与自检（同 `iis_compress`）；压了更大或收益低于 `min_saving` → 不生成 .iis；
4. 写 `X.iis.<8 位十六进制>.iistmp`（只新建）→ `FlushFileBuffers` / `fsync` → 读回与已自检的容器逐字节比较；
5. 改名为 X.iis：Windows `MoveFileEx` 写穿，不覆盖（替换旧内容时才覆盖）；Linux `renameat2(RENAME_NOREPLACE)`（不支持时用硬链接）后同步目录；
6. 释放原文件（`IIS_RELEASE_DELETE`）：独占打开（只共享读：别人不能写、改名、删），长度与 SHA-256 都与第 1 步相同才删除；
   Windows 用 POSIX 语义删除（名字立即消失，视觉软件随即可写同名新文件）。被占用 / 已修改 / 只读时不删，结果中说明。

恢复规则（原文件优先，不认识的文件一律不动）：

| 发现 | 处理 |
|---|---|
| `X.iis.*.iistmp`，原文件 X 在 | 删除临时文件 |
| 临时文件，X 与 X.iis 都不在 | 深度校验通过则改名为 X.iis，否则保留 |
| 临时文件，X 不在、X.iis 已在 | 保留，报告 |
| X 与 X.iis 同在（释放模式） | X.iis 深度校验通过且与 X 一致才释放 X；否则计入 `pending`，原文件保留待重新压缩 |

错误码：`IIS_E_NOT_FOUND`（原文件不存在）、`IIS_E_BUSY`（被占用）、`IIS_E_NO_SPACE`（磁盘满）、`IIS_E_CONFLICT`（目标已存在不能替换）。
Windows 下所有文件接口支持超过 260 字符的长路径。

## 测试

| 测试 | 内容 |
|---|---|
| `iis_core_tests` | SHA-256 / CRC32C 标准向量与分段计算；CRC32C 硬件与软件实现一致；JPEG-LS 2~16 位、1/3/4 分量、极端尺寸往返；非法参数、超位深样本、有损码流、非交织码流；2000 次随机损坏 / 截断不崩溃不卡死；zstd 往返、长度不符、多余字节、损坏 |
| `iis_c_abi_smoke` | 以纯 C 编译并链接动态库，验证头文件与导出符号 |
| `iis_core_tests`（容器部分） | 兜底容器往返；与按规格独立拼装的容器逐字节相同；完整容器各段读取；**逐字节改动每个位置、每种截断长度、尾部追加**都必须报错；3000 次随机损坏与纯垃圾输入不崩溃；版本兼容规则（主 / 次版本、加长头部与段表项、特性位、未知关键 / 非关键段）；区间表规则；写入参数校验；文件读写（不覆盖、未完成删除、中文路径） |
| `bench/tests/test_native.py` | 通过 ctypes 调用动态库，与 imagecodecs 逐字节对照（未构建时跳过） |
| `bench/tests/test_container.py` | Python 参考实现往返；与内核逐字节相同、互相读取；两边对每个字节的损坏都报错；配方 JSON Schema 合法、正例通过、反例拒绝 |
| `iis_core_tests`（格式部分） | 在 C++ 里生成 BMP（1/3/4 通道、行序、垃圾填充、文件尾）、TIFF（8/16 位、1/3 分量、II/MM、未压缩 / LZW / ZIP / PackBits、Predictor 2）、RAW（打包格式、文件头、规则与参数文件）变体，压缩后只凭容器还原逐字节一致；3000 次坏文件头模糊测试不崩溃、还原一致；通配符规则 |
| `bench/tests/test_tiffcodec.py` | 经 `iis-devtool`：两种 LZW、PackBits（整块 / 按行）、zlib 与 libdeflate（1/6/9 级）编码与 imagecodecs / Python 参考逐字节相同，解码正确 |
| `bench/tests/test_native_formats.py` | 合成小数据集与 140 个随机 BMP / TIFF / RAW 变体：识别逐字段、平面像素与 JPEG-LS 码流、原样字节区间、附加字节、复现编码器与 Python 参考实现一致，配方符合 Schema；1500 次坏文件头模糊测试；图像容器逐字节损坏全部检出 |
| `iis_core_tests`（提交部分） | 基本提交 / 已存在 / 旧内容替换 / 非 IIS 目标 / 损坏修复 / 跳过 / 收益阈值 / 同名参数文件；**每一步注入失败**（含磁盘满）后原文件不变、无残留；**在 13 个步骤各强制终止子进程**（再在恢复中终止一次），之后原文件或完整 .iis 至少其一、恢复后收敛；编码 / 自检失败兜底；压缩中原文件被改写不释放；原文件被占用 / 只读；恢复的各种残留；中文长路径；线程池 48 个文件去重、暂停；设置 `IIS_TEST_SMALL_DIR` 时测真实磁盘写满（Linux CI 用 1 MB tmpfs） |
| `tools/t3_parity.py` | 阶段 0 全部合成样本的同项对照（见 FINDINGS-阶段1.md） |
| `tools/t3_speed.py` | 与阶段 0 同编码器、同测法的单核吞吐对比 |
| `tools/native_parity.py` | 阶段 0 全部合成样本：识别 + 拆分后的每个像素平面，两边 JPEG-LS 码流、zstd 输出、SHA-256 逐字节对照，并比较速度 |

## 开发工具

`iis-devtool codec <名称> <输入> <输出> [参数]`：直接调用 TIFF 块编解码器，供 Python 对照测试驱动（不随产品发布）。

`iis-perf files <根目录> [--rules 规则.json] [--repeat N] [--cpu K] 相对路径…`：整文件压缩（分段计时）与还原，输出 CSV。

`iis-perf pool <样本根目录> <工作目录> [--threads 1,2,4] [--rules 规则.json]`：把样本复制到工作目录后用线程池原地压缩并释放
（完整提交路径，含落盘），按线程数输出吞吐，最后逐个还原与样本比对。样本目录只读。

`iis-perf`：单线程吞吐测量（JPEG-LS 编码 / 解码、写入自检管线、zstd、SHA-256、CRC32C）。输入 PGM，
可由 `tools/native_parity.py --pgm-out <目录>` 从合成样本导出：

```bash
build\win-x64\src\tools\Release\iis-perf.exe --repeat 3 --cpu 5 .tmp\planes\*.pgm
```
