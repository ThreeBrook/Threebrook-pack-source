"""BMP 拆分 / 重组（docs/05 4.1）。

骨架：文件头 + DIB 头 + 调色板（像素偏移之前的全部字节）、文件尾附加数据，原样保存。
像素：行序统一为自上而下；去掉行填充（全 0 只记标志，非 0 原样保存）；
      8 位灰度 → 灰度平面（Bayer 时拆四平面）；8 位彩色调色板 → 索引平面；
      24 位 BGR → RGB 交错平面；32 位 BGRA → RGB 平面 + Alpha（恒值只记数值）。
"""
from __future__ import annotations

import numpy as np

from ..formats import bmp as fbmp
from . import transforms as tf
from .model import LayoutError, Plane, Split, assemble, complement


def split(data: bytes, rec: dict) -> Split:
    info = fbmp.parse_bmp(data)
    if info.damaged:
        raise LayoutError("BMP 截断")
    rows = fbmp.pixel_rows(data, info)
    pix, pad = tf.split_padding(rows, info.row_bytes)
    if not info.top_down:
        pix = pix[::-1]
    h, w, ch = info.height, info.width, info.channels
    recipe = {"format": "bmp", "width": w, "height": h, "channels": ch, "top_down": info.top_down,
              "stride": info.stride, "pixel_offset": info.pixel_offset, "padding_zero": pad is None}
    extras = {} if pad is None else {"row_padding": pad}
    planes: list[Plane] = []
    if ch == 1:
        name = "gray" if info.gray_palette else "index"
        ps, recipe["gray"] = tf.gray_to_planes(name, np.ascontiguousarray(pix), 0,
                                               rec["class"] == "bayer" and name == "gray")
        planes += ps
    else:
        px = pix.reshape(h, w, ch)
        rgb = np.ascontiguousarray(px[..., 2::-1])
        planes.append(Plane("rgb", rgb, tf.eff_bits(rgb)))
        if ch == 4:
            a = px[..., 3]
            if (a == a.flat[0]).all():
                recipe["alpha_const"] = int(a.flat[0])
            else:
                planes.append(Plane("alpha", np.ascontiguousarray(a), tf.eff_bits(a)))
    skeleton = complement([(info.pixel_offset, info.pixel_len)], len(data), data)
    return Split("bmp", len(data), rec["sha256"], recipe, skeleton, planes, extras)


def rebuild(s: Split) -> bytes:
    r = s.recipe
    planes = {p.name: p.data for p in s.planes}
    h, w, ch = r["height"], r["width"], r["channels"]
    if ch == 1:
        px = tf.planes_to_gray(planes, r["gray"]).astype(np.uint8)
    else:
        rgb = planes["rgb"]
        px = np.empty((h, w, ch), np.uint8)
        px[..., :3] = rgb[..., ::-1]
        if ch == 4:
            px[..., 3] = r["alpha_const"] if "alpha_const" in r else planes["alpha"]
    px = px.reshape(h, w * ch)
    if not r["top_down"]:
        px = px[::-1]
    rows = tf.join_padding(np.ascontiguousarray(px), r["stride"], s.extras.get("row_padding"))
    return assemble(s.size, s.skeleton, [(r["pixel_offset"], rows.tobytes())])
