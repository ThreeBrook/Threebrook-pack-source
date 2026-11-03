"""TIFF 解析：用 tifffile 读取 IFD、标签与像素块位置，不改动任何字节。"""
from __future__ import annotations

import io
from dataclasses import dataclass, field

import numpy as np
import tifffile

COMPRESSION_NAMES = {1: None, 5: "lzw", 8: "zip", 32946: "zip", 32773: "packbits"}
# 复现式还原可尝试的压缩方式（其余已压缩格式按“不处理”）
REPRODUCIBLE = {"lzw", "zip", "packbits"}


class TiffError(ValueError):
    pass


def is_tiff(head: bytes) -> bool:
    return head[:4] in (b"II*\0", b"MM\0*", b"II+\0", b"MM\0+")


@dataclass
class TiffPage:
    index: int
    width: int
    height: int
    bits: int
    samples: int
    sampleformat: int
    compression: int
    predictor: int
    planar: int
    photometric: int
    orientation: int
    tiled: bool
    tile: tuple | None
    rowsperstrip: int | None
    offsets: tuple
    bytecounts: tuple
    dtype: str

    @property
    def compression_name(self):
        return COMPRESSION_NAMES.get(self.compression, f"other:{self.compression}")


@dataclass
class TiffInfo:
    byteorder: str            # II / MM
    bigtiff: bool
    pages: list[TiffPage]
    tag_codes: set            # 第一页出现的标签号（写图程序指纹）
    software: str | None
    description: str | None
    private_tags: list = field(default_factory=list)   # [(code, name)]

    @property
    def compressions(self) -> set:
        return {p.compression_name for p in self.pages}


def parse_tiff(data: bytes) -> TiffInfo:
    try:
        t = tifffile.TiffFile(io.BytesIO(data))
    except Exception as e:  # tifffile 对坏文件抛多种异常
        raise TiffError(f"TIFF 解析失败：{e}") from e
    with t:
        pages = []
        for i, p in enumerate(t.pages):
            tags = p.tags
            pages.append(TiffPage(
                index=i, width=p.imagewidth, height=p.imagelength, bits=p.bitspersample,
                samples=p.samplesperpixel, sampleformat=int(p.sampleformat), compression=int(p.compression),
                predictor=int(p.predictor), planar=int(p.planarconfig), photometric=int(p.photometric),
                orientation=int(tags[274].value) if 274 in tags else 1, tiled=p.is_tiled,
                tile=(p.tilelength, p.tilewidth) if p.is_tiled else None,
                rowsperstrip=None if p.is_tiled else p.rowsperstrip,
                offsets=tuple(p.dataoffsets), bytecounts=tuple(p.databytecounts), dtype=str(p.dtype)))
        if not pages:
            raise TiffError("TIFF 没有图像页")
        first = t.pages[0].tags
        codes = {tg.code for tg in first.values()}
        priv = sorted({(tg.code, tg.name) for p in t.pages for tg in p.tags.values() if tg.code >= 32768})
        return TiffInfo(
            byteorder="II" if t.byteorder == "<" else "MM", bigtiff=t.is_bigtiff, pages=pages, tag_codes=codes,
            software=str(first[305].value) if 305 in first else None,
            description=str(first[270].value)[:200] if 270 in first else None, private_tags=priv)


def page_pixels(data: bytes, index: int = 0) -> np.ndarray:
    """解码一页像素（本机字节序）。分平面的 RGB 返回 (S,H,W)。"""
    with tifffile.TiffFile(io.BytesIO(data)) as t:
        return t.pages[index].asarray()
