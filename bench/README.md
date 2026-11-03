# 阶段 0 评估工具（bench / S1）

给定一个图像目录，自动识别文件、测试多种**文件字节级无损**压缩方案、逐字节还原校验，并输出中文评估报告。
用于客户现场：实测能压多少、多快、是否明显优于 Windows NTFS 压缩、后台压缩会不会影响检测，以及代入 ROI。

> 这是评估工具，不是产品。样本目录**只读**：工具不会修改、移动、删除其中任何文件，所有输出写到 `--out`，
> 且输出目录不允许位于样本目录内。工具不联网、不上传任何样本内容。

---

## 1. 安装

要求 Python 3.11+（开发与测试在 Python 3.14 / Windows 10 上进行，Linux 同样可运行，NTFS 对照与 CPU 影响测试自动跳过）。

```bash
python -m venv .venv
```

```bash
.venv\Scripts\python -m pip install -r requirements.txt
```

Linux 下把 `.venv\Scripts\python` 换成 `.venv/bin/python`。依赖版本在 `requirements.txt` 中固定（复现式还原与速度数据依赖具体库版本）。

如果希望依赖缓存与临时文件也留在项目目录（不落到 C 盘），安装前设置：

```bash
set PIP_CACHE_DIR=%CD%\.cache\pip
```

```bash
set TMP=%CD%\.tmp
```

（`.venv/`、`.cache/`、`.tmp/`、`bench/out/` 都已在 `.gitignore` 中。）

主要依赖：numpy、tifffile、imagecodecs（CharLS JPEG-LS / OpenJPH HTJ2K / OpenJPEG / libjxl / libpng / zstd）、
opencv-python-headless（合成数据与模拟检测负载）、PyYAML、psutil、pytest。

---

## 2. 最常用：一条命令出报告

```bash
.venv\Scripts\python -m bench scan D:\样本\客户A --out bench\out\客户A
```

依次运行：识别 → 拆分重组自检 → 压缩对比 → 天花板测试（样本中有序列与空载背景时）→ 背景潜力评估（有 ≥3 张空载背景时）
→ CPU 影响（指定 `--cpuload` 时）→ 报告。输出目录：

| 文件 / 目录 | 内容 |
|---|---|
| `report.html` | **主报告**：单文件、离线可打开、中文 |
| `details.csv` | 逐文件明细（识别结果 + 各方案体积 / 压缩比 / 状态 + NTFS 占用） |
| `summary.json` | 机器可读汇总 |
| `detect/` `layout/` `compress/` `ceiling/` `bgpotential/` `cpuload/` | 各阶段原始结果 |

常用参数：

| 参数 | 说明 |
|---|---|
| `--quick` | 跳过 JPEG XL e7 / e9（最慢的两档），现场赶时间时用 |
| `--jobs N` | 压缩比阶段的并行进程数（每进程单线程），默认约为逻辑核数的一半 |
| `--speed-per-group N` | 测速时每个格式组抽几个文件（默认 1；0 = 不测速） |
| `--raw-config 文件` | RAW 参数配置，默认取 `<样本目录>/raw_config.yaml` |
| `--frames GLOB` / `--background GLOB` | 时序序列与空载背景（相对样本目录），用于天花板测试与背景潜力评估 |
| `--cpuload` | 运行 CPU 影响测试（仅 Windows，约 5 分钟，**需要机器空闲**） |
| `--no-ntfs` | 跳过 NTFS 压缩对照 |
| ROI 参数 | 见第 5 节 |

换 ROI 参数重新出报告，不必重跑测试：

```bash
.venv\Scripts\python -m bench report bench\out\客户A --daily-gb 300 --disk-tb 10 --retention-days 90
```

---

## 3. 各子命令

| 命令 | 任务 | 作用 |
|---|---|---|
| `synth` | T1 | 生成合成测试集（BMP / TIFF / RAW / Bayer / 序列 / 其他），带真值 `manifest.json` 与各类说明 |
| `detect` | T2 | 按内容识别格式（不看扩展名），输出 `detect.json / detect.csv`；有真值时给出正确率 |
| `layout` | T3 | 只拆分再重组（不压缩），逐文件 SHA-256 比对；统计复现式还原成功率 |
| `compress` | T4 | 多方案压缩对比：往返校验、体积算全、单核测速、NTFS 对照 |
| `ceiling` | T5 | 天花板测试 G1 / G2 / G3a / G3b / G3c 与自动结论 |
| `cpuload` | T6 | 模拟检测负载 + 低优先级后台压缩，对比检测耗时（仅 Windows） |
| `scan` | T7 | 一站式评估并生成报告 |
| `report` | T7 | 由 `scan` 输出目录重新生成报告 |
| `bgpotential` | — | 背景压缩潜力评估：空载背景 → 背景参考帧最多能多省多少 |
| `bgsweep` | — | 背景参考收益的敏感性扫描（合成数据） |

每个命令都有 `--help`。示例：

```bash
.venv\Scripts\python -m bench synth --out bench\out\synth --size medium
```

```bash
.venv\Scripts\python -m bench detect bench\out\synth --out bench\out\detect
```

```bash
.venv\Scripts\python -m bench compress D:\样本\客户A --out bench\out\A_compress --schemes zstd19 jpegls htj2k
```

```bash
.venv\Scripts\python -m bench bgpotential D:\样本\客户A --background "工位3/空载_*.bmp" --frames "工位3/*.bmp"
```

```bash
.venv\Scripts\python -m bench cpuload D:\样本\客户A --cores 16 --load 0.6 --compress-cores 1 2 4 --priorities below_normal idle
```

压缩方案 id：`zstd3`、`zstd19`（整文件通用压缩对照）、`png`、`jpegls`、`htj2k`、`jxl3`、`jxl7`、`jxl9`。

---

## 4. RAW 参数配置

无文件头的 RAW 需要参数。来源优先级：**目录 / 文件名规则配置 → 同名参数文件 → 按文件大小推候选**。
没有配置的 RAW 只给出候选参数（`detect.csv` 的 `raw_candidates` 列），**不会被压缩**，确认后写入配置再运行。

规则配置（YAML 或 JSON，默认 `<样本目录>/raw_config.yaml`；按顺序匹配，先匹配者生效；`match` 为相对样本目录的 glob）：

```yaml
raw:
  - match: "工位3/*.raw"
    width: 2448
    height: 2048
    pixel_format: Mono12p        # GenICam PFNC 名称
  - match: "工位5/cam*_*.raw"
    width: 5472
    height: 3648
    pixel_format: Mono16         # 16 位容器；12 位高位对齐等由工具自动检测移位
    stride: 10944                # 可选：行跨距（字节），有行末填充时填写
    offset: 0                    # 可选：文件头字节数
  - match: "工位7/*.raw"
    width: 1920
    height: 1200
    pixel_format: BayerRG8       # 明确 Bayer 排列时用 Bayer 格式名
```

同名参数文件：`xxx.raw` 旁放 `xxx.json`（或 `.yaml`），内容为 `width / height / pixel_format`（可选 `stride / offset`）。

支持的 `pixel_format`：`Mono8 / Mono10 / Mono12 / Mono14 / Mono16`（16 位容器，小端）、`Mono10p / Mono12p / Mono12Packed`（打包），
以及对应的 `BayerRG / BayerGB / BayerGR / BayerBG` + `8 / 10 / 12 / 14 / 16 / 10p / 12p / 12Packed`。

---

## 5. ROI 代入（工控机本地硬盘）

| 参数 | 说明 |
|---|---|
| `--daily-gb` | 每天出图量（GB）；或用 `--daily-images` × `--image-mb` |
| `--debug-daily-gb` | 调试期每天出图量（默认同正常期） |
| `--disk-tb` | 图像盘容量（TB） |
| `--retention-days` | 招标要求保留天数 |
| `--reserve` | 硬盘预留比例（默认 0.10） |
| `--roi-scheme` | 用哪个方案的实测总体压缩比（默认 `jpegls`） |

输出：现有硬盘可保留天数 X → Y、调试期爆盘周期 A → B、满足招标保留天数需配硬盘（原 / 现）与少配 N TB，
以及按 4 / 8 / 10 / 16 TB 盘的块数。不提供参数时用示例参数，并在报告中醒目标注。计算定义见 `FINDINGS.md` 第 3 节。

---

## 6. 报告解读

| 报告章节 | 看什么 |
|---|---|
| 结论摘要 | 推荐方案（默认 JPEG-LS）总体压缩比、对照 zstd / NTFS、**校验不一致必须为 0** |
| 1 样本概览 | 格式分布（按内容识别）、推测写图程序及依据 |
| 2 压缩比与速度 | 总体 / 分格式 / 分目录压缩比；单核编解码 MB/s |
| 3 NTFS 对比 | NTFS 透明压缩 vs 推荐方案的节省量 |
| 4 Bayer 拆分收益 | 直接按灰度压 vs 拆四平面 |
| 5 复现式还原 | 已压缩 TIFF（LZW / ZIP / PackBits）能否逐字节复现；失败的整文件 zstd 兜底 |
| 6 天花板测试 | G1 → G2 → G3 的增益与自动判读；背景潜力评估 |
| 7 CPU 影响 | 后台压缩时检测耗时均值 / P95 / P99 与超周期；**机器不安静时会标注“仅供参考”** |
| 8 失败与兜底 | 每个未能按图像压缩的文件及原因（往返不一致的会被检出并按兜底计入，不会算进压缩比） |
| 9 ROI | 代入结果 |

口径：

- **压缩比** = 原文件总字节 / 存储总字节。存储体积**算全**：像素码流 + 非像素字节（文件头、调色板、IFD、填充等，经 zstd）
  + 配方 + 容器头与段头（按 `docs/05` 第 7 节容器草案估算）。校验失败或不可处理的文件按兜底（zstd 或原样）计入。
- **逐字节校验**：每个文件的每个方案都做“编码 → 解码 → 按原版面重组 → SHA-256 与原文件比对”，一致才计入压缩结果；
  整文件 zstd 兜底同样解压比对。
- **大小单位**：报告与界面按 Windows 资源管理器的口径，1 MB = 1024×1024 字节，统计文件实际大小；
  界面另用小字给出精确字节数与按簇补齐的“占用空间”（与资源管理器属性里的两个数对得上）。ROI 的 GB / TB 按硬盘厂商的十进制口径。
- **速度**：单核（专用子进程绑定一个逻辑核），预热 1 次 + 3 次取中位数，MB/s 按原文件字节计；编码含拆分，解码含重组。
  单次超过 10 s 的方案只测 1 次（`speed.csv` 的 note 列注明）。TIFF-LZW 组的复现式还原在本工具中用纯 Python 实现，速度不代表产品，单列。
- 合成数据的压缩比只说明工具与方法可用，**对外承诺必须以客户样本实测为准**。

---

## 7. 现场测试建议

1. 每个工位 50~500 张原图（OK 与 NG、不同批次），保持原始文件，不要转存；
2. RAW 工位准备好参数配置（宽、高、像素格式、行跨距）；
3. 每个工位另取 **10~20 张空载背景 + 连续 20 张产品图**，用于天花板测试与背景潜力评估；
4. CPU 影响测试请在空闲的机器或客户同型号工控机上运行（关闭虚拟机、编译等后台负载）；
5. 先 `--quick` 快速出一版，确认样本识别无误后再跑完整方案。

---

## 8. 开发与测试

```bash
.venv\Scripts\python -m pytest -q
```

测试覆盖：打包格式往返、合成数据确定性与覆盖度、格式识别（真值 100%、扩展名无关、乱码不崩溃、截断判损坏、移位、Bayer 正反例、
RAW 候选）、拆分重组（全部变体 SHA-256 一致、篡改必被发现、libtiff LZW 码表重置、RAW 头尾与非 0 填充、打包尾字节）、
压缩（全部校验、体积算全、编码器出错必兜底、Bayer 变体）、天花板（残差极值、运动补偿、对齐抗前景、模式选择）、
CPU 影响、报告（全章节、离线、明细行数、只读保护）、背景潜力评估与扫描。pytest 的临时文件写到项目内 `.tmp/pytest`。

代码结构：

| 路径 | 内容 |
|---|---|
| `cli.py` `__main__.py` | 命令行入口 |
| `synth/` | 合成测试数据（场景、传感器噪声、BMP 写入器） |
| `formats/` | BMP / TIFF / RAW 解析与魔数识别（只读） |
| `detect/` | 分类、有效位与移位、Bayer 检测、写图程序推测、真值评估 |
| `layout/` | 拆分模型与可逆整数变换、BMP / TIFF / RAW 重组、复现式还原、libtiff 兼容 LZW、打包格式 |
| `codecs.py` `compress.py` `compress_run.py` `ntfs.py` | 编码器接口、容器体积、压缩对比与测速、NTFS 对照 |
| `ceiling.py` `ceiling_run.py` | 天花板测试 |
| `bgmodel.py` `bgsweep.py` | 背景潜力评估与敏感性扫描 |
| `cpuload.py` `cpuload_run.py` | CPU 影响测试 |
| `scan.py` `report.py` `roi.py` | 一站式流程、报告、ROI |
| `FINDINGS.md` | 开发中的发现、与设计文档不符之处、建议 |

### 打包为免安装 exe（Windows）

```bash
.venv\Scripts\python -m pip install pyinstaller
```

```bash
.venv\Scripts\python -m PyInstaller bench/frozen_entry.py --name iis-bench --onedir --noconfirm --console --distpath bench/out/dist --workpath .tmp/pyinstaller --specpath .tmp/pyinstaller --paths . --collect-submodules bench --collect-all imagecodecs --collect-all tifffile --hidden-import yaml
```

生成 `bench/out/dist/iis-bench/`（约 220 MB，整目录拷到客户机器即可，无需安装 Python），用法与 `python -m bench` 相同：

```bash
iis-bench.exe scan D:\样本\客户A --out D:\评估结果\客户A --quick
```

打包时关于 `jxs.dll / jetraw.dll / heif.dll` 的警告可忽略（本工具不用这些编码器）。
已用打包版验证 synth、scan（含多进程压缩与天花板测试）、cpuload、NTFS 对照。

---

## 9. 图形界面（快速测试用）

```bash
.venv\Scripts\python -m bench gui
```

四个页面，外观按《三溪外观规范》（颜色只用 `bench/gui/web/threebrook-tokens.css` 的变量；标题站酷小薇 / Bricolage Grotesque，
路径与数字 JetBrains Mono；跟随系统浅色 / 深色，也可在设置里固定）：

| 页面 | 作用 |
|---|---|
| 快速测试（Ctrl+N） | ① 选样本文件夹（自动发现 raw_config.yaml、序列与空载背景）→ 选快速 / 完整与可选项 → ② 开始，逐阶段进度、可随时停止 → 右侧实时压缩比、能省的空间、NTFS 对照、还原不一致例数 → ③ 没按图像压缩的文件及原因 → 查看报告 |
| 测试记录（Ctrl+H） | 历次测试（含已停止 / 出错的），查看报告、打开文件夹、删除（只删结果，不碰样本，删除前确认） |
| 容量与 ROI（Ctrl+R） | 选一次测试，填出图量、硬盘、保留天数，实时算出可保留天数、爆盘周期、少配硬盘；“写入报告”更新该次报告第 9 节 |
| 设置（Ctrl+,） | 主题、结果保存位置、并行进程数、默认模式；关于（版本、CPU、编码库） |

实现：pywebview + Windows 自带的 Edge WebView2，页面在 `bench/gui/web/`（原生 HTML/CSS/JS，无外部资源），
接口在 `bench/gui/app.py`（与命令行共用同一套引擎，测试在后台线程运行，进度经 `bench/progress.py` 回调，停止时立即终止子进程）。
设置与结果默认保存在：源码运行时 `bench/out/gui/`，打包版在 exe 旁的 `data/`（不写 C 盘）。
页面可用浏览器直接打开 `bench/gui/web/index.html?mock=running`（`idle / running / done`，可加 `&theme=dark&page=roi`）查看演示数据，便于调样式。

打包为免安装的图形界面版（无控制台窗口）：

```bash
.venv\Scripts\python -m PyInstaller bench/frozen_gui_entry.py --name iis-bench-gui --onedir --noconfirm --windowed --distpath bench/out/dist --workpath .tmp/pyinstaller-gui --specpath .tmp/pyinstaller-gui --paths . --collect-submodules bench --collect-all imagecodecs --collect-all tifffile --collect-all webview --hidden-import yaml --hidden-import clr --add-data "bench/gui/web;bench/gui/web"
```

打包后可用隐藏的自检开关验证（隐藏窗口自动跑一遍小样本，结果写到 `data/selftest.json`）：

```bash
bench\out\dist\iis-bench-gui\iis-bench-gui.exe --selftest D:\样本\小样本
```

---

## 10. 已知局限

- Halcon / VisionPro / 相机 SDK 的写图程序指纹目前只按 Software 等文本标签判断，需真实样本校准。
- Bayer 能可靠判断 G 的位置，R / B 只给两个候选（需要完整排列时在 RAW 配置中写 Bayer 格式名）。
- 无配置 RAW 按文件大小推候选时，宽度 W / 2W / W/2 的区分度只有 3%~20%，必须人工确认。
- 位深非 8 / 16 的 TIFF（如 12 位紧凑打包）、浮点 TIFF、JPEG 压缩 TIFF、RLE BMP 当前按“不处理”兜底。
- 测速与 CPU 影响结果受测试机负载影响，正式结论请在空闲机器或目标机型上复测。
