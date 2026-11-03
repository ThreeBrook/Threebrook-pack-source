# IIS 容器格式 v1（`.iis`）

状态：v1.0 定稿（阶段 1 T2）。基于 `docs/05` 第 7 节 v0 草案，差异见第 15 节。
本规格公开，任何人可据此独立实现读写器。参考实现：`src/core`（C++）、`bench/container.py`（Python，与 C++ 逐字节对照）。

## 1. 目标

1. **逐字节还原**：容器保存还原原文件所需的全部内容——原样字节段、标准像素码流、配方；还原结果的 SHA-256 必须等于头部记录值。
2. **任何损坏都能发现**：文件中每一个字节都被某个 CRC32C 或固定值覆盖；改动任意单个字节，完整校验必然失败。
3. **流式写入**：写入方按段依次追加，不需要预知段数与段长，也不需要回头改写；段表与文件尾最后写。
4. **随机读取**：读取方从文件尾定位段表，可直接读取任一段，无需扫描全文件。
5. **标准与开放**：像素段是标准码流（JPEG-LS 等），可被任何标准解码器直接解开；配方为公开定义的 JSON。
6. **可演进**：主 / 次版本号、不兼容 / 兼容特性位、段的“关键”标志，规定老读取方遇到新文件时的行为。

## 2. 总体结构

```
┌──────────────┐ 0
│ 文件头 72 B   │  魔数、版本、特性位、原文件长度与 SHA-256、头部 CRC
├──────────────┤ 72
│ 段记录 #0    │  段头 40 B + meta + data
│ 段记录 #1    │
│ …            │  段与段首尾相接，无填充
├──────────────┤ index_offset
│ 段表         │  16 B + 40 B × 段数 + 4 B CRC
├──────────────┤
│ 文件尾 32 B   │  段表位置、容器总长、文件尾 CRC、结束魔数 "IEND"
└──────────────┘ container_size
```

## 3. 基本约定

- 所有整数为**无符号小端序**；u16 / u32 / u64 分别为 2 / 4 / 8 字节。
- 偏移与长度均以字节计，从文件开头起算。
- 结构之间没有对齐填充。
- **CRC32C**：Castagnoli 多项式（反射形式 0x82F63B78），初值 0xFFFFFFFF，结果取反——与 iSCSI / SSE4.2 `crc32` 指令相同。
  校验向量：`CRC32C("123456789") = 0xE3069283`。
- “保留”字段写入方必须写 0，读取方忽略其值（但它仍在 CRC 覆盖范围内）。
- 文本一律 UTF-8，不带 BOM。

## 4. 文件头（72 字节）

| 偏移 | 类型 | 字段 | 说明 |
|---|---|---|---|
| 0 | 8 B | magic | `49 49 53 43 4F 4E 54 00`（"IISCONT\0"） |
| 8 | u16 | version_major | 1 |
| 10 | u16 | version_minor | 0 |
| 12 | u32 | header_size | 文件头总长；v1.0 为 72，以后的次版本可以加长 |
| 16 | u32 | incompat_flags | 不兼容特性位：读取方不认识其中任何一位就必须拒绝读取（第 12 节） |
| 20 | u32 | compat_flags | 兼容特性位：不认识可以忽略；v1.0 未定义任何位 |
| 24 | u64 | orig_size | 原文件字节数 |
| 32 | 32 B | orig_sha256 | 原文件 SHA-256 |
| 64 | u32 | reserved | 0 |
| header_size − 4 | u32 | header_crc | 对 `[0, header_size − 4)` 的 CRC32C；v1.0 即偏移 68 |

不兼容特性位：

| 位 | 名称 | 含义 |
|---|---|---|
| 0 | PREDICTION | 含领域预测层段（第 14 节），v1.0 读写方都不支持；写入方不得置位 |
| 1~31 | — | 保留 |

## 5. 段记录

每个段 = 40 字节段头 + `meta_len` 字节 meta + `data_len` 字节 data。

| 偏移 | 类型 | 字段 | 说明 |
|---|---|---|---|
| 0 | 4 B | magic | `49 53 45 47`（"ISEG"），便于人工排查与抢救 |
| 4 | u16 | type | 段类型（第 6.1 节） |
| 6 | u16 | codec | data 的编码（第 6.2 节） |
| 8 | u16 | flags | 段标志（第 6.3 节） |
| 10 | u16 | id | 同类段内的编号：PLANE、EXTRA 与配方中的 `planes[].id`、`extras[].id` 对应；SKELETON 从 0 起编号；RECIPE、META 为 0 |
| 12 | u32 | meta_len | meta 长度（不压缩，按类型解释） |
| 16 | u64 | raw_len | data 解码后的字节数（codec = none 时等于 data_len） |
| 24 | u64 | data_len | data 在容器中的字节数 |
| 32 | u32 | data_crc | 对 meta 与 data 连在一起的 CRC32C |
| 36 | u32 | head_crc | 对段头 `[0, 36)` 的 CRC32C |
| 40 | meta_len B | meta | |
| 40 + meta_len | data_len B | data | |

`(type, id)` 在一个容器内唯一。

## 6. 段类型、编码与标志

### 6.1 段类型

| 值 | 名称 | 关键 | 数量 | meta | data |
|---|---|---|---|---|---|
| 1 | RECIPE | 是 | 恰好 1 | 空 | 配方 JSON（第 13 节）；codec 为 none 或 zstd |
| 2 | SKELETON | 是 | 0~n | 区间表（第 6.4 节） | 原文件中这些区间的原样字节按顺序连接；codec 为 none 或 zstd |
| 3 | PLANE | 是 | 0~n | 空 | 像素码流；几何与位深见配方 `planes[]` |
| 4 | EXTRA | 是 | 0~n | 空 | 版面整理需要的附加字节（非零行填充、打包尾字节等），含义见配方 `extras[]` |
| 5 | META | 否 | 0~1 | 空 | 描述性 JSON（第 13.6 节），不参与还原 |
| 16、17 | 预测层 | 是 | — | — | 预留给领域预测层（第 14 节），v1.0 不得出现 |
| 其他 | — | — | — | — | 保留 |

### 6.2 编码（codec）

| 值 | 名称 | 适用段 | 说明 |
|---|---|---|---|
| 0 | none | 全部 | 不编码 |
| 1 | zstd | 全部 | 单个 zstd 帧，帧头必须带原始长度，帧后不得有多余字节（PLANE 用 zstd 时 data 为按下文布局排列的像素） |
| 2 | jpegls | PLANE | ISO/IEC 14495-1 码流，只允许 NEAR = 0（无损） |
| 3 | htj2k | PLANE | 预留（ISO/IEC 15444-15 可逆），v1.0 不写 |
| 4 | jpegxl | PLANE | 预留（ISO/IEC 18181 无损），v1.0 不写 |

PLANE 段解码后的字节布局：行与行紧密相接；有效位 ≤ 8 每样本 1 字节，9~16 位每样本 2 字节**小端**；多分量按像素交织。
`raw_len` 按此布局计算，解码长度不符即视为损坏。

### 6.3 段标志

| 位 | 名称 | 含义 |
|---|---|---|
| 0 | CRITICAL | 还原必需。读取方遇到不认识的段类型：带此位则拒绝还原，不带则跳过 |
| 1~15 | — | 保留 |

已定义类型的 CRITICAL 位必须与第 6.1 节“关键”列一致，否则视为损坏。

### 6.4 SKELETON 区间表

meta 由 LEB128 无符号变长整数组成：

```
count
gap_0  len_0
gap_1  len_1
…
```

第 i 个区间起点 = 上一区间终点 + gap_i（第一个区间相对 0），长度 len_i（> 0）。
因此区间按升序、互不重叠。约束：所有区间终点 ≤ orig_size；`raw_len` = 各区间长度之和。
同一容器的多个 SKELETON 段之间区间也不得重叠。变长整数不得多余补零字节（最短编码），单个值不超过 2^64−1。

## 7. 段表

| 偏移 | 类型 | 字段 | 说明 |
|---|---|---|---|
| 0 | 4 B | magic | `49 49 44 58`（"IIDX"） |
| 4 | u32 | count | 段数 |
| 8 | u32 | entry_size | 每项字节数；v1.0 为 40，以后的次版本可以加长（读取方只读认识的前 40 字节） |
| 12 | u32 | reserved | 0 |
| 16 | entry_size × count | entries | 按段在文件中的顺序 |
| 末尾 | u32 | index_crc | 对 `[0, 16 + entry_size × count)` 的 CRC32C |

每项（v1.0 的 40 字节）：

| 偏移 | 类型 | 字段 |
|---|---|---|
| 0 | u64 | offset（段记录起点） |
| 8 | u16 | type |
| 10 | u16 | codec |
| 12 | u16 | flags |
| 14 | u16 | id |
| 16 | u32 | meta_len |
| 20 | u32 | data_crc |
| 24 | u64 | raw_len |
| 32 | u64 | data_len |

段表各项必须与对应段头中的同名字段完全一致。

## 8. 文件尾（32 字节）

| 偏移 | 类型 | 字段 | 说明 |
|---|---|---|---|
| 0 | u64 | index_offset | 段表起点 |
| 8 | u64 | index_size | 段表总长（含 index_crc） |
| 16 | u64 | container_size | 整个容器的字节数（含文件尾） |
| 24 | u32 | trailer_crc | 对 `[0, 24)` 的 CRC32C |
| 28 | 4 B | magic | `49 45 4E 44`（"IEND"） |

## 9. 读取与损坏检测

### 9.1 打开（随机读取的前提）

1. 读文件头：魔数、主版本、`header_size ≥ 72`、header_crc；不认识的不兼容特性位 → 拒绝。
2. 读最后 32 字节：结束魔数、trailer_crc、`container_size` 等于实际长度（截断或尾部追加都会在这里发现）。
3. 检查 `header_size ≤ index_offset`、`index_offset + index_size + 32 = container_size`、`index_size = 16 + entry_size × count + 4`。
4. 读段表：魔数、`entry_size ≥ 40`、index_crc。
5. 段表一致性：第一个段从 `header_size` 开始，每段终点 = 下一段起点，最后一段终点 = index_offset；
   各段 `40 + meta_len + data_len` 不溢出；`(type, id)` 唯一；RECIPE 恰好 1 个；已知类型的 CRITICAL 位正确；
   带 CRITICAL 的未知类型 → 打开成功但不可还原。

### 9.2 读取单个段

读 40 字节段头 → 魔数、head_crc → 与段表项逐字段比对 → 读 meta 与 data → data_crc。
**全部通过后才把 data 交给解码器**（解码库在畸形输入上可能卡死或出错，见 `FINDINGS-阶段1.md` 2.1）。
解码后长度必须等于 raw_len。

### 9.3 完整校验

打开 + 逐段读取校验（9.2）+ 检查 SKELETON 区间表合法且互不重叠。“深度校验”再加：逐段解码、还原原文件并比对 SHA-256。

### 9.4 覆盖关系（为什么任意单字节损坏都能发现）

| 区域 | 由谁保护 |
|---|---|
| 文件头 `[0, header_size)` | header_crc（CRC 字段本身被改则不匹配） |
| 段头 | head_crc；且与段表逐字段比对 |
| meta + data | data_crc（data_crc 字段在段头内，受 head_crc 保护） |
| 段表 | index_crc |
| 文件尾 | trailer_crc、结束魔数、container_size 与实际长度比对 |

CRC32C 对 32 位以内的突发错误（含任意单字节改动）检出率 100%；多处随机损坏的漏检率约 2^-32，
深度校验的 SHA-256 再兜底。

## 10. 写入

1. 写文件头（原文件长度与 SHA-256 在写入前已知）。
2. 按任意顺序逐段追加段记录（建议 RECIPE 在最前，便于人工查看）。写入方在内存中记录段表项。
3. 写段表、文件尾。
4. 原子提交（写临时文件 → 落盘 → 自检 → 重命名）由上层负责（阶段 1 T4），不属于本格式。

没有写完文件尾的容器在 9.1 第 2 步即被识别为不完整。
同样的输入与段顺序，写入结果逐字节相同（格式中没有时间戳等易变字段；时间等信息放在 META 段）。

## 11. 还原

1. 建立长度为 orig_size 的输出。
2. 每个 SKELETON 段：解码 data，按区间表依次填入输出。
3. 像素与附加字节：按配方 `format` 与 `layout`，把 PLANE（解码后的像素）与 EXTRA 段按版面写回输出（阶段 1 T3 的格式处理器）。
4. 输出的每个字节必须恰好被填一次——SKELETON 区间与版面覆盖的区间合起来正好是 `[0, orig_size)`。
5. 计算输出的 SHA-256，与头部 orig_sha256 比对；不一致即还原失败，不得交付输出。

`format = "bytes"` 的容器（兜底：整文件 zstd 或原样）只有 SKELETON 段，区间覆盖整个原文件。

## 12. 版本兼容规则

| 情况 | 读取方行为 |
|---|---|
| 魔数不对 | 不是 IIS 容器 |
| version_major = 0 或大于读取方支持的主版本 | 拒绝 |
| version_minor 大于读取方认识的次版本 | 照常读取；忽略加长的头部 / 段表项中不认识的部分 |
| incompat_flags 有不认识的位 | 拒绝 |
| compat_flags 有不认识的位 | 忽略 |
| 不认识的段类型，无 CRITICAL | 跳过（仍校验 CRC） |
| 不认识的段类型，有 CRITICAL；或不认识的 codec | 可以列出与校验，拒绝还原 |
| 配方 `iis_recipe` 大于读取方支持的配方版本 | 拒绝还原 |

演进约定：只增加字段 / 段类型 / 特性位 → 次版本加一；改变已有字段含义或布局 → 主版本加一。
写入方总是写自己支持的最低能表达内容的版本（v1.0 写入方只写 1.0）。

## 13. 配方 JSON

RECIPE 段的 data（解码后）是一个 UTF-8 JSON 对象，JSON Schema 见 `spec/recipe-v1.schema.json`（Draft 2020-12）。

### 13.1 外层

| 字段 | 类型 | 必需 | 说明 |
|---|---|---|---|
| `iis_recipe` | 整数 | 是 | 配方版本，v1 为 1 |
| `format` | 字符串 | 是 | `bytes` / `bmp` / `tiff` / `raw` |
| `writer` | 对象 | 是 | `name`、`version`、`libs`（编码库名 → 版本），用于追溯与复现式还原 |
| `planes` | 数组 | 是 | 像素平面描述（13.2），无平面时为空数组 |
| `extras` | 数组 | 是 | 附加字节描述（13.3） |
| `layout` | 对象 | 是 | 版面参数，按 `format` 定义（13.4） |
| `predictor` | null | 是 | 预留给领域预测层，v1 必须为 null |

读取方必须忽略不认识的字段名（以便次版本扩展）；`layout` 内同样。

写入方应输出**紧凑、键名按字典序**的 JSON（无空白、非 ASCII 字符原样 UTF-8，不转义）——参考实现两边按此输出，
同样的内容得到逐字节相同的容器，便于对照测试。读取方不得依赖这种形式。

### 13.2 `planes[]`

| 字段 | 说明 |
|---|---|
| `id` | 对应 PLANE 段的 id，唯一 |
| `image` | 所属图像序号：BMP / RAW 为 0；TIFF 为页序号 |
| `role` | `gray`（单通道）/ `rgb`（3 分量，R、G、B 顺序）/ `alpha` / `index`（调色板索引）/ `sample`（TIFF 非 1、3 分量时的逐分量平面） |
| `sample` | role=`sample` 时的分量号 |
| `bayer_phase` | null，或 `[行相位, 列相位]`（各 0/1）：该平面是单通道图按 2×2 相位拆出的四分之一 |
| `width`、`height`、`components`、`bits` | 平面几何与码流位深（2~16），必须与码流头一致；`bits` ≤ 8 时解码样本为 1 字节，否则 2 字节 |
| `shift` | 0~15：原样本 = 平面样本左移 shift 位（MSB 对齐的 12 位图等） |
| `codec` | `jpegls` / `zstd` / `none`，必须与段的 codec 一致 |

单通道图像（BMP 8 位、RAW、TIFF 单分量页）由同一 `image` 下 role 为 `gray` / `index` 的平面组成：
一个 `bayer_phase` 为 null 的平面即整幅图；或四个相位平面 `[dy, dx]`，原图 `(y, x)` 处的样本来自相位
`[y mod 2, x mod 2]` 平面的 `(⌊y/2⌋, ⌊x/2⌋)`（奇数宽高时各相位平面尺寸不同）。之后左移 `shift` 位。

### 13.3 `extras[]`

| 字段 | 说明 |
|---|---|
| `id` | 对应 EXTRA 段的 id |
| `image` | 所属图像序号 |
| `role` | `row_padding`（非零行填充：按文件行序，每行 stride − 行像素字节）/ `packed_tail`（打包格式像素区的最后 1 字节，含未用位）/ `block_tail`（TIFF 未压缩像素块中超出几何推算的多余字节）/ `low_bits`（预留） |
| `block` | role=`block_tail` 时的像素块序号（与 `offsets[]` 下标相同） |

### 13.4 `layout` 与重组规则

`bytes`：空对象。没有像素平面，原样字节段覆盖整个原文件。

`bmp`：

| 字段 | 说明 |
|---|---|
| `width`、`height` | 像素尺寸（高为正数，方向由 `top_down` 表示） |
| `channels` | 1（8 位调色板）/ 3（24 位）/ 4（32 位） |
| `top_down` | 行序：true 为自上而下 |
| `stride` | 文件中每行字节数（含行填充） |
| `pixel_offset` | 像素区起点；像素区长度 = stride × height |
| `padding_zero` | 行填充是否全为 0；false 时由 `row_padding` 附加段保存原值 |
| `alpha_const` | 32 位图 Alpha 恒为同一值时记录该值（此时没有 alpha 平面），否则 null |

重组：平面按自上而下排列；8 位图取单通道图像（13.2）；24 / 32 位取 `rgb` 平面并按 B、G、R 写回，32 位第 4 字节取
`alpha` 平面或 `alpha_const`。`top_down` 为 false 时第 y 行写到文件第 height−1−y 行。每行后接行填充。

`raw`：

| 字段 | 说明 |
|---|---|
| `pixel_format` | GenICam PFNC 名称：`Mono8/10/12/14/16`、`Mono10p`、`Mono12p`、`Mono12Packed`、`Bayer{RG,GB,GR,BG}{8,10,12,14,16,10p,12p,12Packed}` |
| `width`、`height` | |
| `stride` | 每行字节数；null 为紧密排列（打包格式恒为 null） |
| `body_offset`、`body_len` | 像素区在文件中的位置；之外的字节在原样字节段 |
| `padding_zero` | 行尾填充是否全为 0（打包格式恒为 true） |

重组：取单通道图像。非打包格式每样本 1 字节（8 位）或 2 字节小端，按 stride 排行并接行填充；
打包格式按 GenICam 规则打包（Mono10p / Mono12p 为 LSB 优先连续位流；Mono12Packed 每 2 像素 3 字节：
`b0 = p0>>4，b1 = (p0 & 0xF) | (p1 & 0xF)<<4，b2 = p1>>4`），最后 1 字节用 `packed_tail` 替换。

`tiff`：

| 字段 | 说明 |
|---|---|
| `byteorder` | `II` / `MM` |
| `bigtiff` | 是否 BigTIFF |
| `pages[]` | 每页：`index`、`width`、`height`、`samples`、`bits`（8/16）、`planar`（1/2）、`tiled`、`tile`（[高, 宽] 或 null）、`rowsperstrip`（null 表示整页一条带）、`predictor`（1/2）、`compression`（null / `lzw` / `zip` / `packbits`）、`offsets[]`、`bytecounts[]`、`encoder`、`encoders` |

像素块几何（各实现必须相同）：条带时每条带 `min(rps, height − i·rps)` 行（rps = min(rowsperstrip, height)），
整宽；图块时按图块网格逐行排列，每块满 tile 尺寸。planar=2 时以上序列按分量重复。
画布 = 条带时 height × width，图块时（行块数 × 图块高）×（列块数 × 图块宽），含边缘图块填充区。
平面描述的是画布：单分量为单通道图像，3 分量为 `rgb` 平面，其他分量数为逐分量 `sample` 平面。

重组：对每个像素块从画布取出样本（按块内分量交织），`predictor`=2 时逐行做水平差分（按 8 / 16 位回绕），
按 `byteorder` 写成字节；未压缩时接上该块的 `block_tail`；已压缩时用 `encoder` 重新编码，结果长度必须等于 `bytecounts`。

`encoder` 对象（复现式还原，未压缩时为 null）：

| name | 参数 | 实现 |
|---|---|---|
| `libtiff-lzw` | — | libtiff tif_lzw.c 的编码行为（码表满 / 压缩率下降时 CLEAR、9~12 位码宽） |
| `imcd-lzw` | — | imagecodecs imcd.c 的 LZW 编码（tifffile 写出的 LZW） |
| `lzw` | 配合 `encoders` | 逐块混用上面两种：`encoders` 为每块一个字符，`I` = imcd-lzw，`L` = libtiff-lzw |
| `zlib` | `level` | 经典 zlib `compress2`（zlib 格式，默认窗口与内存级别） |
| `libdeflate` | `level` | libdeflate zlib 格式压缩 |
| `packbits-rows` | — | 逐行 PackBits（imagecodecs imcd.c 算法），游程不跨行 |
| `packbits` | — | 整块 PackBits |

复现式编码器必须与记录的实现逐字节相同；配方 `writer.libs` 记录写入时的库版本，供追溯。

### 13.5 示例（8 位灰度 BMP，自下而上，Bayer 拆分）

```json
{
  "iis_recipe": 1, "format": "bmp",
  "writer": {"name": "libiis", "version": "0.1.0", "libs": {"charls": "2.4.5", "zstd": "1.5.7"}},
  "planes": [
    {"id": 0, "image": 0, "role": "gray", "bayer_phase": [0, 0], "width": 640, "height": 512, "components": 1, "bits": 8, "shift": 0, "codec": "jpegls"},
    {"id": 1, "image": 0, "role": "gray", "bayer_phase": [0, 1], "width": 640, "height": 512, "components": 1, "bits": 8, "shift": 0, "codec": "jpegls"},
    {"id": 2, "image": 0, "role": "gray", "bayer_phase": [1, 0], "width": 640, "height": 512, "components": 1, "bits": 8, "shift": 0, "codec": "jpegls"},
    {"id": 3, "image": 0, "role": "gray", "bayer_phase": [1, 1], "width": 640, "height": 512, "components": 1, "bits": 8, "shift": 0, "codec": "jpegls"}
  ],
  "extras": [],
  "layout": {"width": 1280, "height": 1024, "channels": 1, "top_down": false, "stride": 1280,
             "pixel_offset": 1078, "padding_zero": true, "alpha_const": null},
  "predictor": null
}
```

### 13.6 META 段

可选的描述性 JSON，不参与还原、不影响校验结论，供索引重建与人工查看。约定字段（均可选）：
`source_name`（原文件名）、`source_path`（相对监控根目录的路径）、`mtime`（原文件修改时间，ISO 8601）、
`station`（工位）、`verdict`（`OK` / `NG` / null）、`created`（压缩时间，ISO 8601）。读取方忽略不认识的字段。

## 14. 预留：领域预测层

阶段 4 可能引入“以参考图预测当前图、只存残差”的领域预测层（天花板测试 G3 方向）。v1 为它预留：

- 不兼容特性位 0（PREDICTION）：含预测层段的容器必须置位，使 v1 读取方明确拒绝而不是还原出错；
- 段类型 16（预测参考：引用的参考图标识与哈希）、17（残差码流）；
- 配方 `predictor` 字段（v1 恒为 null）。

具体定义在启用时以新的次版本给出。

## 15. 限制值

| 项目 | 上限 | 超出时 |
|---|---|---|
| 段数 | 65 536 | 视为损坏 |
| 段表项长度 entry_size | 4 096 | 视为损坏 |
| 文件头长度 header_size | 4 096 | 视为损坏 |
| meta_len | 16 MiB | 视为损坏 |
| RECIPE / META 解码后长度 | 16 MiB | 视为损坏 |
| 单段 raw_len | 8 GiB | 视为损坏（防止损坏的长度字段引发超大分配） |

## 16. 示例：整文件 zstd 兜底容器

原文件 N 字节，zstd 压缩后 Z 字节，配方 `{"iis_recipe":1,"format":"bytes",…}` 长 R 字节（不压缩）：

```
0       文件头 72 B
72      段头 RECIPE   type=1 codec=0 flags=1 id=0 meta_len=0 raw_len=R data_len=R
112     配方 JSON R B
112+R   段头 SKELETON type=2 codec=1 flags=1 id=0 meta_len=m raw_len=N data_len=Z
152+R   区间表 m B：01 00 <N 的 LEB128>
…       zstd 帧 Z B
…       段表 16 + 2×40 + 4 = 100 B
…       文件尾 32 B
```

总开销 = 72 + 2×40 + m + R + 100 + 32 字节（除配方与压缩数据外约 290 B）。

## 17. 与 v0 草案（`docs/05` 第 7 节）的差异

| v0 | v1 | 原因 |
|---|---|---|
| 配方放在文件头 | 配方是 RECIPE 段（可 zstd） | 头部定长、可 CRC；配方大时可压缩 |
| 段数在头部 | 段表 + 文件尾放在最后 | 流式写入不必预知段数；随机读取 |
| 每段 orig_offset / orig_length | 只有 SKELETON 段带区间表；像素段位置由配方版面决定 | 像素平面经行序翻转、去填充、Bayer 拆分后不是原文件中的一个连续区间 |
| 段类型 0~3（原样 / zstd / 图像 / 复现式） | 段类型与编码分开；复现式还原不再是单独的段类型 | 复现式还原的数据就是像素平面 + 配方中的编码器参数 |
| 只有段数据 CRC | 头部、段头、段表、文件尾都有 CRC | 任意字节损坏都能发现 |
| 无版本兼容规则 | 主 / 次版本、特性位、CRITICAL 标志 | 老读取方面对新文件有确定行为 |
| `lib_versions` 在配方顶层 | `writer.libs` | 统一记录写入方信息 |

## 修订记录

| 版本 | 日期 | 说明 |
|---|---|---|
| 1.0 | 2026-11-01 | 首版。13.4 各格式 `layout` 依据阶段 0 参考实现定义，阶段 1 T3 移植格式处理器时如需调整，在首个试点版本发布前直接修订本节并记录 |
| 1.0 修订 | 2026-11-02 | T3 移植后定稿第 13 节（试点前，未改版本号）：增加 `sample` 平面、`block_tail` 附加字节；TIFF `compression` 用 `zip`；`tile` 为 [高, 宽]；`encoder` 名称与参数定表、逐块 LZW 用 `encoders` 字符串；RAW 像素格式补全 Mono14 与 Bayer；写明各格式重组规则 |
