"""可逆版面变换（纯整数运算）。每个正变换都有对应的逆变换，由往返测试保证。"""
from __future__ import annotations

import numpy as np

from .model import LayoutError, Plane

BAYER_PHASES = ((0, 0), (0, 1), (1, 0), (1, 1))


def split_padding(rows: np.ndarray, row_bytes: int) -> tuple[np.ndarray, bytes | None]:
    """rows：(H, stride) 字节。返回 (像素部分, 填充字节)；填充全 0 时返回 None（只记标志）。"""
    pix = rows[:, :row_bytes]
    pad = rows[:, row_bytes:]
    if pad.size == 0 or not pad.any():
        return pix, None
    return pix, pad.tobytes()


def join_padding(pix: np.ndarray, stride: int, pad: bytes | None) -> np.ndarray:
    h, rb = pix.shape
    out = np.zeros((h, stride), np.uint8)
    out[:, :rb] = pix
    if pad is not None:
        out[:, rb:] = np.frombuffer(pad, np.uint8).reshape(h, stride - rb)
    return out


def to_native(raw: np.ndarray, bps: int, byteorder: str) -> np.ndarray:
    """文件字节 → 本机整数。raw 为 uint8，最后一维是字节。"""
    if bps == 1:
        return raw
    dt = np.dtype("<u2" if byteorder == "II" else ">u2")
    return np.ascontiguousarray(raw).view(dt).astype(np.uint16)


def to_file(values: np.ndarray, bps: int, byteorder: str) -> np.ndarray:
    if bps == 1:
        return values.astype(np.uint8)
    dt = np.dtype("<u2" if byteorder == "II" else ">u2")
    return np.ascontiguousarray(values.astype(dt)).view(np.uint8)


def shift_down(a: np.ndarray, shift: int) -> np.ndarray:
    if not shift:
        return a
    if (a & ((1 << shift) - 1)).any():
        raise LayoutError(f"低 {shift} 位不全为 0，不能右移")
    return a >> shift


def shift_up(a: np.ndarray, shift: int) -> np.ndarray:
    return (a << shift).astype(a.dtype) if shift else a


def bayer_split(a: np.ndarray) -> list[np.ndarray]:
    """拆成 4 个相位平面 p00 p01 p10 p11（奇数宽高时各平面尺寸不同，边界行列自然落在对应相位）。"""
    return [np.ascontiguousarray(a[dy::2, dx::2]) for dy, dx in BAYER_PHASES]


def bayer_merge(planes: list[np.ndarray], shape: tuple[int, int]) -> np.ndarray:
    out = np.empty(shape, planes[0].dtype)
    for (dy, dx), p in zip(BAYER_PHASES, planes):
        if out[dy::2, dx::2].shape != p.shape:
            raise LayoutError(f"Bayer 相位 {dy}{dx} 尺寸 {p.shape} 与原图不符")
        out[dy::2, dx::2] = p
    return out


def delta_encode(a: np.ndarray, axis: int) -> np.ndarray:
    """TIFF Predictor=2（水平差分），按整数模运算回绕。"""
    d = a.copy()
    sl_hi = [slice(None)] * a.ndim
    sl_lo = [slice(None)] * a.ndim
    sl_hi[axis], sl_lo[axis] = slice(1, None), slice(None, -1)
    d[tuple(sl_hi)] = a[tuple(sl_hi)] - a[tuple(sl_lo)]
    return d


def delta_decode(d: np.ndarray, axis: int) -> np.ndarray:
    return np.cumsum(d, axis=axis, dtype=d.dtype)


def eff_bits(a: np.ndarray) -> int:
    return max(1, int(a.max()).bit_length()) if a.size else 1


def gray_to_planes(name: str, a: np.ndarray, shift: int = 0, bayer: bool = False):
    """单通道图像 → 平面列表 + 配方片段（移位 → Bayer 拆分；逆序还原）。"""
    v = shift_down(a, shift)
    rec = {"name": name, "shape": list(a.shape), "dtype": str(a.dtype), "shift": shift, "bayer": bayer}
    if bayer:
        planes = [Plane(f"{name}.p{dy}{dx}", p, eff_bits(p)) for (dy, dx), p in zip(BAYER_PHASES, bayer_split(v))]
    else:
        planes = [Plane(name, np.ascontiguousarray(v), eff_bits(v))]
    return planes, rec


def planes_to_gray(planes: dict, rec: dict) -> np.ndarray:
    name, shape = rec["name"], tuple(rec["shape"])
    if rec["bayer"]:
        v = bayer_merge([planes[f"{name}.p{dy}{dx}"] for dy, dx in BAYER_PHASES], shape)
    else:
        v = planes[name]
    if v.shape != shape:
        raise LayoutError(f"平面 {name} 尺寸 {v.shape} ≠ {shape}")
    return shift_up(v.astype(rec["dtype"]), rec["shift"])
