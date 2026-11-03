"""编码器统一接口与容器体积估算。

像素编码器（作用于 layout 拆出的平面）：png / jpegls / htj2k / jxl3 / jxl7 / jxl9
整文件编码器（不拆分，通用对照）：zstd3 / zstd19
全部单线程；位深按平面实际有效位数传给编码器（JPEG-LS MAXVAL / HTJ2K / JPEG XL 位深）。
"""
from __future__ import annotations

import json
from dataclasses import dataclass
from typing import Callable

import imagecodecs as ic
import numpy as np

from .layout.model import Plane, Split

_V = ic.version(dict)


def _prep(p: Plane) -> np.ndarray:
    """有效位 ≤ 8 的 16 位平面转 8 位（值不变，无损），编码器按 8 位处理。"""
    a = p.data
    if a.dtype == np.uint16 and p.bits <= 8:
        a = a.astype(np.uint8)
    return np.ascontiguousarray(a)


def _rgb(a: np.ndarray) -> bool:
    return a.ndim == 3 and a.shape[2] == 3


@dataclass(frozen=True)
class Codec:
    id: str
    label: str
    lib: str
    encode: Callable[[Plane], bytes]
    decode: Callable[[bytes], np.ndarray]


def _jpegls(p: Plane) -> bytes:
    a = _prep(p)
    return ic.jpegls_encode(a, level=0, bitspersample=max(2, min(p.bits, a.itemsize * 8)))


def _htj2k(p: Plane) -> bytes:
    a = _prep(p)
    kw = {"rgb": True} if _rgb(a) else {}
    return ic.htj2k_encode(a, reversible=True, bitspersample=max(2, min(p.bits, a.itemsize * 8)), **kw)


def _jxl(effort: int):
    def enc(p: Plane) -> bytes:
        a = _prep(p)
        return ic.jpegxl_encode(a, lossless=True, effort=effort, numthreads=1,
                                bitspersample=max(1, min(p.bits, a.itemsize * 8)))
    return enc


PIXEL_CODECS: dict[str, Codec] = {
    "png": Codec("png", "PNG（最高压缩）", f"libpng {_V.get('libpng')}",
                 lambda p: ic.png_encode(_prep(p), level=9), ic.png_decode),
    "jpegls": Codec("jpegls", "JPEG-LS（near=0）", f"CharLS {_V.get('charls')}", _jpegls, ic.jpegls_decode),
    "htj2k": Codec("htj2k", "HTJ2K 无损", f"OpenJPH {_V.get('openjph')}", _htj2k, ic.htj2k_decode),
    "jxl3": Codec("jxl3", "JPEG XL 无损 e3", f"libjxl {_V.get('libjxl')}", _jxl(3),
                  lambda b: ic.jpegxl_decode(b, numthreads=1)),
    "jxl7": Codec("jxl7", "JPEG XL 无损 e7", f"libjxl {_V.get('libjxl')}", _jxl(7),
                  lambda b: ic.jpegxl_decode(b, numthreads=1)),
    "jxl9": Codec("jxl9", "JPEG XL 无损 e9", f"libjxl {_V.get('libjxl')}", _jxl(9),
                  lambda b: ic.jpegxl_decode(b, numthreads=1)),
}
FILE_CODECS = {"zstd3": 3, "zstd19": 19}
FILE_LABELS = {"zstd3": "zstd -3（整文件）", "zstd19": "zstd -19（整文件）"}
ALL_SCHEMES = list(FILE_CODECS) + list(PIXEL_CODECS)
LIB_VERSIONS = {"imagecodecs": ic.__version__, "zstd": _V.get("zstd"), "charls": _V.get("charls"),
                "openjph": _V.get("openjph"), "libjxl": _V.get("libjxl"), "libpng": _V.get("libpng")}

# ---------------------------------------------------------------- 容器体积（按 docs/05 第 7 节草案估算）

HEADER_BYTES = 8 + 2 + 2 + 8 + 32 + 4 + 4      # magic version flags orig_size sha256 recipe_len segment_count
SEGMENT_HEADER_BYTES = 1 + 1 + 8 + 8 + 8 + 4   # type codec orig_offset orig_length data_length crc32c


def nonpixel_blob(s: Split) -> bytes:
    return b"".join(b for _, b in s.skeleton) + b"".join(s.extras[k] for k in sorted(s.extras))


def recipe_json(s: Split) -> bytes:
    meta = {"recipe": s.recipe, "skeleton": [[o, len(b)] for o, b in s.skeleton],
            "extras": {k: len(v) for k, v in sorted(s.extras.items())}}
    return json.dumps(meta, ensure_ascii=False, separators=(",", ":")).encode()


def container_size(s: Split, streams: list[bytes], blob_z: bytes | None) -> dict:
    """算全：像素码流 + 非像素字节（zstd）+ 配方 + 容器头与段头。"""
    rj = recipe_json(s)
    nseg = len(streams) + (1 if blob_z else 0)
    pix = sum(len(b) for b in streams)
    nonpix = len(blob_z) if blob_z else 0
    overhead = HEADER_BYTES + nseg * SEGMENT_HEADER_BYTES
    return {"pixel": pix, "nonpixel": nonpix, "recipe": len(rj), "overhead": overhead,
            "total": pix + nonpix + len(rj) + overhead}


def zstd_file_size(compressed_len: int) -> int:
    """“原文件 + zstd”兜底的存储体积：一个 zstd 段 + 容器头。"""
    return compressed_len + HEADER_BYTES + SEGMENT_HEADER_BYTES
