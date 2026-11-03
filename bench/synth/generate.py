"""T1 合成测试数据生成器。

一条命令生成全部测试集：
    python -m bench synth --out bench/out/synth [--size medium] [--seed 2026]

输出目录结构：
    README.md            总说明
    manifest.json        每个文件的“真值”（T2 识别、T3 重组的验收依据）
    raw_config.yaml      RAW 参数配置示例（按目录 / 文件名规则）
    bmp/ tiff/ raw/ bayer/ sequence/ other/   各类变体，每个目录带 README.md

每个文件写完立即用独立的库（OpenCV / tifffile / 解包函数）读回，与生成时的像素比对，
保证“真值”可信。
"""
from __future__ import annotations

import hashlib
import io
import json
import struct
import zlib
from dataclasses import dataclass, field
from pathlib import Path

import cv2
import numpy as np
import tifffile
import yaml

from ..layout import packing
from . import scene as sc
from .bmpwrite import encode_bmp, row_stride

PRESETS = {
    # 名称: (单图宽, 单图高, 序列宽, 序列高)
    "small": (256, 192, 192, 160),
    "medium": (1280, 1024, 1024, 768),
    "large": (2448, 2048, 2448, 2048),
}

CATEGORY_DOC = {
    "bmp": "BMP 变体：8 位灰度（带调色板）/ 非灰度调色板 / 24 位 / 32 位；自下而上与自上而下；"
           "宽度非 4 的倍数（有行填充，含非 0 填充）；40 与 124 字节 DIB 头；文件尾附加数据；OpenCV 实写样本。",
    "tiff": "TIFF 变体：未压缩 8/16 位、16 位容器内 12 位有效（低位 / 高位对齐）、II 与 MM 字节序、"
            "多页、条带与图块（含不满整块的边缘图块）、BigTIFF、RGB 交错与分平面、厂商私有标签；"
            "已压缩：OpenCV(libtiff) 默认 LZW+水平预测、OpenCV ZIP、tifffile LZW / ZIP / PackBits。",
    "raw": "无文件头 RAW：Mono8（含行跨距填充）、Mono16 全 16 位、16 位容器内 12 位（低位 / 高位对齐）、"
           "Mono10p、Mono12p（含奇数像素总数）、Mono12Packed。参数见上级目录 raw_config.yaml；"
           "一个文件用同名 .json 参数文件；一个文件故意不给配置（检验按文件大小推候选）。",
    "bayer": "Bayer 马赛克：RGGB / BGGR / GRBG / GBRG 四种排列，分别以 RAW、灰度 BMP、灰度 TIFF 保存，"
             "含奇数宽高；RAW 中只有 RGGB 在配置里明确写 BayerRG8，其余伪装成 Mono 格式，需要自动检测。",
    "sequence": "时序序列（天花板测试用）：同一型号产品连续 200 件（每件细纹理与序列号不同），"
                "带轻微亚像素位移、光照闪烁与独立噪声；同一台“相机”的固定模式噪声（PRNU、列偏置）；"
                "其中少量为 NG（划痕 + 污点）；另有 8 张空载背景。8 位灰度 BMP。",
    "other": "非处理对象与边界情况：JPEG、PNG（应识别为不处理）、文本、随机字节（未知）、"
             "扩展名与内容不符的 BMP/TIFF（检验按内容识别）、空文件、截断的 BMP。",
}


def eff_bits(arr: np.ndarray, shift: int = 0) -> int:
    return int(int(arr.max()) >> shift).bit_length() if arr.size else 0


def expect(**kw) -> dict:
    base = dict(
        cls=None, container=None, width=None, height=None, channels=1, bits_stored=None,
        bits_effective=None, shift=0, byteorder=None, row_order=None, row_padding=0,
        pages=1, layout=None, compression=None, palette=None, bayer=None, pixel_format=None,
        writer="unknown", trailer_bytes=0, raw_config=None, damaged=False,
    )
    unknown = set(kw) - set(base)
    if unknown:
        raise KeyError(unknown)
    base.update(kw)
    base["class"] = base.pop("cls")
    return base


@dataclass
class Ctx:
    root: Path
    size: str
    seed: int
    seq_frames: int
    entries: list = field(default_factory=list)
    raw_rules: list = field(default_factory=list)

    @property
    def wh(self):
        return PRESETS[self.size][:2]

    @property
    def seq_wh(self):
        return PRESETS[self.size][2:]

    def rng(self, name: str) -> np.random.Generator:
        return np.random.default_rng([self.seed, zlib.crc32(name.encode())])

    def put(self, rel: str, data: bytes, exp: dict | None, notes: str, readback: str = "n/a"):
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
        self.entries.append({
            "path": rel, "category": rel.split("/")[0], "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest(), "expect": exp, "notes": notes,
            "readback": readback,
        })


def _same(a: np.ndarray, b: np.ndarray, what: str):
    if a is None or a.shape != b.shape or not np.array_equal(a, b):
        raise AssertionError(f"读回校验失败：{what} {None if a is None else a.shape} vs {b.shape}")


def cv_decode(data: bytes) -> np.ndarray:
    return cv2.imdecode(np.frombuffer(data, np.uint8), cv2.IMREAD_UNCHANGED)


def cv_encode(ext: str, img: np.ndarray, params=()) -> bytes:
    ok, buf = cv2.imencode(ext, img, list(params))
    if not ok:
        raise RuntimeError(f"OpenCV 编码 {ext} 失败")
    return buf.tobytes()


def tiff_bytes(arr, **kw) -> bytes:
    kw.setdefault("metadata", None)
    kw.setdefault("software", False)
    b = io.BytesIO()
    tifffile.imwrite(b, arr, **kw)
    return b.getvalue()


def tiff_read(data: bytes) -> tuple[np.ndarray, tifffile.TiffFile]:
    t = tifffile.TiffFile(io.BytesIO(data))
    n = len(t.pages)
    arr = t.asarray(key=range(n)) if n > 1 else t.asarray()
    return arr, t


# ---------------------------------------------------------------- 图像内容

def gray_img(ctx, name, w, h, bits=8, defect=False):
    rng = ctx.rng(name)
    return sc.Sensor(bits).expose(sc.to_gray(sc.scene(h, w, rng, defect=defect)), rng)


def color_img(ctx, name, w, h):
    """彩色相机去马赛克后的 RGB（带轻微通道相关噪声），返回 BGR uint8。"""
    rng = ctx.rng(name)
    rgb = sc.Sensor(8).expose(sc.scene(h, w, rng), rng)
    rgb = cv2.GaussianBlur(rgb, (3, 3), 0.6)
    return np.ascontiguousarray(rgb[..., ::-1])


def bayer_img(ctx, name, w, h, pattern, bits):
    rng = ctx.rng(name)
    return sc.Sensor(bits).expose(sc.mosaic(sc.scene(h, w, rng), pattern), rng)


# ---------------------------------------------------------------- BMP

def put_bmp(ctx, rel, img, exp, notes, *, readback_palette=None, **kw):
    data = encode_bmp(img, **kw)
    if exp.get("damaged"):
        ctx.put(rel, data, exp, notes)
        return data
    got = cv_decode(data)
    want = img
    if readback_palette is not None:            # 非灰度调色板：OpenCV 读出的是 BGR
        want = readback_palette[img][..., :3]
    if img.ndim == 3 and img.shape[2] == 4 and got is not None and got.ndim == 3 and got.shape[2] == 3:
        want = img[..., :3]                      # 部分 OpenCV 版本读 32 位 BMP 丢 Alpha
    _same(got, want, rel)
    ctx.put(rel, data, exp, notes, "ok")
    return data


def gen_bmp(ctx: Ctx):
    W, H = ctx.wh

    def e8(w, h, img, **kw):
        stride = row_stride(w, 8)
        return expect(cls="bmp", container="bmp", width=w, height=h, bits_stored=8,
                      bits_effective=eff_bits(img), row_order=kw.pop("row_order", "bottom_up"),
                      row_padding=stride - w, palette=kw.pop("palette", "gray"), **kw)

    img = gray_img(ctx, "bmp8a", W, H)
    put_bmp(ctx, "bmp/bmp8_gray_bu.bmp", img, e8(W, H, img), "8 位灰度，自下而上，宽为 4 的倍数（无填充）")

    w = W - 1
    img = gray_img(ctx, "bmp8b", w, H)
    put_bmp(ctx, "bmp/bmp8_gray_bu_pad1.bmp", img, e8(w, H, img), "8 位灰度，自下而上，每行 1 字节 0 填充")

    w = W - 3
    img = gray_img(ctx, "bmp8c", w, H)
    put_bmp(ctx, "bmp/bmp8_gray_td_pad3.bmp", img, e8(w, H, img, row_order="top_down"),
            "8 位灰度，自上而下（高度为负），每行 3 字节 0 填充", top_down=True)

    w = W - 2
    img = gray_img(ctx, "bmp8d", w, H)
    junk = ctx.rng("junk").integers(1, 256, 251, dtype=np.uint8).tobytes()
    put_bmp(ctx, "bmp/bmp8_gray_bu_padjunk.bmp", img, e8(w, H, img),
            "8 位灰度，每行 2 字节填充且填充为非 0 垃圾字节（必须原样保留）", pad_bytes=junk)

    img = gray_img(ctx, "bmp8e", W, H)
    lut = cv2.applyColorMap(np.arange(256, dtype=np.uint8).reshape(-1, 1), cv2.COLORMAP_JET).reshape(256, 3)
    pal = np.concatenate([lut, np.zeros((256, 1), np.uint8)], axis=1)
    put_bmp(ctx, "bmp/bmp8_palette_color.bmp", img, e8(W, H, img, palette="color"),
            "8 位索引图，非灰度（伪彩）调色板：应按索引编码", palette=pal, readback_palette=pal)

    img = gray_img(ctx, "bmp8f", W, H)
    trailer = b"VNDRMETA" + struct.pack("<IIq", 1, 42, 1730419200) + ctx.rng("trailer").bytes(44)
    put_bmp(ctx, "bmp/bmp8_gray_trailer.bmp", img, e8(W, H, img, trailer_bytes=len(trailer)),
            f"8 位灰度，文件尾附加 {len(trailer)} 字节厂商数据", trailer=trailer)

    w = W - 2
    img = color_img(ctx, "bmp24a", w, H)
    put_bmp(ctx, "bmp/bmp24_bu_pad.bmp", img,
            expect(cls="bmp", container="bmp", width=w, height=H, channels=3, bits_stored=8,
                   bits_effective=eff_bits(img), row_order="bottom_up", row_padding=row_stride(w, 24) - 3 * w),
            "24 位 BGR，自下而上，有行填充")

    img = color_img(ctx, "bmp24b", W, H)
    put_bmp(ctx, "bmp/bmp24_td.bmp", img,
            expect(cls="bmp", container="bmp", width=W, height=H, channels=3, bits_stored=8,
                   bits_effective=eff_bits(img), row_order="top_down"),
            "24 位 BGR，自上而下", top_down=True)

    bgr = color_img(ctx, "bmp32a", W, H)
    img = np.concatenate([bgr, np.full((H, W, 1), 255, np.uint8)], axis=2)
    put_bmp(ctx, "bmp/bmp32_bu.bmp", img,
            expect(cls="bmp", container="bmp", width=W, height=H, channels=4, bits_stored=8,
                   bits_effective=8, row_order="bottom_up"),
            "32 位 BGRA，BI_RGB，Alpha 恒为 255")

    bgr = color_img(ctx, "bmp32b", W, H)
    img = np.concatenate([bgr, np.full((H, W, 1), 255, np.uint8)], axis=2)
    put_bmp(ctx, "bmp/bmp32_v5_bitfields.bmp", img,
            expect(cls="bmp", container="bmp", width=W, height=H, channels=4, bits_stored=8,
                   bits_effective=8, row_order="bottom_up"),
            "32 位 BGRA，BITMAPV5HEADER（124 字节）+ BI_BITFIELDS", header=124)

    w = W - 1
    img = gray_img(ctx, "bmp8cv", w, H)
    data = cv_encode(".bmp", img)
    _same(cv_decode(data), img, "bmp8_opencv")
    ctx.put("bmp/bmp8_opencv.bmp", data, e8(w, H, img, writer="opencv"),
            "OpenCV cv2.imwrite 实写的 8 位灰度 BMP（真实写图程序指纹）", "ok")

    w = W - 2
    img = color_img(ctx, "bmp24cv", w, H)
    data = cv_encode(".bmp", img)
    _same(cv_decode(data), img, "bmp24_opencv")
    ctx.put("bmp/bmp24_opencv.bmp", data,
            expect(cls="bmp", container="bmp", width=w, height=H, channels=3, bits_stored=8,
                   bits_effective=eff_bits(img), row_order="bottom_up",
                   row_padding=row_stride(w, 24) - 3 * w, writer="opencv"),
            "OpenCV cv2.imwrite 实写的 24 位 BMP", "ok")


# ---------------------------------------------------------------- TIFF

def put_tiff(ctx, rel, arr, exp, notes, data=None, **kw):
    data = tiff_bytes(arr, **kw) if data is None else data
    got, t = tiff_read(data)
    _same(got, arr, rel)
    exp["pages"] = len(t.pages)
    ctx.put(rel, data, exp, notes, "ok")


def gen_tiff(ctx: Ctx):
    W, H = ctx.wh

    def et(w, h, img, bits=8, **kw):
        kw.setdefault("byteorder", "II")
        kw.setdefault("layout", "strips")
        return expect(cls=kw.pop("cls", "tiff_uncompressed"), container="tiff", width=w, height=h,
                      bits_stored=bits, bits_effective=eff_bits(img, kw.get("shift", 0)),
                      row_order="top_down", **kw)

    img = gray_img(ctx, "tif8a", W, H)
    put_tiff(ctx, "tiff/tif8_II_strips.tif", img, et(W, H, img), "8 位，II，条带（每条 64 行）",
             byteorder="<", rowsperstrip=64)

    w, h = W - 37, H - 21
    img = gray_img(ctx, "tif8b", w, h)
    put_tiff(ctx, "tiff/tif8_II_tiles.tif", img, et(w, h, img, layout="tiles"),
             "8 位，II，256×256 图块；宽高非图块整数倍（边缘图块含填充区）", byteorder="<", tile=(256, 256))

    img = gray_img(ctx, "tif16a", W, H, bits=16)
    put_tiff(ctx, "tiff/tif16_II.tif", img, et(W, H, img, 16), "16 位全量程，II", byteorder="<")

    img = gray_img(ctx, "tif16b", W, H, bits=12)
    put_tiff(ctx, "tiff/tif16_12bit_lsb_II.tif", img, et(W, H, img, 16),
             "16 位容器，12 位有效，低位对齐（值 0~4095），II", byteorder="<")

    img = (gray_img(ctx, "tif16c", W, H, bits=12) << 4).astype(np.uint16)
    put_tiff(ctx, "tiff/tif16_12bit_msb_MM.tif", img, et(W, H, img, 16, shift=4, byteorder="MM"),
             "16 位容器，12 位有效，高位对齐（低 4 位为 0），MM 大端", byteorder=">")

    w, h = W - 5, H - 3
    img = gray_img(ctx, "tif16d", w, h, bits=16)
    put_tiff(ctx, "tiff/tif16_MM_tiles.tif", img, et(w, h, img, 16, byteorder="MM", layout="tiles"),
             "16 位，MM，128×128 图块", byteorder=">", tile=(128, 128))

    pages = np.stack([gray_img(ctx, f"tifmp8_{i}", W, H) for i in range(4)])
    put_tiff(ctx, "tiff/tif8_multipage_II.tif", pages, et(W, H, pages),
             "8 位，4 页，II", byteorder="<", photometric="minisblack")

    pages = np.stack([gray_img(ctx, f"tifmp16_{i}", W, H, bits=12) for i in range(3)])
    put_tiff(ctx, "tiff/tif16_multipage_12bit_MM.tif", pages, et(W, H, pages, 16, byteorder="MM"),
             "16 位容器 12 位有效，3 页，MM", byteorder=">", photometric="minisblack")

    img = gray_img(ctx, "tifbig", W, H)
    put_tiff(ctx, "tiff/tif8_bigtiff.tif", img, et(W, H, img, layout="strips"),
             "BigTIFF（64 位偏移），8 位", byteorder="<", bigtiff=True, rowsperstrip=128)

    rgb = color_img(ctx, "tifrgb", W, H)[..., ::-1].copy()
    put_tiff(ctx, "tiff/tif8_rgb_contig.tif", rgb, et(W, H, rgb, channels=3),
             "RGB 8 位，交错（PlanarConfiguration=1）", byteorder="<", photometric="rgb")

    rgbp = np.moveaxis(color_img(ctx, "tifrgbp", W, H)[..., ::-1], 2, 0).copy()
    put_tiff(ctx, "tiff/tif8_rgb_planar.tif", rgbp, et(W, H, rgbp, channels=3),
             "RGB 8 位，分平面（PlanarConfiguration=2）", byteorder="<", photometric="rgb",
             planarconfig="separate")

    img = gray_img(ctx, "tifhal", W, H)
    put_tiff(ctx, "tiff/tif8_halcon_sim.tif", img, et(W, H, img, writer="halcon"),
             "【模拟指纹】Software=“MVTec HALCON”，带厂商私有标签 65000（ASCII）；指纹为假设，需真实样本校准",
             byteorder="<", software="MVTec HALCON",
             extratags=[(65000, "s", 0, "station=ST03;lot=26A-1107;result=OK", True)])

    img = gray_img(ctx, "tifvp", W, H)
    put_tiff(ctx, "tiff/tif8_visionpro_sim.tif", img, et(W, H, img, writer="visionpro"),
             "【模拟指纹】Software=“Cognex VisionPro”；指纹为假设，需真实样本校准",
             byteorder="<", software="Cognex VisionPro")

    img = gray_img(ctx, "tifsdk", W, H, bits=12)
    put_tiff(ctx, "tiff/tif16_camsdk_sim.tif", img, et(W, H, img, 16, writer="camera_sdk"),
             "【模拟指纹】Software=“Basler pylon”，12 位；指纹为假设，需真实样本校准",
             byteorder="<", software="Basler pylon")

    img = gray_img(ctx, "tifcvnone", W - 1, H)
    data = cv_encode(".tif", img, [cv2.IMWRITE_TIFF_COMPRESSION, 1])
    put_tiff(ctx, "tiff/tif8_opencv_none.tif", img, et(W - 1, H, img, writer="opencv"),
             "OpenCV(libtiff) 写的未压缩 TIFF", data=data)

    img = gray_img(ctx, "tifcvlzw8", W, H)
    data = cv_encode(".tif", img)
    put_tiff(ctx, "tiff/tif8_opencv_lzw.tif", img,
             et(W, H, img, cls="tiff_compressed", compression="lzw", writer="opencv"),
             "OpenCV cv2.imwrite 默认参数：LZW + 水平预测（现场最常见的“TIFF”）", data=data)

    img = gray_img(ctx, "tifcvlzw16", W, H, bits=12)
    data = cv_encode(".tif", img)
    put_tiff(ctx, "tiff/tif16_opencv_lzw.tif", img,
             et(W, H, img, 16, cls="tiff_compressed", compression="lzw", writer="opencv"),
             "OpenCV 默认参数写 16 位（12 位有效）：LZW + 水平预测", data=data)

    img = gray_img(ctx, "tifcvzip", W, H)
    data = cv_encode(".tif", img, [cv2.IMWRITE_TIFF_COMPRESSION, 8])
    put_tiff(ctx, "tiff/tif8_opencv_zip.tif", img,
             et(W, H, img, cls="tiff_compressed", compression="zip", writer="opencv"),
             "OpenCV(libtiff) Adobe Deflate（ZIP）", data=data)

    img = gray_img(ctx, "tiftflzw", W, H)
    put_tiff(ctx, "tiff/tif8_tifffile_lzw_pred.tif", img,
             et(W, H, img, cls="tiff_compressed", compression="lzw", writer="tifffile"),
             "tifffile 默认参数（Software=tifffile.py）LZW + 水平预测", byteorder="<", compression="lzw", software=None,
             predictor=True, rowsperstrip=64)

    img = gray_img(ctx, "tiftfzip", W, H, bits=16)
    put_tiff(ctx, "tiff/tif16_tifffile_zip.tif", img,
             et(W, H, img, 16, cls="tiff_compressed", compression="zip", writer="tifffile"),
             "tifffile 默认参数（Software=tifffile.py）Adobe Deflate，16 位，无预测", byteorder="<", compression="zlib",
             software=None,
             compressionargs={"level": 6}, rowsperstrip=64)

    img = gray_img(ctx, "tifpb", W, H)
    put_tiff(ctx, "tiff/tif8_tifffile_packbits.tif", img,
             et(W, H, img, cls="tiff_compressed", compression="packbits", writer="tifffile"),
             "tifffile 默认参数（Software=tifffile.py）PackBits", byteorder="<", compression="packbits",
             software=None, rowsperstrip=64)


# ---------------------------------------------------------------- RAW

def raw_bytes(img: np.ndarray, fmt: str, stride: int | None = None) -> bytes:
    h, w = img.shape
    if fmt in packing.PACKED_FORMATS:
        return packing.pack(fmt, img)
    rows = img.astype("<u2").view(np.uint8).reshape(h, -1) if img.dtype == np.uint16 else img.reshape(h, w)
    if stride and stride > rows.shape[1]:
        buf = np.zeros((h, stride), np.uint8)
        buf[:, : rows.shape[1]] = rows
        rows = buf
    return rows.tobytes()


def raw_readback(data: bytes, fmt: str, w: int, h: int, stride: int | None = None) -> np.ndarray:
    if fmt in packing.PACKED_FORMATS:
        return packing.unpack(fmt, data, w * h).reshape(h, w)
    bpp = 1 if fmt.endswith("8") else 2
    stride = stride or w * bpp
    rows = np.frombuffer(data, np.uint8).reshape(h, stride)[:, : w * bpp]
    return rows.copy() if bpp == 1 else rows.copy().view("<u2").reshape(h, w)


def put_raw(ctx, rel, img, fmt, exp, notes, *, config="rule", stride=None, cfg_fmt=None):
    h, w = img.shape
    data = raw_bytes(img, fmt, stride)
    _same(raw_readback(data, fmt, w, h, stride), img, rel)
    exp.update(raw_config=config)
    rule = {"width": w, "height": h, "pixel_format": cfg_fmt or fmt}
    if stride:
        rule["stride"] = stride
    if config == "rule":
        ctx.raw_rules.append({"match": rel, **rule})
    elif config == "sidecar":
        side = rel.rsplit(".", 1)[0] + ".json"
        ctx.put(side, json.dumps(rule, ensure_ascii=False, indent=2).encode(), None,
                f"{rel} 的同名参数文件（不是图像，扫描时应识别为参数文件 / 非图像）")
    ctx.put(rel, data, exp, notes, "ok")


def gen_raw(ctx: Ctx):
    W, H = ctx.wh

    def er(w, h, img, fmt, bits, **kw):
        return expect(cls="raw", container="raw", width=w, height=h, bits_stored=bits,
                      bits_effective=eff_bits(img, kw.get("shift", 0)), row_order="top_down",
                      pixel_format=fmt, byteorder=None if bits == 8 else "II", **kw)

    img = gray_img(ctx, "raw8", W, H)
    put_raw(ctx, "raw/mono8.raw", img, "Mono8", er(W, H, img, "Mono8", 8), "Mono8，无填充")

    w = W - 3
    stride = (w + 15) // 16 * 16
    img = gray_img(ctx, "raw8s", w, H)
    put_raw(ctx, "raw/mono8_stride.raw", img, "Mono8", er(w, H, img, "Mono8", 8, row_padding=stride - w),
            f"Mono8，宽 {w}，行跨距 {stride}（16 字节对齐，0 填充）", stride=stride)

    img = gray_img(ctx, "raw16", W, H, bits=16)
    put_raw(ctx, "raw/mono16_full.raw", img, "Mono16", er(W, H, img, "Mono16", 16), "Mono16 全 16 位，小端")

    img = gray_img(ctx, "raw12", W, H, bits=12)
    put_raw(ctx, "raw/mono12_lsb.raw", img, "Mono12", er(W, H, img, "Mono12", 16),
            "Mono12（PFNC：16 位容器低位对齐），小端")

    img = (gray_img(ctx, "raw12m", W, H, bits=12) << 4).astype(np.uint16)
    put_raw(ctx, "raw/mono16_12bit_msb.raw", img, "Mono16", er(W, H, img, "Mono16", 16, shift=4),
            "配置写 Mono16，实际 12 位高位对齐（低 4 位为 0），需工具检测移位")

    w = W - 2
    img = gray_img(ctx, "raw10p", w, H, bits=10)
    put_raw(ctx, "raw/mono10p.raw", img, "Mono10p", er(w, H, img, "Mono10p", 10), "Mono10p：4 像素 5 字节")

    w, h = W - 1, H - 1
    img = gray_img(ctx, "raw12p", w, h, bits=12)
    put_raw(ctx, "raw/mono12p_odd.raw", img, "Mono12p", er(w, h, img, "Mono12p", 12),
            f"Mono12p，{w}×{h} 像素总数为奇数，最后半字节补 0")

    img = gray_img(ctx, "raw12pk", W, H, bits=12)
    put_raw(ctx, "raw/mono12packed.raw", img, "Mono12Packed", er(W, H, img, "Mono12Packed", 12),
            "Mono12Packed（GigE Vision 旧格式）")

    img = gray_img(ctx, "raw12side", W, H, bits=12)
    put_raw(ctx, "raw/mono12p_sidecar.raw", img, "Mono12p", er(W, H, img, "Mono12p", 12),
            "Mono12p，参数来自同名 .json 参数文件", config="sidecar")

    img = gray_img(ctx, "raw8none", W, H)
    put_raw(ctx, "raw/mono8_noconfig.raw", img, "Mono8", er(W, H, img, "Mono8", 8),
            f"Mono8 {W}×{H}，故意不提供配置：候选列表中应包含真值", config="none")


# ---------------------------------------------------------------- Bayer

def gen_bayer(ctx: Ctx):
    W, H = ctx.wh
    plan = {
        # 排列: [(容器, 宽, 高, 有效位, 额外参数)]
        "RGGB": [("raw", W, H, 8, {"cfg": "BayerRG8"}), ("bmp", W - 1, H - 1, 8, {}), ("tif", W, H, 8, {})],
        "BGGR": [("raw", W - 1, H - 1, 12, {"cfg": "Mono12"}), ("bmp", W, H, 8, {}), ("tif", W - 1, H, 12, {})],
        "GRBG": [("raw", W - 1, H, 8, {"cfg": "Mono8"}), ("bmp", W, H - 1, 8, {}),
                 ("tif", W, H, 12, {"byteorder": ">"})],
        "GBRG": [("raw", W, H, 12, {"cfg": "Mono16", "msb": True}), ("bmp", W - 3, H, 8, {}),
                 ("tif", W - 1, H - 1, 8, {})],
    }
    for pat, items in plan.items():
        for cont, w, h, bits, extra in items:
            name = f"bayer_{pat}_{cont}{bits}"
            img = bayer_img(ctx, name, w, h, pat, bits)
            shift = 0
            if extra.get("msb"):
                img, shift = (img << 4).astype(np.uint16), 4
            stored = 8 if bits == 8 else 16
            common = dict(cls="bayer", width=w, height=h, bits_stored=stored, bayer=pat, shift=shift,
                          bits_effective=eff_bits(img, shift), row_order="top_down")
            odd = "，奇数宽高" if (w % 2 or h % 2) else ""
            if cont == "raw":
                cfg = extra["cfg"]
                fmt = "Mono8" if bits == 8 else "Mono16"
                disguise = "配置明确为 Bayer" if cfg.startswith("Bayer") else f"伪装：配置写 {cfg}"
                put_raw(ctx, f"bayer/{name}.raw", img, fmt,
                        expect(container="raw", pixel_format=cfg, byteorder=None if bits == 8 else "II", **common),
                        f"{pat} RAW {bits} 位{'（高位对齐）' if shift else ''}，{disguise}{odd}", cfg_fmt=cfg)
            elif cont == "bmp":
                put_bmp(ctx, f"bayer/{name}.bmp", img,
                        expect(container="bmp", palette="gray", row_padding=row_stride(w, 8) - w,
                               **{**common, "row_order": "bottom_up"}),
                        f"{pat} 伪装成 8 位灰度 BMP{odd}")
            else:
                bo = extra.get("byteorder", "<")
                put_tiff(ctx, f"bayer/{name}.tif", img,
                         expect(container="tiff", byteorder="MM" if bo == ">" else "II", layout="strips",
                                **common),
                         f"{pat} 伪装成灰度 TIFF，{stored} 位容器 {bits} 位有效{odd}", byteorder=bo)


# ---------------------------------------------------------------- 时序序列

def gen_sequence(ctx: Ctx):
    W, H = ctx.seq_wh
    rng = ctx.rng("sequence")
    design = sc.Design(H, W, int(rng.integers(1 << 31)))
    bg = sc.background(H, W, ctx.rng("seq_bg"))
    sensor = sc.Sensor.with_fpn(8, H, W, ctx.rng("seq_fpn"))
    n = ctx.seq_frames
    ng = set(rng.choice(n, size=max(1, n // 40), replace=False).tolist()) if n else set()

    def write(rel, gray, notes, **meta):
        img = sensor.expose(gray, rng)
        exp = expect(cls="bmp", container="bmp", width=W, height=H, bits_stored=8,
                     bits_effective=eff_bits(img), row_order="bottom_up", row_padding=row_stride(W, 8) - W,
                     palette="gray")
        put_bmp(ctx, rel, img, exp, notes)
        ctx.entries[-1]["sequence"] = meta

    for i in range(8):
        write(f"sequence/background_{i:02d}.bmp", sc.to_gray(bg), "空载背景（无产品），独立噪声", kind="background")
    dx = dy = 0.0
    for i in range(n):
        dx = float(np.clip(dx + rng.normal(0, 0.6), -4, 4))
        dy = float(np.clip(dy + rng.normal(0, 0.6), -4, 4))
        pm, a = sc.render_product(design, rng, f"{100000 + i}", defect=i in ng)
        rad = sc.compose(bg, pm, a, dx, dy) * np.float32(1 + 0.01 * rng.standard_normal())
        write(f"sequence/frame_{i:04d}.bmp", sc.to_gray(rad),
              f"第 {i} 件，位移 ({dx:+.2f}, {dy:+.2f}) 像素{'，NG' if i in ng else ''}",
              kind="frame", index=i, dx=round(dx, 3), dy=round(dy, 3), ng=i in ng)


# ---------------------------------------------------------------- 其他

def gen_other(ctx: Ctx):
    W, H = ctx.wh
    img = gray_img(ctx, "oth", W, H)
    color = color_img(ctx, "othc", W, H)
    ctx.put("other/photo.jpg", cv_encode(".jpg", color, [cv2.IMWRITE_JPEG_QUALITY, 92]),
            expect(cls="unsupported", container="jpeg", width=W, height=H, channels=3), "JPEG：不处理，原样存储")
    ctx.put("other/image.png", cv_encode(".png", img),
            expect(cls="unsupported", container="png", width=W, height=H), "PNG：不处理（P2 才考虑复现式还原）")
    log = "".join(f"2026-11-01 08:{i // 60:02d}:{i % 60:02d} ST03 OK cycle={i} t=41ms\n" for i in range(400))
    ctx.put("other/inspect.log", log.encode(), expect(cls="unknown", container=None), "文本日志：非图像")
    ctx.put("other/random.bin", ctx.rng("rnd").bytes(W * H), expect(cls="unknown", container=None),
            f"随机字节，大小恰为 {W}×{H}：不应被当成 RAW")
    ctx.put("other/empty.dat", b"", expect(cls="unknown", container=None), "空文件")

    bmp = encode_bmp(img)
    _same(cv_decode(bmp), img, "bmp_named_as")
    ctx.put("other/bmp_named_as.tif", bmp,
            expect(cls="bmp", container="bmp", width=W, height=H, bits_stored=8, bits_effective=eff_bits(img),
                   row_order="bottom_up", palette="gray"),
            "内容是 BMP、扩展名是 .tif：必须按内容识别", "ok")
    tif = tiff_bytes(img, byteorder="<")
    _same(tiff_read(tif)[0], img, "tiff_named_as")
    ctx.put("other/tiff_named_as.dat", tif,
            expect(cls="tiff_uncompressed", container="tiff", width=W, height=H, bits_stored=8,
                   bits_effective=eff_bits(img), byteorder="II", row_order="top_down", layout="strips"),
            "内容是 TIFF、扩展名是 .dat", "ok")
    ctx.put("other/truncated.bmp", bmp[: len(bmp) * 3 // 5],
            expect(cls="unknown", container="bmp", width=W, height=H, bits_stored=8, damaged=True),
            "BMP 被截断到 60%：头部声明的数据不完整，应判为损坏 / 未知并原样或 zstd 兜底")


# ---------------------------------------------------------------- 说明文件

def _fmt_exp(e: dict | None) -> str:
    if not e:
        return "—"
    parts = [e["class"]]
    if e["width"]:
        parts.append(f"{e['width']}×{e['height']}")
    if e["channels"] and e["channels"] > 1:
        parts.append(f"{e['channels']}ch")
    if e["bits_stored"]:
        parts.append(f"{e['bits_stored']}位存/{e['bits_effective']}位有效" + (f"/移{e['shift']}" if e["shift"] else ""))
    for k in ("byteorder", "row_order", "layout", "compression", "pixel_format", "bayer"):
        if e[k]:
            parts.append(str(e[k]))
    if e["row_padding"]:
        parts.append(f"行填充{e['row_padding']}")
    if e["pages"] > 1:
        parts.append(f"{e['pages']}页")
    if e["palette"] == "color":
        parts.append("彩色调色板")
    if e["trailer_bytes"]:
        parts.append(f"尾部{e['trailer_bytes']}B")
    if e["writer"] != "unknown":
        parts.append(f"写图:{e['writer']}")
    if e["raw_config"]:
        parts.append(f"配置:{e['raw_config']}")
    return "，".join(parts)


def write_docs(ctx: Ctx):
    cats = {}
    for e in ctx.entries:
        cats.setdefault(e["category"], []).append(e)
    for cat, items in cats.items():
        lines = [f"# {cat}/ 测试变体\n", CATEGORY_DOC[cat], "",
                 f"共 {len(items)} 个文件，{sum(i['size'] for i in items) / 1e6:.1f} MB。"
                 "“期望识别”即 `manifest.json` 中的 `expect` 真值摘要。", ""]
        if cat == "sequence":
            frames = [i for i in items if i.get("sequence", {}).get("kind") == "frame"]
            ngs = [i["path"].split("/")[-1] for i in frames if i["sequence"]["ng"]]
            lines += [f"- 空载背景：8 张 `background_00..07.bmp`", f"- 产品帧：{len(frames)} 张 `frame_0000..`",
                      f"- NG 帧：{', '.join(ngs) or '无'}",
                      "- 每帧位移、是否 NG 见 manifest 的 `sequence` 字段", ""]
            items = items[:10] + ([{"path": "…", "size": 0, "expect": None, "notes": "（其余帧同上，略）"}]
                                  if len(items) > 10 else [])
        lines += ["| 文件 | 字节 | 期望识别 | 说明 |", "|---|---:|---|---|"]
        for i in items:
            lines.append(f"| `{i['path'].split('/')[-1]}` | {i['size']:,} | {_fmt_exp(i['expect'])} | {i['notes']} |")
        (ctx.root / cat / "README.md").write_text("\n".join(lines) + "\n", encoding="utf-8")

    total = sum(e["size"] for e in ctx.entries)
    top = [
        "# 合成测试集（T1）", "",
        f"- 生成命令：`python -m bench synth --out <目录> --size {ctx.size} --seed {ctx.seed} --seq-frames {ctx.seq_frames}`",
        f"- 尺寸预设 `{ctx.size}`：单图基准 {ctx.wh[0]}×{ctx.wh[1]}，序列 {ctx.seq_wh[0]}×{ctx.seq_wh[1]}",
        f"- 文件数 {len(ctx.entries)}，总量 {total / 1e6:.1f} MB；同一种子重复生成结果逐字节一致",
        "- 图像内容：静态背景 + 产品（平坦基板、拉丝纹理、走线 / 通孔边缘、文字、二维码块、饱和高光）",
        "  + 传感器噪声（散粒噪声 + 读出噪声 + 黑电平；序列另加 PRNU 与列偏置固定模式噪声）",
        "- 每个文件写出后已用独立库读回比对（manifest 的 `readback` 字段）", "",
        "## 目录", "",
    ]
    for cat in cats:
        top.append(f"- `{cat}/`（{len(cats[cat])} 个）：{CATEGORY_DOC[cat]}")
    top += ["", "## manifest.json 字段", "",
            "`expect` 是识别真值：`class`（bmp / tiff_uncompressed / tiff_compressed / raw / bayer / "
            "unsupported / unknown）、`container`、宽高、`channels`、`bits_stored`（容器位深）、"
            "`bits_effective`（实际最大值的有效位数，已扣除 `shift`）、`shift`（高位对齐移位）、`byteorder`、"
            "`row_order`、`row_padding`（每行填充字节）、`pages`、`layout`（strips / tiles）、`compression`、"
            "`palette`、`bayer`（排列）、`pixel_format`（RAW 配置中的 PFNC 名称）、`writer`、"
            "`trailer_bytes`、`raw_config`（rule / sidecar / none）、`damaged`。",
            "", "`writer` 为 halcon / visionpro / camera_sdk 的文件是**模拟指纹**（只写了 Software 标签），"
            "真实指纹需用客户样本校准。opencv 与 tifffile 是真实库写出的。", "",
            "## RAW 配置", "", "见 `raw_config.yaml`：按规则顺序匹配（glob，相对本目录），"
            "字段 width / height / pixel_format / stride（可选，行跨距字节）。"]
    (ctx.root / "README.md").write_text("\n".join(top) + "\n", encoding="utf-8")


def generate(out: Path, size: str = "medium", seed: int = 2026, seq_frames: int = 200,
             only: list[str] | None = None, log=print) -> Ctx:
    if size not in PRESETS:
        raise ValueError(f"未知尺寸预设 {size}，可选 {list(PRESETS)}")
    out.mkdir(parents=True, exist_ok=True)
    if any(out.iterdir()):
        raise FileExistsError(f"输出目录非空：{out}（为避免混入旧文件，请指定空目录）")
    ctx = Ctx(out, size, seed, seq_frames)
    steps = {"bmp": gen_bmp, "tiff": gen_tiff, "raw": gen_raw, "bayer": gen_bayer,
             "sequence": gen_sequence, "other": gen_other}
    for name, fn in steps.items():
        if only and name not in only:
            continue
        n0 = len(ctx.entries)
        fn(ctx)
        log(f"[synth] {name:<9} {len(ctx.entries) - n0:4d} 个文件")
    config = {"raw": ctx.raw_rules}
    header = ("# RAW 参数配置：按顺序匹配，先匹配者生效。match 为相对扫描根目录的 glob。\n"
              "# pixel_format 取 GenICam PFNC 名称：Mono8 / Mono12 / Mono16 / Mono10p / Mono12p / "
              "Mono12Packed / BayerRG8 ...\n# stride：行跨距字节（可选）。\n")
    (out / "raw_config.yaml").write_text(header + yaml.safe_dump(config, allow_unicode=True, sort_keys=False),
                                         encoding="utf-8")
    manifest = {"generator": "bench.synth", "size": size, "seed": seed, "seq_frames": seq_frames,
                "versions": {"numpy": np.__version__, "opencv": cv2.__version__, "tifffile": tifffile.__version__},
                "files": ctx.entries}
    (out / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=1), encoding="utf-8")
    write_docs(ctx)
    return ctx
