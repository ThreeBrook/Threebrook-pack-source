"""可控的 BMP 写入器，用来制造各种版面变体（不追求通用，只服务测试数据）。"""
from __future__ import annotations

import struct

import numpy as np

BI_RGB = 0
BI_BITFIELDS = 3


def gray_palette() -> np.ndarray:
    i = np.arange(256, dtype=np.uint8)
    return np.stack([i, i, i, np.zeros(256, np.uint8)], axis=1)  # B G R 0


def row_stride(width: int, bits: int) -> int:
    return ((width * bits + 31) // 32) * 4


def encode_bmp(
    img: np.ndarray,
    *,
    palette: np.ndarray | None = None,
    top_down: bool = False,
    header: int = 40,
    pad_bytes: bytes | None = None,
    trailer: bytes = b"",
    size_image_zero: bool = False,
    ppm: tuple[int, int] = (2835, 2835),
    clr_used: int | None = None,
) -> bytes:
    """img: 8 位 (H,W) 索引/灰度；24 位 (H,W,3) BGR；32 位 (H,W,4) BGRA。

    pad_bytes: 每行填充字节的来源（循环取用）；None 表示全 0。
    header: 40 (BITMAPINFOHEADER) 或 124 (BITMAPV5HEADER)。
    """
    if img.dtype != np.uint8:
        raise TypeError("BMP 只写 uint8")
    h, w = img.shape[:2]
    ch = 1 if img.ndim == 2 else img.shape[2]
    bits = {1: 8, 3: 24, 4: 32}[ch]
    if bits == 8 and palette is None:
        palette = gray_palette()
    stride = row_stride(w, bits)
    pad = stride - w * ch

    rows = img.reshape(h, w * ch)
    if not top_down:
        rows = rows[::-1]
    buf = np.zeros((h, stride), np.uint8)
    buf[:, : w * ch] = rows
    if pad and pad_bytes:
        src = np.frombuffer(pad_bytes, np.uint8)
        fill = np.resize(src, h * pad).reshape(h, pad)
        buf[:, w * ch :] = fill
    pixels = buf.tobytes()

    compression = BI_BITFIELDS if (bits == 32 and header >= 108) else BI_RGB
    pal = palette.astype(np.uint8).tobytes() if bits == 8 else b""
    off = 14 + header + len(pal)
    file_size = off + len(pixels) + len(trailer)
    if clr_used is None:
        clr_used = 256 if bits == 8 else 0
    info = struct.pack(
        "<IiiHHIIiiII",
        header, w, -h if top_down else h, 1, bits, compression,
        0 if size_image_zero else len(pixels), ppm[0], ppm[1], clr_used, 0,
    )
    if header == 124:
        masks = (0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000) if bits == 32 else (0, 0, 0, 0)
        info += struct.pack("<IIII", *masks)
        info += b"BGRs"            # LCS_sRGB
        info += bytes(36 + 12)     # CIEXYZTRIPLE endpoints + gamma RGB
        info += struct.pack("<IIII", 4, 0, 0, 0)  # intent=LCS_GM_IMAGES, profile data/size, reserved
    elif header != 40:
        raise ValueError("只支持 40 / 124 字节 DIB 头")
    assert len(info) == header
    fh = b"BM" + struct.pack("<IHHI", file_size, 0, 0, off)
    return fh + info + pal + pixels + trailer
