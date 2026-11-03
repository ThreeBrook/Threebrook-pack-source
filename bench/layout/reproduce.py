"""复现式还原（docs/05 第 5 节）：已压缩 TIFF 的像素块，用候选编码器按记录参数重新编码，
与原字节逐字节一致才采用；否则整文件兜底。

候选按“编码器族”组织，每个族内若有多个参数，在第一个像素块上试出参数后用于全部像素块。
LZW 的 libtiff 兼容编码器较慢，先用 imagecodecs 试，不一致的块再用 libtiff 版；配方逐块记录。
"""
from __future__ import annotations

import zlib

import imagecodecs

from . import lzw_libtiff

LIB_VERSIONS = {
    "imagecodecs": imagecodecs.__version__,
    "zlib(imagecodecs)": imagecodecs.version(dict).get("zlib", "?"),
    "libdeflate": imagecodecs.version(dict).get("libdeflate", "?"),
    "zlib(python)": zlib.ZLIB_RUNTIME_VERSION,
    "lzw_libtiff": "bench port of libtiff tif_lzw.c",
}


def _packbits_rows(raw: bytes, row_bytes: int) -> bytes:
    """TIFF PackBits 按行打包（libtiff / tifffile 的做法），行间不跨游程。"""
    return b"".join(imagecodecs.packbits_encode(raw[i:i + row_bytes]) for i in range(0, len(raw), row_bytes))


def _encoders(compression: str):
    """返回 [(编码器 id, 函数(raw, row_bytes) -> bytes)]。"""
    if compression == "lzw":
        return [("ic.lzw", lambda raw, rb: imagecodecs.lzw_encode(raw)),
                ("libtiff.lzw", lambda raw, rb: lzw_libtiff.encode(raw))]
    if compression == "zip":
        out = []
        for lv in (6, 9, 1, 2, 3, 4, 5, 7, 8):            # 常见默认值优先
            out.append((f"ic.zlib{lv}", lambda raw, rb, lv=lv: imagecodecs.zlib_encode(raw, level=lv)))
            out.append((f"ic.deflate{lv}", lambda raw, rb, lv=lv: imagecodecs.deflate_encode(raw, level=lv)))
            out.append((f"py.zlib{lv}", lambda raw, rb, lv=lv: zlib.compress(raw, lv)))
        return out
    if compression == "packbits":
        return [("ic.packbits.rows", _packbits_rows),
                ("ic.packbits", lambda raw, rb: imagecodecs.packbits_encode(raw))]
    return []


ENCODERS = {}
for _c in ("lzw", "zip", "packbits"):
    ENCODERS.update(dict(_encoders(_c)))


def decoder(compression: str):
    return {"lzw": imagecodecs.lzw_decode, "zip": imagecodecs.zlib_decode,
            "packbits": imagecodecs.packbits_decode}[compression]


def find(compression: str, segments: list[tuple[bytes, bytes, int]]) -> list[str] | None:
    """segments：[(原压缩字节, 预测后的未压缩字节, 行字节数)]。
    返回每个块使用的编码器 id；找不到能复现全部块的方案返回 None。"""
    cands = _encoders(compression)
    if not cands or not segments:
        return None
    if compression == "lzw":
        # 逐块：先快后慢
        out = []
        for orig, raw, rb in segments:
            for cid, enc in cands:
                if enc(raw, rb) == orig:
                    out.append(cid)
                    break
            else:
                return None
        return out
    orig0, raw0, rb0 = segments[0]
    for cid, enc in cands:
        if enc(raw0, rb0) == orig0 and all(enc(raw, rb) == orig for orig, raw, rb in segments[1:]):
            return [cid] * len(segments)
    return None


def encode(cid: str, raw: bytes, row_bytes: int) -> bytes:
    return ENCODERS[cid](raw, row_bytes)
