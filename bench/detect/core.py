"""T2 格式识别：按内容（魔数与关键字段）识别，不看扩展名。

分类（class）：
  bmp / tiff_uncompressed / tiff_compressed / raw / bayer / unsupported / unknown
- bayer：任意容器中检测到 Bayer 马赛克（伪装成灰度的 BMP / TIFF / RAW，或 RAW 配置明确为 Bayer）
- unsupported：能识别但不在 P0 处理范围（JPEG、PNG、浮点 TIFF、RLE BMP ……），原样 / zstd 存储
- unknown：无法识别、非图像、损坏
"""
from __future__ import annotations

import hashlib
import io
import struct
from pathlib import Path

import numpy as np
import tifffile

from ..formats import bmp as fbmp
from ..formats import raw as fraw
from ..formats import tiff as ftiff
from ..formats.magic import HEAD_LEN, looks_like_text, sniff
from . import stats, writer

FIELDS = ["path", "size", "sha256", "class", "container", "width", "height", "channels", "bits_stored",
          "bits_effective", "value_max", "shift", "byteorder", "row_order", "row_padding", "pages", "layout",
          "compression", "predictor", "palette", "bayer", "bayer_candidates", "bayer_guess", "bayer_confidence",
          "pixel_format", "raw_config", "raw_params", "raw_candidates", "writer", "writer_evidence", "trailer_bytes",
          "damaged", "processable", "notes", "error"]


def _record(path: str, data: bytes) -> dict:
    r = dict.fromkeys(FIELDS)
    r.update(path=path, size=len(data), sha256=hashlib.sha256(data).hexdigest(), channels=None, pages=1,
             row_padding=0, shift=0, trailer_bytes=0, damaged=False, processable=False, writer="unknown",
             notes=[], bayer_candidates=[])
    return r


def _apply_stats(r: dict, arrays, bits_stored: int, bayer_img: np.ndarray | None):
    r.update(stats.value_stats(arrays, bits_stored))
    if bayer_img is not None:
        b = stats.bayer_detect(bayer_img >> r["shift"] if r["shift"] else bayer_img,
                               (1 << (bits_stored - r["shift"])) - 1)
        if b["is_bayer"]:
            r.update(bayer_candidates=b["candidates"], bayer_guess=b["guess"], bayer_confidence=b["confidence"])
            r["notes"].append(f"Bayer 检测：G 在{'主' if b['g_diagonal'] == 'main' else '副'}对角线，"
                              f"候选 {'/'.join(b['candidates'])}，置信度 {b['confidence']}，"
                              f"有效图块 {b['tiles_valid']}，一致率 {b['agree']}")
        return b["is_bayer"]
    return False


# ---------------------------------------------------------------- BMP

def _detect_bmp(r: dict, data: bytes):
    r["container"] = "bmp"
    try:
        info = fbmp.parse_bmp(data)
    except fbmp.Unsupported as e:
        r.update({"class": "unsupported", "error": str(e)})
        return
    except fbmp.BmpError as e:
        r.update({"class": "unknown", "damaged": True, "error": str(e)})
        return
    r.update(width=info.width, height=info.height, channels=info.channels, bits_stored=8,
             row_order="top_down" if info.top_down else "bottom_up", row_padding=info.row_padding,
             trailer_bytes=info.trailer_len)
    r["notes"] += info.problems
    r["writer"], r["writer_evidence"] = writer.from_bmp(info)
    if info.damaged:
        r.update({"class": "unknown", "damaged": True, "trailer_bytes": 0,
                  "error": f"像素数据应到 {info.pixel_end} 字节，文件只有 {info.file_len} 字节（截断）"})
        return
    if info.bits == 8:
        r["palette"] = "gray" if info.gray_palette else "color"
    px = fbmp.pixels(data, info)
    gray = info.channels == 1 and r["palette"] == "gray"
    is_bayer = _apply_stats(r, [px], 8, px if gray else None)
    if info.channels == 4:
        alpha = np.unique(px[..., 3])
        if alpha.size == 1:
            r["notes"].append(f"Alpha 恒为 {int(alpha[0])}")
    if info.trailer_len:
        r["notes"].append(f"文件尾附加 {info.trailer_len} 字节")
    r["class"] = "bayer" if is_bayer else "bmp"
    r["processable"] = True


# ---------------------------------------------------------------- TIFF

def _detect_tiff(r: dict, data: bytes):
    r["container"] = "tiff"
    try:
        info = ftiff.parse_tiff(data)
    except ftiff.TiffError as e:
        r.update({"class": "unknown", "damaged": True, "error": str(e)})
        return
    p0 = info.pages[0]
    comps = sorted({p.compression_name or "none" for p in info.pages})
    r.update(byteorder=info.byteorder, pages=len(info.pages), width=p0.width, height=p0.height,
             channels=p0.samples, bits_stored=p0.bits,
             layout="tiles" if p0.tiled else "strips", predictor=p0.predictor,
             row_order="top_down" if p0.orientation == 1 else f"orientation={p0.orientation}")
    r["writer"], r["writer_evidence"] = writer.from_tiff(info)
    if info.bigtiff:
        r["notes"].append("BigTIFF")
    if info.private_tags:
        r["notes"].append("私有标签：" + ", ".join(f"{c}({n})" for c, n in info.private_tags))
    if len({(p.width, p.height, p.bits, p.samples) for p in info.pages}) > 1:
        r["notes"].append("多页尺寸 / 位深不一致")
    bad = [c for c in comps if c.startswith("other:")]
    reason = None
    if bad:
        reason = f"压缩方式 {', '.join(bad)} 不在复现式还原范围"
    elif any(p.sampleformat not in (1,) for p in info.pages):
        reason = "非无符号整数采样（浮点 / 有符号）"
    elif any(p.bits not in (8, 16) for p in info.pages):
        reason = f"位深 {p0.bits} 不在 8 / 16 位范围"
    if reason:
        r.update({"class": "unsupported", "error": reason})
        return
    compressed = [c for c in comps if c != "none"]
    r["compression"] = "+".join(compressed) if compressed else None
    arrays = []
    try:
        with tifffile.TiffFile(io.BytesIO(data)) as t:
            for p in t.pages:
                arrays.append(p.asarray())
    except Exception as e:
        r.update({"class": "unknown", "damaged": True, "error": f"像素解码失败：{e}"})
        return
    single = p0.samples == 1 and p0.photometric in (0, 1)
    is_bayer = _apply_stats(r, arrays, p0.bits, arrays[0] if single else None)
    if compressed:
        r["class"] = "tiff_compressed"
        if is_bayer:
            r["notes"].append("已压缩 TIFF 中的 Bayer 图（复现式还原通过后才可拆分压缩）")
    else:
        r["class"] = "bayer" if is_bayer else "tiff_uncompressed"
    r["processable"] = True


# ---------------------------------------------------------------- RAW

def _detect_raw(r: dict, data: bytes, params: fraw.RawParams):
    r.update(container="raw", width=params.width, height=params.height, channels=1,
             bits_stored=params.bits_stored, pixel_format=params.pixel_format,
             byteorder=None if params.bits_stored == 8 else "II", row_order="top_down",
             row_padding=params.row_padding,
             raw_config={"rule": "rule", "sidecar": "sidecar", "guess": "none"}[params.source],
             writer_evidence="RAW 无文件头，无法推测写图程序",
             raw_params={"width": params.width, "height": params.height, "pixel_format": params.pixel_format,
                         "stride": params.stride, "offset": params.offset})
    r["notes"].append(f"RAW 参数来源：{params.source} {params.origin}".strip())
    try:
        img = fraw.decode(data, params)
    except fraw.RawError as e:
        r.update({"class": "raw", "error": f"参数与文件不符：{e}"})
        return
    extra = len(data) - params.offset - params.data_len
    if extra:
        r["trailer_bytes"] = extra
        r["notes"].append(f"按参数解码后文件尾多出 {extra} 字节")
    is_bayer = _apply_stats(r, [img], 16 if params.bpp == 2 else params.bits_stored, img)
    if params.bayer:
        r.update(bayer=params.bayer, bayer_candidates=[params.bayer], bayer_guess=params.bayer)
        r["notes"].append(f"配置给定 Bayer 排列 {params.bayer}")
        is_bayer = True
    r["class"] = "bayer" if is_bayer else "raw"
    r["processable"] = True


# ---------------------------------------------------------------- 其他已知格式

def _image_dims(container: str, data: bytes):
    if container == "png" and len(data) >= 26:
        w, h = struct.unpack_from(">II", data, 16)
        ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(data[25])
        return w, h, ch
    if container == "jpeg":
        i = 2
        while i + 9 < len(data):
            if data[i] != 0xFF:
                break
            marker, seglen = data[i + 1], struct.unpack_from(">H", data, i + 2)[0]
            if marker in (0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7, 0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF):
                h, w = struct.unpack_from(">HH", data, i + 5)
                return w, h, data[i + 9]
            i += 2 + seglen
    return None, None, None


def detect_bytes(rel: str, data: bytes, path: Path | None = None, rules: list | None = None) -> dict:
    r = _record(rel, data)
    if not data:
        r.update({"class": "unknown", "notes": ["空文件"]})
        return r
    container = sniff(data[:HEAD_LEN])
    if container == "bmp":
        _detect_bmp(r, data)
    elif container == "tiff":
        _detect_tiff(r, data)
    elif container is not None:
        w, h, ch = _image_dims(container, data)
        r.update({"class": "unsupported", "container": container, "width": w, "height": h, "channels": ch})
        r["notes"].append(f"{container}：不在 P0 处理范围，原样 / zstd 存储")
    else:
        params = None
        try:
            params = fraw.match_rule(rules or [], rel) or (fraw.find_sidecar(path) if path else None)
        except fraw.RawError as e:
            r["error"] = str(e)
        if params:
            _detect_raw(r, data, params)
        elif looks_like_text(data):
            owner = fraw.is_sidecar_of_image(path) if path else None
            r.update({"class": "unknown"})
            r["notes"].append(f"文本文件{'（' + owner.name + ' 的参数文件）' if owner else ''}")
        else:
            cands = fraw.size_candidates(data)
            r["raw_candidates"] = cands
            if cands and cands[0]["cost"] < fraw.IMAGE_COST_MAX:
                best = cands[0]
                guess = fraw.RawParams(best["width"], best["height"], best["pixel_format"], source="guess",
                                       origin=f"按文件大小推测（像图像程度 {best['cost']}）")
                _detect_raw(r, data, guess)
                r["processable"] = False       # 无配置的 RAW 不压缩，先请用户确认参数
                r["notes"].append("无 RAW 配置：候选参数见 raw_candidates，确认后写入配置才会压缩")
            else:
                r.update({"class": "unknown"})
                r["notes"].append("无法识别的二进制数据（按文件大小推出的候选都不像图像）")
    return r


def detect_file(path: Path, root: Path, rules: list | None = None) -> dict:
    rel = path.relative_to(root).as_posix()
    try:
        data = path.read_bytes()
    except OSError as e:
        r = _record(rel, b"")
        r.update({"class": "unknown", "error": f"读取失败：{e}"})
        return r
    try:
        return detect_bytes(rel, data, path, rules)
    except Exception as e:  # 不中断：单文件出错只记录
        r = _record(rel, data)
        r.update({"class": "unknown", "error": f"识别异常：{type(e).__name__}: {e}"})
        return r
