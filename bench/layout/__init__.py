"""版面整理：行序、填充、字节序、解包/打包、移位、Bayer 拆分（全部整数、可逆）。

split(data, rec) 把文件拆成 Split（骨架 + 像素平面 + 附加字节 + 配方）；rebuild(split) 只用 Split 重组原文件。
无法安全拆分时抛 LayoutError，上层回退到 zstd / 原样。
"""
from __future__ import annotations

from .model import LayoutError, Plane, Split

__all__ = ["LayoutError", "Plane", "Split", "split", "rebuild", "SPLITTABLE"]

SPLITTABLE = {"bmp", "tiff_uncompressed", "tiff_compressed", "raw", "bayer"}


def split(data: bytes, rec: dict) -> Split:
    from . import bmp, raw, tiff
    if rec["class"] not in SPLITTABLE or not rec.get("processable"):
        raise LayoutError(f"类别 {rec['class']} 不拆分" + ("（未确认参数）" if rec["class"] in SPLITTABLE else ""))
    mod = {"bmp": bmp, "tiff": tiff, "raw": raw}.get(rec["container"])
    if mod is None:
        raise LayoutError(f"容器 {rec['container']} 不支持")
    return mod.split(data, rec)


def rebuild(s: Split) -> bytes:
    from . import bmp, raw, tiff
    return {"bmp": bmp, "tiff": tiff, "raw": raw}[s.fmt].rebuild(s)
