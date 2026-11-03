"""无文件头 RAW：参数配置、同名参数文件、按文件大小推候选、解码。

参数来源优先级（docs/05 4.3）：目录 / 文件名规则配置 → 同名参数文件 → 文件大小反推候选。
配置文件（YAML 或 JSON）：
    raw:
      - match: "station3/*.raw"     # 相对扫描根目录的 glob
        width: 2448
        height: 2048
        pixel_format: Mono12p      # GenICam PFNC 名称
        stride: 3672               # 可选，行跨距字节（仅非打包格式）
        offset: 0                  # 可选，文件头字节数
"""
from __future__ import annotations

import fnmatch
import json
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import yaml

from ..layout import packing

# PFNC 名称 → (每像素字节数 或 None 表示打包, 有效位数, Bayer 排列, 打包格式)
_BAYER = {"RG": "RGGB", "GB": "GBRG", "GR": "GRBG", "BG": "BGGR"}
PIXEL_FORMATS: dict[str, tuple] = {
    "Mono8": (1, 8, None, None), "Mono10": (2, 10, None, None), "Mono12": (2, 12, None, None),
    "Mono14": (2, 14, None, None), "Mono16": (2, 16, None, None),
    "Mono10p": (None, 10, None, "Mono10p"), "Mono12p": (None, 12, None, "Mono12p"),
    "Mono12Packed": (None, 12, None, "Mono12Packed"),
}
for _k, _pat in _BAYER.items():
    PIXEL_FORMATS[f"Bayer{_k}8"] = (1, 8, _pat, None)
    for _b in (10, 12, 14, 16):
        PIXEL_FORMATS[f"Bayer{_k}{_b}"] = (2, _b, _pat, None)
    PIXEL_FORMATS[f"Bayer{_k}10p"] = (None, 10, _pat, "Mono10p")
    PIXEL_FORMATS[f"Bayer{_k}12p"] = (None, 12, _pat, "Mono12p")
    PIXEL_FORMATS[f"Bayer{_k}12Packed"] = (None, 12, _pat, "Mono12Packed")

SIDECAR_EXTS = (".json", ".yaml", ".yml")


class RawError(ValueError):
    pass


@dataclass
class RawParams:
    width: int
    height: int
    pixel_format: str
    stride: int | None = None
    offset: int = 0
    source: str = "rule"          # rule / sidecar / guess
    origin: str = ""              # 匹配的规则或参数文件路径

    def __post_init__(self):
        if self.pixel_format not in PIXEL_FORMATS:
            raise RawError(f"未知像素格式 {self.pixel_format}")
        if self.width <= 0 or self.height <= 0:
            raise RawError("宽高必须为正")

    @property
    def bpp(self):
        return PIXEL_FORMATS[self.pixel_format][0]

    @property
    def packed(self):
        return PIXEL_FORMATS[self.pixel_format][3]

    @property
    def bayer(self):
        return PIXEL_FORMATS[self.pixel_format][2]

    @property
    def bits_stored(self) -> int:
        """容器位数：非打包为 8 / 16，打包为有效位数。"""
        return PIXEL_FORMATS[self.pixel_format][1] if self.packed else self.bpp * 8

    @property
    def row_stride(self) -> int | None:
        if self.packed:
            return None
        return self.stride or self.width * self.bpp

    @property
    def row_padding(self) -> int:
        return 0 if self.packed else self.row_stride - self.width * self.bpp

    @property
    def data_len(self) -> int:
        if self.packed:
            return packing.packed_size(self.packed, self.width * self.height)
        return self.row_stride * self.height

    @classmethod
    def from_dict(cls, d: dict, source: str, origin: str) -> "RawParams":
        try:
            return cls(int(d["width"]), int(d["height"]), str(d["pixel_format"]),
                       int(d["stride"]) if d.get("stride") else None, int(d.get("offset", 0)), source, origin)
        except KeyError as e:
            raise RawError(f"RAW 参数缺少字段 {e}（来源 {origin}）") from e


def load_config(path: Path | None) -> list[dict]:
    if not path or not Path(path).is_file():
        return []
    doc = yaml.safe_load(Path(path).read_text(encoding="utf-8")) or {}
    rules = doc.get("raw", []) if isinstance(doc, dict) else doc
    for r in rules:
        if "match" not in r:
            raise RawError(f"RAW 规则缺少 match：{r}")
    return rules


def match_rule(rules: list[dict], rel: str) -> RawParams | None:
    for r in rules:
        if fnmatch.fnmatch(rel, r["match"]):
            return RawParams.from_dict(r, "rule", r["match"])
    return None


def find_sidecar(path: Path) -> RawParams | None:
    for ext in SIDECAR_EXTS:
        side = path.with_suffix(ext)
        if side != path and side.is_file():
            try:
                d = yaml.safe_load(side.read_text(encoding="utf-8"))
            except (yaml.YAMLError, UnicodeDecodeError):
                continue
            if isinstance(d, dict) and {"width", "height", "pixel_format"} <= set(d):
                return RawParams.from_dict(d, "sidecar", side.name)
    return None


def is_sidecar_of_image(path: Path) -> Path | None:
    """若 path 是某个 RAW 的同名参数文件，返回对应文件。"""
    if path.suffix.lower() not in SIDECAR_EXTS:
        return None
    for sib in path.parent.glob(path.stem + ".*"):
        if sib != path and sib.suffix.lower() not in SIDECAR_EXTS:
            return sib
    return None


def decode(data: bytes, p: RawParams) -> np.ndarray:
    """按参数解码为 (H,W)，8 位返回 uint8，其余 uint16（小端）。"""
    need = p.offset + p.data_len
    if len(data) < need:
        raise RawError(f"文件 {len(data)} 字节 < 参数要求 {need} 字节")
    body = data[p.offset: need]
    if p.packed:
        return packing.unpack(p.packed, body, p.width * p.height).reshape(p.height, p.width)
    rows = np.frombuffer(body, np.uint8).reshape(p.height, p.row_stride)[:, : p.width * p.bpp]
    if p.bpp == 1:
        return rows
    return np.ascontiguousarray(rows).view("<u2").reshape(p.height, p.width)


# ---------------------------------------------------------------- 按文件大小推候选

COMMON_WIDTHS = (640, 720, 752, 768, 800, 1024, 1080, 1280, 1296, 1440, 1600, 1920, 2016, 2048, 2448,
                 2464, 2592, 2840, 3072, 3840, 4024, 4096, 4112, 4480, 5120, 5320, 5472, 6144, 8192)
_GUESS_FORMATS = ("Mono8", "Mono16", "Mono10p", "Mono12p", "Mono12Packed")
IMAGE_COST_MAX = 0.6        # 低于此值才认为“像图像”
BAYER_CORR_MIN = 0.3        # 相邻异色像素相关系数绝对值高于此值，才按 Bayer（2 像素间距）评估
BAYER_LAG_RATIO = 3.0       # 且 1 像素间距的代价至少是 2 像素间距的 3 倍


def _pixel_count(size: int, fmt: str) -> int | None:
    bpp, bits, _, packed = PIXEL_FORMATS[fmt]
    if packed:
        n = size * 8 // bits
        for m in (n, n - 1):
            if m > 0 and packing.packed_size(packed, m) == size:
                return m
        return None
    return size // bpp if size % bpp == 0 else None


def _widths(n: int) -> list[int]:
    out = set()
    i = 1
    while i * i <= n:
        if n % i == 0:
            for w in (i, n // i):
                h = n // w
                if 64 <= w <= 16384 and 0.2 <= w / h <= 5:
                    out.add(w)
        i += 1
    return sorted(out, key=lambda w: (w not in COMMON_WIDTHS, abs(np.log((w / (n / w)) / (4 / 3)))))


def _diff_cost(x: np.ndarray, lag: int) -> float:
    spread = np.abs(x - np.median(x)).mean() + 1e-9
    hd = np.abs(x[:, lag:] - x[:, :-lag]).mean()
    vd = np.abs(x[lag:] - x[:-lag]).mean()
    return float((hd + vd) / (2 * spread))


def _corr(a: np.ndarray, b: np.ndarray) -> float:
    a, b = a.ravel(), b.ravel()
    sa, sb = a.std(), b.std()
    if sa == 0 or sb == 0:
        return 0.0
    return float(((a - a.mean()) * (b - b.mean())).mean() / (sa * sb))


def _cost(img: np.ndarray) -> float:
    """“像不像图像”：相邻像素差相对整体起伏的大小。真图像 ≪ 1，宽度错 / 格式错 / 随机数据 ≈ 1。

    Bayer 马赛克相邻像素颜色不同，只在 2 像素间距（同色相位）上平滑；但 2 像素间距会让
    “半宽错位”“字节交错”也显得平滑。所以只有当 1 像素间距的相邻像素在水平、垂直方向都明显相关
    （真 Bayer：相邻异色像素都随场景变化，彩色场景中可能是负相关），且 2 像素间距明显更平滑时，
    才采用 2 像素间距的结果。"""
    x = img.astype(np.float64)
    best = _diff_cost(x, 1)
    if x.shape[0] > 4 and x.shape[1] > 4:
        h2, w2 = x.shape[0] // 2 * 2, x.shape[1] // 2 * 2
        # 行列两个方向都固定相位（只取 (偶行,偶列) 与其右邻 / 下邻），
        # 否则 (R,G) 与 (G,B) 等不同颜色组合混在一起会互相抵消
        ch = _corr(x[0:h2:2, 0:w2:2], x[0:h2:2, 1:w2:2])
        cv = _corr(x[0:h2:2, 0:w2:2], x[1:h2:2, 0:w2:2])
        c2 = _diff_cost(x, 2)
        # 彩色场景中 R 与 G 可能负相关，取绝对值；马赛克使 1 像素间距显著变差（≥3 倍）才认定是 Bayer
        if min(abs(ch), abs(cv)) > BAYER_CORR_MIN and c2 * BAYER_LAG_RATIO < best:
            best = c2
    return best


def _bytes_alike(img16: np.ndarray) -> bool:
    """真 16 位数据：高字节随场景缓变，低字节含噪声与回绕，比高字节粗糙得多。
    8 位数据（含 Bayer）按 16 位读时，高低字节分别是相邻像素，平滑程度相当。"""
    lo, hi = img16 & 0xFF, img16 >> 8
    if int(hi.max()) - int(hi.min()) <= 1:      # 高字节几乎不变：≤8 位内容放在 16 位容器，不判
        return False
    return _cost(lo) < 2 * _cost(hi)


def size_candidates(data: bytes, top: int = 5, max_widths: int = 40) -> list[dict]:
    size = len(data)
    cands = []
    for fmt in _GUESS_FORMATS:
        n = _pixel_count(size, fmt)
        if not n:
            continue
        for w in _widths(n)[:max_widths]:
            h = n // w
            p = RawParams(w, h, fmt, source="guess")
            rows = min(h, 64)
            y0 = (h - rows) // 2
            if p.packed:
                img = decode(data, p)[y0:y0 + rows]
            else:
                sub = RawParams(w, rows, fmt, offset=y0 * p.row_stride, source="guess")
                img = decode(data, sub)
            c = {"width": w, "height": h, "pixel_format": fmt, "cost": round(_cost(img), 4)}
            if fmt == "Mono16" and _bytes_alike(img):
                c["cost"] = round(c["cost"] + 1.0, 4)
                c["note"] = "高低字节同样平滑，更像 8 位数据被按 16 位误读"
            cands.append(c)
    cands.sort(key=lambda c: c["cost"])
    return cands[:top]

