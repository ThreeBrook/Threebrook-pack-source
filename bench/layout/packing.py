"""GenICam PFNC 打包像素格式的打包 / 解包（纯整数运算，可逆）。

支持：
  Mono10p      —— 连续位流，LSB 优先：4 像素 → 5 字节
  Mono12p      —— 连续位流，LSB 优先：2 像素 → 3 字节
  Mono12Packed —— GigE Vision 旧格式：b0=p0[11:4], b1=p0[3:0]|p1[3:0]<<4, b2=p1[11:4]

像素总数不是组长整数倍时，最后一个字节的剩余位补 0；打包长度 = ceil(n * bits / 8)。
原文件尾部填充位若非 0，重新打包后不会一致 —— 由上层 verify 发现并兜底。
"""
from __future__ import annotations

import numpy as np

PACKED_FORMATS = {
    # 名称: (每像素位数, 每组像素数, 每组字节数)
    "Mono10p": (10, 4, 5),
    "Mono12p": (12, 2, 3),
    "Mono12Packed": (12, 2, 3),
}


def packed_size(fmt: str, n_pixels: int) -> int:
    bits = PACKED_FORMATS[fmt][0]
    return (n_pixels * bits + 7) // 8


def _groups(values: np.ndarray, group: int, bits: int) -> tuple[np.ndarray, int]:
    v = np.ascontiguousarray(values).ravel()
    if v.size and int(v.max()) >= (1 << bits):
        raise ValueError(f"像素值超出 {bits} 位范围")
    n = v.size
    pad = (-n) % group
    v = v.astype(np.uint64)
    if pad:
        v = np.concatenate([v, np.zeros(pad, np.uint64)])
    return v.reshape(-1, group), n


def pack(fmt: str, values: np.ndarray) -> bytes:
    bits, group, nbytes = PACKED_FORMATS[fmt]
    g, n = _groups(values, group, bits)
    if fmt == "Mono12Packed":
        p0, p1 = g[:, 0], g[:, 1]
        out = np.empty((g.shape[0], 3), np.uint8)
        out[:, 0] = (p0 >> 4) & 0xFF
        out[:, 1] = (p0 & 0xF) | ((p1 & 0xF) << 4)
        out[:, 2] = (p1 >> 4) & 0xFF
    else:
        word = np.zeros(g.shape[0], np.uint64)
        for i in range(group):
            word |= g[:, i] << np.uint64(bits * i)
        out = np.empty((g.shape[0], nbytes), np.uint8)
        for k in range(nbytes):
            out[:, k] = (word >> np.uint64(8 * k)) & np.uint64(0xFF)
    return out.ravel()[: packed_size(fmt, n)].tobytes()


def unpack(fmt: str, data: bytes, n_pixels: int) -> np.ndarray:
    """解包为 uint16 一维数组。data 长度不足时抛 ValueError。"""
    bits, group, nbytes = PACKED_FORMATS[fmt]
    need = packed_size(fmt, n_pixels)
    if len(data) < need:
        raise ValueError(f"{fmt}: 需要 {need} 字节，只有 {len(data)}")
    b = np.frombuffer(data, np.uint8, count=need)
    pad = (-need) % nbytes
    if pad:
        b = np.concatenate([b, np.zeros(pad, np.uint8)])
    b = b.reshape(-1, nbytes).astype(np.uint64)
    if fmt == "Mono12Packed":
        p0 = (b[:, 0] << np.uint64(4)) | (b[:, 1] & np.uint64(0xF))
        p1 = (b[:, 2] << np.uint64(4)) | (b[:, 1] >> np.uint64(4))
        out = np.stack([p0, p1], axis=1)
    else:
        word = np.zeros(b.shape[0], np.uint64)
        for k in range(nbytes):
            word |= b[:, k] << np.uint64(8 * k)
        mask = np.uint64((1 << bits) - 1)
        out = np.stack([(word >> np.uint64(bits * i)) & mask for i in range(group)], axis=1)
    return out.ravel()[:n_pixels].astype(np.uint16)
