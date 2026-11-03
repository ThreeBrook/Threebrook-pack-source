# iis-decode：IIS 压缩文件的独立开源解码器

把 IIS 工业图像压缩存储生成的 `.iis` 文件还原为**与原文件逐字节相同**的 BMP / TIFF / RAW 等文件，并按文件里记录的
SHA-256 核对。目的：客户不依赖 IIS 设备、服务或任何闭源软件，也能取回自己的数据。

- 许可证：BSD-3-Clause（见 `LICENSE`）；
- 依据：公开的容器规格 `spec/container-v1.md`（容器 v1、配方第 13 节）；
- 依赖：只用标准编码库——CharLS 2.4.5（JPEG-LS）、zstd 1.5.7、zlib 1.3.2、libdeflate 1.26，以及 nlohmann/json 3.12.0；
- 实现：一个约 900 行的 C++17 文件 `src/iis_decode.cpp`，与 IIS 自身的实现相互独立，便于审阅与移植。
  已压缩 TIFF 的复现编码器（LZW、ZIP、PackBits）必须与写图程序逐字节相同，因此 zlib 与 libdeflate 须保持上述版本。

## 构建

单独构建（自动下载上述依赖并按 SHA-256 校验）：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build --config Release
```

## 使用

```bash
iis-decode 图像.bmp.iis
```

还原到同目录（去掉 `.iis`，已存在不覆盖），或 `-o 输出文件` 指定位置；`--check` 只校验能否还原、不写文件。
SHA-256 不一致、数据损坏、版本不支持时报错且不留下任何文件。

## 支持范围（容器 v1）

| 内容 | 支持 |
|---|---|
| 段编码 | 不压缩、zstd、JPEG-LS（无损） |
| 原格式 | 整文件字节（`bytes`）、BMP（8 位调色板 / 24 / 32 位，行序、行填充、Alpha）、RAW（Mono / Bayer 8~16 位，Mono10p / 12p / 12Packed 打包）、TIFF（II / MM、BigTIFF、多页、条带 / 图块、平面配置 1 / 2、8 / 16 位、Predictor 2、未压缩 / LZW / ZIP / PackBits 复现式还原） |
| 不支持 | HTJ2K、JPEG XL 编码的段与领域预测层（容器 v1 预留，遇到时报“不支持”，不会给出错误结果） |

## English summary

`iis-decode` is a standalone, BSD-licensed decoder for IIS container v1 files. It reproduces the original file byte for
byte (verified against the stored SHA-256) using only standard codec libraries (CharLS, zstd, zlib, libdeflate).
