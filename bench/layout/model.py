"""拆分结果的数据模型：原样字节段（骨架）+ 像素平面 + 附加字节 + 配方。

重组只能用这里的内容，不能偷看原文件 —— `detach()` 把它变成与原文件完全无关的副本，
测试与 verify 都在副本上重组，保证“拆得出、拼得回”。
"""
from __future__ import annotations

import copy
from dataclasses import dataclass, field

import numpy as np


@dataclass
class Plane:
    """交给编码器的一个像素平面：2 维 (H,W) 或交错 3 通道 (H,W,3)，uint8 / uint16。"""
    name: str
    data: np.ndarray
    bits: int                 # 有效位数（JPEG-LS MAXVAL / HTJ2K 位深的依据）

    @property
    def nbytes(self) -> int:
        return self.data.nbytes


@dataclass
class Split:
    fmt: str                                   # bmp / tiff / raw
    size: int                                  # 原文件字节数
    sha256: str                                # 原文件 SHA-256
    recipe: dict                               # 可 JSON 序列化
    skeleton: list[tuple[int, bytes]]          # 原样字节段 (偏移, 内容)，按偏移排序、互不重叠
    planes: list[Plane]
    extras: dict[str, bytes] = field(default_factory=dict)   # 非 0 行填充、打包尾字节等

    @property
    def skeleton_bytes(self) -> int:
        return sum(len(b) for _, b in self.skeleton)

    @property
    def extras_bytes(self) -> int:
        return sum(len(b) for b in self.extras.values())

    @property
    def pixel_bytes(self) -> int:
        return sum(p.nbytes for p in self.planes)

    def detach(self) -> "Split":
        """深拷贝：像素转成字节再还原，骨架 / 附加字节复制，切断与原文件缓冲区的一切引用。"""
        planes = [Plane(p.name, np.frombuffer(p.data.tobytes(), p.data.dtype).reshape(p.data.shape).copy(), p.bits)
                  for p in self.planes]
        return Split(self.fmt, self.size, self.sha256, copy.deepcopy(self.recipe),
                     [(o, bytes(b)) for o, b in self.skeleton], planes,
                     {k: bytes(v) for k, v in self.extras.items()})


class LayoutError(ValueError):
    """无法安全拆分：上层回退到 zstd / 原样。"""


def complement(ranges: list[tuple[int, int]], size: int, data: bytes) -> list[tuple[int, bytes]]:
    """ranges：像素段 (偏移, 长度)。返回其余部分的原样字节段。要求像素段互不重叠且在文件内。"""
    rs = sorted((o, n) for o, n in ranges if n > 0)
    out, pos = [], 0
    for o, n in rs:
        if o < pos:
            raise LayoutError(f"像素段重叠或乱序：偏移 {o} < {pos}")
        if o + n > size:
            raise LayoutError(f"像素段越界：{o}+{n} > {size}")
        if o > pos:
            out.append((pos, data[pos:o]))
        pos = o + n
    if pos < size:
        out.append((pos, data[pos:size]))
    return out


def assemble(size: int, skeleton: list[tuple[int, bytes]], pieces: list[tuple[int, bytes]]) -> bytes:
    """把骨架与像素段按偏移拼回原文件；检查覆盖完整、无重叠。"""
    buf = bytearray(size)
    covered = 0
    for o, b in sorted(skeleton + pieces, key=lambda t: t[0]):
        if o < covered:
            raise LayoutError(f"重组时段重叠：偏移 {o}")
        if o > covered:
            raise LayoutError(f"重组时出现空洞：{covered}..{o}")
        buf[o:o + len(b)] = b
        covered = o + len(b)
    if covered != size:
        raise LayoutError(f"重组长度 {covered} ≠ 原文件 {size}")
    return bytes(buf)
