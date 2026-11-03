"""BMP 解析：只读头部与版面参数，不改动任何字节。

支持 P0 范围：8 位（调色板）、24 位、32 位（BI_RGB 或标准 BGRA 掩码的 BI_BITFIELDS）。
其他（1/4/16 位、RLE、内嵌 JPEG/PNG、OS/2 头）抛 Unsupported，由上层按“不处理”兜底。
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field

import numpy as np

DIB_SIZES = {12: "BITMAPCOREHEADER", 40: "BITMAPINFOHEADER", 52: "BITMAPV2INFOHEADER",
             56: "BITMAPV3INFOHEADER", 108: "BITMAPV4HEADER", 124: "BITMAPV5HEADER"}
BI_RGB, BI_RLE8, BI_RLE4, BI_BITFIELDS, BI_JPEG, BI_PNG, BI_ALPHABITFIELDS = range(7)
STD_MASKS_32 = (0x00FF0000, 0x0000FF00, 0x000000FF)


class BmpError(ValueError):
    """不是合法 BMP。"""


class Unsupported(ValueError):
    """合法 BMP，但不在 P0 处理范围。"""


@dataclass
class BmpInfo:
    file_len: int
    file_size_field: int
    pixel_offset: int
    dib_size: int
    width: int
    height: int
    top_down: bool
    bits: int
    compression: int
    size_image: int
    ppm: tuple[int, int]
    clr_used: int
    masks: tuple | None
    palette: np.ndarray | None          # (N,4) B G R 0
    palette_offset: int
    stride: int
    channels: int
    problems: list = field(default_factory=list)

    @property
    def row_bytes(self) -> int:
        return self.width * self.channels

    @property
    def row_padding(self) -> int:
        return self.stride - self.row_bytes

    @property
    def pixel_len(self) -> int:
        return self.stride * self.height

    @property
    def pixel_end(self) -> int:
        return self.pixel_offset + self.pixel_len

    @property
    def damaged(self) -> bool:
        return self.pixel_end > self.file_len

    @property
    def trailer_len(self) -> int:
        return max(0, self.file_len - self.pixel_end)

    @property
    def gray_palette(self) -> bool:
        if self.palette is None:
            return False
        p = self.palette.astype(np.int32)
        n = len(p)
        return n > 0 and bool(np.all(p[:, 0] == p[:, 1]) and np.all(p[:, 1] == p[:, 2])
                              and np.array_equal(p[:, 0], np.arange(n) * 255 // max(1, n - 1)))


def is_bmp(head: bytes) -> bool:
    return len(head) >= 18 and head[:2] == b"BM" and struct.unpack_from("<I", head, 14)[0] in DIB_SIZES


def parse_bmp(data: bytes) -> BmpInfo:
    if not is_bmp(data[:18]):
        raise BmpError("缺少 BM 魔数或 DIB 头长度异常")
    file_size_field, _, _, pixel_offset = struct.unpack_from("<IHHI", data, 2)
    dib = struct.unpack_from("<I", data, 14)[0]
    if dib == 12:
        raise Unsupported("OS/2 BITMAPCOREHEADER")
    if len(data) < 14 + dib:
        raise BmpError("DIB 头被截断")
    (w, h, planes, bits, comp, size_image, ppx, ppy, clr_used, _clr_imp) = struct.unpack_from("<iiHHIIiiII", data, 18)
    if w <= 0 or h == 0 or planes != 1:
        raise BmpError(f"非法尺寸 / planes：{w}×{h}，planes={planes}")
    top_down = h < 0
    h = abs(h)
    masks = None
    if comp in (BI_BITFIELDS, BI_ALPHABITFIELDS):
        # 掩码紧跟 40 字节信息头：40 字节头时位于头外，V2+ 头时位于头内，偏移相同
        n = 4 if (comp == BI_ALPHABITFIELDS or dib >= 56) else 3
        masks = struct.unpack_from(f"<{n}I", data, 14 + 40)
    if comp in (BI_RLE8, BI_RLE4):
        raise Unsupported("RLE 压缩 BMP")
    if comp in (BI_JPEG, BI_PNG):
        raise Unsupported("内嵌 JPEG/PNG 的 BMP")
    if bits not in (8, 24, 32):
        raise Unsupported(f"{bits} 位 BMP")
    if comp != BI_RGB and not (bits == 32 and masks and tuple(masks[:3]) == STD_MASKS_32):
        raise Unsupported(f"非标准位掩码 {masks}")

    palette, pal_off = None, 14 + dib
    if comp == BI_BITFIELDS and dib == 40:
        pal_off += 12
    elif comp == BI_ALPHABITFIELDS and dib == 40:
        pal_off += 16
    if bits == 8:
        n = clr_used or 256
        if n > 256 or pal_off + 4 * n > pixel_offset:
            raise BmpError(f"调色板长度 {n} 与像素偏移 {pixel_offset} 冲突")
        palette = np.frombuffer(data, np.uint8, count=4 * n, offset=pal_off).reshape(n, 4).copy()
    channels = bits // 8
    stride = ((w * bits + 31) // 32) * 4
    info = BmpInfo(len(data), file_size_field, pixel_offset, dib, w, h, top_down, bits, comp, size_image,
                   (ppx, ppy), clr_used, masks, palette, pal_off, stride, channels)
    if file_size_field != len(data):
        info.problems.append(f"文件头声明大小 {file_size_field} ≠ 实际 {len(data)}")
    if pixel_offset < pal_off + (0 if palette is None else palette.nbytes):
        raise BmpError("像素偏移落在头部 / 调色板之内")
    return info


def pixel_rows(data: bytes, info: BmpInfo) -> np.ndarray:
    """返回文件中的像素行（含填充），形状 (H, stride)，按文件存储顺序。"""
    return np.frombuffer(data, np.uint8, count=info.pixel_len, offset=info.pixel_offset).reshape(info.height, info.stride)


def pixels(data: bytes, info: BmpInfo) -> np.ndarray:
    """返回自上而下的像素：8 位 (H,W)，24/32 位 (H,W,C)（BGR / BGRA 存储顺序）。"""
    rows = pixel_rows(data, info)[:, : info.row_bytes]
    if not info.top_down:
        rows = rows[::-1]
    return rows.reshape(info.height, info.width) if info.channels == 1 else rows.reshape(info.height, info.width, info.channels)
