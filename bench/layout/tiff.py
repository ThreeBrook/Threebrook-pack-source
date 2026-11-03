"""TIFF 拆分 / 重组（docs/05 4.2、第 5 节）。

骨架：像素块（条带 / 图块）以外的全部字节 —— 文件头、所有 IFD、标签数据（含私有标签）、
      块之间的空隙，原样保存；偏移不变，所以不需要改写任何 IFD。
像素：每页把各像素块拼成“画布”：条带即整幅图；图块按图块网格拼成（含边缘图块的填充区，不裁掉，
      保证填充字节原样还原）；字节序统一为本机；Predictor=2 先还原差分；
      之后单通道按“移位 → Bayer 拆分”整理，3 通道为 RGB 交错平面，其他通道数逐通道成平面。
已压缩（LZW / ZIP / PackBits）：解压得到像素，复现式还原找到能逐字节重编码的编码器才拆分；
      否则抛 LayoutError，由上层整文件兜底。
"""
from __future__ import annotations

import numpy as np

from ..formats import tiff as ftiff
from . import reproduce
from . import transforms as tf
from .model import LayoutError, Plane, Split, assemble, complement


def _geometry(g: dict) -> dict:
    """由页参数推算每个像素块的几何（重组时同样调用，配方里不必逐块记录）。
    g：width height samples bits planar tiled tile rowsperstrip。"""
    bps = g["bits"] // 8
    spp = g["samples"] if g["planar"] == 1 else 1
    groups = 1 if g["planar"] == 1 else g["samples"]
    if g["tiled"]:
        th, tw = g["tile"]
        ny, nx = -(-g["height"] // th), -(-g["width"] // tw)
        segs = [{"rows": th, "cols": tw, "y": (i // nx) * th, "x": (i % nx) * tw} for i in range(ny * nx)]
        canvas = (ny * th, nx * tw)
    else:
        rps = min(g["rowsperstrip"] or g["height"], g["height"])
        n = -(-g["height"] // rps)
        segs = [{"rows": min(rps, g["height"] - i * rps), "cols": g["width"], "y": i * rps, "x": 0} for i in range(n)]
        canvas = (g["height"], g["width"])
    full = [{**sg, "sample": s if g["planar"] == 2 else None} for s in range(groups) for sg in segs]
    return {"bps": bps, "spp": spp, "canvas": canvas, "segs": full}


def _page_params(p: ftiff.TiffPage) -> dict:
    return {"width": p.width, "height": p.height, "samples": p.samples, "bits": p.bits, "planar": p.planar,
            "tiled": p.tiled, "tile": list(p.tile) if p.tile else None, "rowsperstrip": p.rowsperstrip}


def _file_dtype(bps: int, byteorder: str) -> np.dtype:
    return np.dtype("u1") if bps == 1 else np.dtype("<u2" if byteorder == "II" else ">u2")


def split(data: bytes, rec: dict) -> Split:
    info = ftiff.parse_tiff(data)
    bo = info.byteorder
    recipe = {"format": "tiff", "byteorder": bo, "pages": []}
    planes: list[Plane] = []
    extras: dict[str, bytes] = {}
    ranges = []
    for p in info.pages:
        comp = p.compression_name
        if comp not in (None, "lzw", "zip", "packbits"):
            raise LayoutError(f"压缩方式 {comp} 不支持")
        if p.predictor not in (1, 2):
            raise LayoutError(f"Predictor={p.predictor} 不支持")
        geo = _geometry(_page_params(p))
        if len(p.offsets) != len(geo["segs"]):
            raise LayoutError(f"第 {p.index} 页像素块数 {len(p.offsets)} 与几何推算 {len(geo['segs'])} 不符")
        bps, spp = geo["bps"], geo["spp"]
        fdt = _file_dtype(bps, bo)
        ndt = np.dtype("u1" if bps == 1 else "u2")
        canvas = np.zeros((*geo["canvas"], p.samples), ndt)
        repro_inputs = []
        for i, (g, off, cnt) in enumerate(zip(geo["segs"], p.offsets, p.bytecounts)):
            ranges.append((off, cnt))
            seg = data[off:off + cnt]
            if len(seg) != cnt:
                raise LayoutError("像素块越界")
            row_bytes = g["cols"] * spp * bps
            expect = g["rows"] * row_bytes
            raw = seg if comp is None else reproduce.decoder(comp)(seg)
            if len(raw) < expect:
                raise LayoutError(f"第 {p.index} 页第 {i} 块数据不足：{len(raw)} < {expect}")
            if len(raw) > expect:
                if comp is not None:
                    raise LayoutError("解压长度超出几何推算，无法复现")
                extras[f"p{p.index}.seg{i}.tail"] = raw[expect:]
                raw = raw[:expect]
            if comp is not None:
                repro_inputs.append((seg, raw, row_bytes))
            v = np.frombuffer(raw, fdt).reshape(g["rows"], g["cols"], spp).astype(ndt)
            if p.predictor == 2:
                v = tf.delta_decode(v, axis=1)
            ys, xs = slice(g["y"], g["y"] + g["rows"]), slice(g["x"], g["x"] + g["cols"])
            if g["sample"] is None:
                canvas[ys, xs, :] = v
            else:
                canvas[ys, xs, g["sample"]] = v[..., 0]
        encoders = None
        if comp is not None:
            encoders = reproduce.find(comp, repro_inputs)
            if encoders is None:
                raise LayoutError(f"第 {p.index} 页 {comp} 复现式还原失败：候选编码器都无法逐字节复现")
        prec = {"index": p.index, **_page_params(p), "predictor": p.predictor, "compression": comp,
                "offsets": list(p.offsets), "bytecounts": list(p.bytecounts),
                "encoders": "".join("L" if e == "libtiff.lzw" else "-" for e in encoders) if comp == "lzw" else None,
                "encoder": None if encoders is None else encoders[0] if comp != "lzw" else "lzw(ic|libtiff)"}
        name = f"p{p.index}"
        if p.samples == 1:
            shift = rec.get("shift") or 0
            try:
                ps, prec["gray"] = tf.gray_to_planes(name, canvas[..., 0], shift, rec["class"] == "bayer")
            except LayoutError:          # 图块填充区低位非 0 等：不移位
                ps, prec["gray"] = tf.gray_to_planes(name, canvas[..., 0], 0, rec["class"] == "bayer")
            planes += ps
        elif p.samples == 3:
            rgb = np.ascontiguousarray(canvas)
            planes.append(Plane(f"{name}.rgb", rgb, tf.eff_bits(rgb)))
        else:
            for k in range(p.samples):
                ch = np.ascontiguousarray(canvas[..., k])
                planes.append(Plane(f"{name}.s{k}", ch, tf.eff_bits(ch)))
        recipe["pages"].append(prec)
    if any(pr["compression"] for pr in recipe["pages"]):
        recipe["lib_versions"] = reproduce.LIB_VERSIONS
    skeleton = complement(ranges, len(data), data)
    return Split("tiff", len(data), rec["sha256"], recipe, skeleton, planes, extras)


def rebuild(s: Split) -> bytes:
    r = s.recipe
    bo = r["byteorder"]
    planes = {p.name: p.data for p in s.planes}
    pieces = []
    for pr in r["pages"]:
        name = f"p{pr['index']}"
        geo = _geometry(pr)
        bps, ns = geo["bps"], pr["samples"]
        ndt = np.dtype("u1" if bps == 1 else "u2")
        if ns == 1:
            canvas = tf.planes_to_gray(planes, pr["gray"])[..., None]
        elif ns == 3:
            canvas = planes[f"{name}.rgb"]
        else:
            canvas = np.stack([planes[f"{name}.s{k}"] for k in range(ns)], axis=2)
        canvas = canvas.astype(ndt)
        if canvas.shape[:2] != geo["canvas"]:
            raise LayoutError("画布尺寸不符")
        fdt = _file_dtype(bps, bo)
        comp = pr["compression"]
        for i, (g, off, cnt) in enumerate(zip(geo["segs"], pr["offsets"], pr["bytecounts"])):
            ys, xs = slice(g["y"], g["y"] + g["rows"]), slice(g["x"], g["x"] + g["cols"])
            v = canvas[ys, xs, :] if g["sample"] is None else canvas[ys, xs, g["sample"]][..., None]
            if pr["predictor"] == 2:
                v = tf.delta_encode(v, axis=1)
            raw = np.ascontiguousarray(v).astype(fdt).tobytes()
            row_bytes = v.shape[1] * v.shape[2] * bps
            if comp is None:
                seg = raw + s.extras.get(f"p{pr['index']}.seg{i}.tail", b"")
            else:
                cid = pr["encoder"]
                if comp == "lzw":
                    cid = "libtiff.lzw" if pr["encoders"][i] == "L" else "ic.lzw"
                seg = reproduce.encode(cid, raw, row_bytes)
            if len(seg) != cnt:
                raise LayoutError(f"第 {pr['index']} 页第 {i} 块重组长度 {len(seg)} ≠ {cnt}")
            pieces.append((off, seg))
    return assemble(s.size, s.skeleton, pieces)

