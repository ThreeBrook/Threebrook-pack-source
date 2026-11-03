"""RAW 拆分 / 重组（docs/05 4.3、4.4）。

骨架：文件头（offset 之前）与参数覆盖范围之后的多余字节，原样保存。
像素：非打包 → 去行跨距填充、16 位小端转本机；打包 → 解包为 16 位，重组时按原格式重新打包，
      最后一个字节含未用位，原样保存以防非 0；之后按“移位 → Bayer 拆分”整理，逆序还原。
"""
from __future__ import annotations

import numpy as np

from ..formats import raw as fraw
from ..layout import packing
from . import transforms as tf
from .model import LayoutError, Split, assemble, complement


def split(data: bytes, rec: dict) -> Split:
    if not rec.get("raw_params"):
        raise LayoutError("RAW 无参数")
    p = fraw.RawParams(**rec["raw_params"], source="rule")
    body_off, body_len = p.offset, p.data_len
    if body_off + body_len > len(data):
        raise LayoutError("RAW 参数超出文件长度")
    recipe = {"format": "raw", "params": rec["raw_params"], "body_offset": body_off, "body_len": body_len}
    extras = {}
    if p.packed:
        img = fraw.decode(data, p)
        body = data[body_off: body_off + body_len]
        if packing.pack(p.packed, img)[:-1] != body[:-1]:
            raise LayoutError("打包格式重新打包不一致")
        extras["packed_tail"] = body[-1:]          # 末字节可能含未用位
    else:
        rows = np.frombuffer(data, np.uint8, count=body_len, offset=body_off).reshape(p.height, p.row_stride)
        pix, pad = tf.split_padding(rows, p.width * p.bpp)
        recipe["padding_zero"] = pad is None
        if pad is not None:
            extras["row_padding"] = pad
        img = tf.to_native(np.ascontiguousarray(pix), p.bpp, "II").reshape(p.height, p.width)
    planes, recipe["gray"] = tf.gray_to_planes("gray", np.ascontiguousarray(img), rec.get("shift") or 0,
                                               rec["class"] == "bayer")
    skeleton = complement([(body_off, body_len)], len(data), data)
    return Split("raw", len(data), rec["sha256"], recipe, skeleton, planes, extras)


def rebuild(s: Split) -> bytes:
    r = s.recipe
    p = fraw.RawParams(**r["params"], source="rule")
    img = tf.planes_to_gray({pl.name: pl.data for pl in s.planes}, r["gray"])
    if p.packed:
        body = packing.pack(p.packed, img)
        body = body[:-1] + s.extras["packed_tail"]
    else:
        pix = tf.to_file(img, p.bpp, "II").reshape(p.height, p.width * p.bpp)
        body = tf.join_padding(pix, p.row_stride, s.extras.get("row_padding")).tobytes()
    if len(body) != r["body_len"]:
        raise LayoutError("RAW 重组长度不符")
    return assemble(s.size, s.skeleton, [(r["body_offset"], body)])
