"""合成“产线图像”：静态背景 + 产品（平坦区 + 纹理 + 边缘 + 文字 + 高光）+ 传感器噪声。

生成器不在可逆路径上，可以用浮点；但所有随机量都来自固定种子，同一平台重复运行结果一致。
场景值为线性辐亮度，1.0 ≈ 满阱，高光可 > 1（饱和）。
"""
from __future__ import annotations

from dataclasses import dataclass, field

import cv2
import numpy as np

BAYER_PATTERNS = ("RGGB", "BGGR", "GRBG", "GBRG")
# 原始传感器各色响应（未白平衡），绿色最强 —— 这正是 Bayer 伪装图 2×2 周期特征的来源
RAW_GAINS = {"R": 0.55, "G": 1.0, "B": 0.45}


def _noise(rng, h, w, ksize=None, sigma=None):
    n = rng.standard_normal((h, w), dtype=np.float32)
    if ksize:
        n = cv2.blur(n, ksize)
    if sigma:
        n = cv2.GaussianBlur(n, (0, 0), sigma)
    return n / (n.std() + 1e-6)


def _mask(h, w, draw) -> np.ndarray:
    m = np.zeros((h, w), np.uint8)
    draw(m)
    return m.astype(np.float32) / 255.0


def _over(img, color, m):
    img *= (1.0 - m)[..., None]
    img += np.asarray(color, np.float32) * m[..., None]


def illumination(h, w) -> np.ndarray:
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    r2 = ((x - w / 2) / (w / 2)) ** 2 + ((y - h / 2) / (h / 2)) ** 2
    return (1.0 - 0.18 * r2).astype(np.float32)


def background(h, w, rng, texture: float = 0.0) -> np.ndarray:
    """空载背景：深灰传送带，带低频纹理与光照渐晕。返回 (H,W,3)。
    texture > 0 时叠加高频细节（细颗粒 + 12 像素周期的网孔），模拟网带 / 冲孔治具等纹理丰富的背景；
    texture = 0（默认）不额外抽随机数，T1 测试集逐字节不变。"""
    tex = 0.6 * _noise(rng, h, w, sigma=6) + 0.4 * _noise(rng, h, w, ksize=(1, 25))
    base = np.float32(0.22) * (1.0 + 0.06 * tex)
    if texture > 0:
        y, x = np.mgrid[0:h, 0:w]
        mesh = (((x % 12) < 3) | ((y % 12) < 3)).astype(np.float32)
        base = base * (1.0 + texture * (0.6 * _noise(rng, h, w, sigma=0.7) - 1.2 * (mesh - mesh.mean())))
    img = np.stack([base, base, base * 1.05], axis=2)
    return img * illumination(h, w)[..., None]


@dataclass
class Design:
    """一种“产品型号”的几何：同型号每件的几何相同，细纹理和序列号不同。"""
    h: int
    w: int
    seed: int
    scale: float = 1.0          # 产品尺寸比例（1.0 时约占画面 35%）
    poly: np.ndarray = field(init=False)
    holes: list = field(init=False)
    traces: list = field(init=False)
    label: tuple = field(init=False)
    matrix: np.ndarray = field(init=False)
    spec: tuple = field(init=False)

    def __post_init__(self):
        rng = np.random.default_rng(self.seed)
        h, w = self.h, self.w
        cx, cy = w * (0.5 + rng.uniform(-0.03, 0.03)), h * (0.5 + rng.uniform(-0.03, 0.03))
        pw, ph = w * 0.62 * self.scale, h * 0.56 * self.scale
        ang = np.deg2rad(rng.uniform(-4, 4))
        c, s = np.cos(ang), np.sin(ang)
        corners = np.array([[-pw / 2, -ph / 2], [pw / 2, -ph / 2], [pw / 2, ph / 2], [-pw / 2, ph / 2]])
        self.poly = (corners @ np.array([[c, s], [-s, c]]) + [cx, cy]).astype(np.int32)
        x0, y0 = cx - pw / 2, cy - ph / 2
        sc = min(w, h) / 1024
        self.holes = [(int(x0 + pw * fx), int(y0 + ph * fy), max(2, int(rng.uniform(10, 26) * sc)))
                      for fx, fy in rng.uniform(0.08, 0.92, size=(8, 2))]
        self.traces = []
        for _ in range(14):
            pts = [(x0 + pw * rng.uniform(0.05, 0.95), y0 + ph * rng.uniform(0.05, 0.95))]
            for _ in range(3):
                px, py = pts[-1]
                if rng.random() < 0.5:
                    px = x0 + pw * rng.uniform(0.05, 0.95)
                else:
                    py = y0 + ph * rng.uniform(0.05, 0.95)
                pts.append((px, py))
            self.traces.append((np.array(pts, np.int32), max(1, int(rng.uniform(3, 8) * sc))))
        lw, lh = pw * 0.42, ph * 0.22
        self.label = (int(x0 + pw * 0.06), int(y0 + ph * 0.70), int(lw), int(lh))
        self.matrix = rng.random((16, 16)) < 0.5
        self.spec = (int(x0 + pw * 0.75), int(y0 + ph * 0.18), int(pw * 0.16), max(2, int(ph * 0.025)))


def render_product(d: Design, rng, serial: str, defect: bool = False, unit_mix: float = 0.3):
    """返回 (premultiplied RGB, alpha)，产品处于设计位置。"""
    h, w = d.h, d.w
    shared = _noise(np.random.default_rng(d.seed + 1), h, w, ksize=(41, 1), sigma=0.8)
    unit = _noise(rng, h, w, ksize=(41, 1), sigma=0.8)
    brushed = (1 - unit_mix) * shared + unit_mix * unit
    alpha = _mask(h, w, lambda m: cv2.fillPoly(m, [d.poly], 255, cv2.LINE_AA))

    img = np.empty((h, w, 3), np.float32)
    img[:] = (0.20, 0.46, 0.25)                      # 绿色基板
    img *= (1.0 + 0.05 * brushed)[..., None]

    def traces(m):
        for pts, t in d.traces:
            cv2.polylines(m, [pts], False, 255, t, cv2.LINE_AA)

    def pads(m):
        for x, y, r in d.holes:
            cv2.circle(m, (x, y), r + max(2, r // 2), 255, -1, cv2.LINE_AA)

    def holes(m):
        for x, y, r in d.holes:
            cv2.circle(m, (x, y), r, 255, -1, cv2.LINE_AA)

    lx, ly, lw, lh = d.label
    fs = max(0.25, lh / 170)

    def text(m):
        for i, txt in enumerate(("LOT 26A-1107", f"SN {serial}")):
            cv2.putText(m, txt, (lx + lh // 10, ly + int(lh * (0.35 + 0.35 * i))), cv2.FONT_HERSHEY_SIMPLEX,
                        fs, 255, max(1, int(fs * 2)), cv2.LINE_AA)

    _over(img, (0.86, 0.56, 0.30), _mask(h, w, traces))            # 铜走线
    _over(img, (0.90, 0.62, 0.34), _mask(h, w, pads))              # 焊盘
    _over(img, (0.03, 0.03, 0.03), _mask(h, w, holes))             # 通孔
    _over(img, (0.93, 0.93, 0.90), _mask(h, w, lambda m: cv2.rectangle(  # 白色标签 + 文字 + 二维码块
        m, (lx, ly), (lx + lw, ly + lh), 255, -1, cv2.LINE_AA)))
    _over(img, (0.05, 0.05, 0.05), _mask(h, w, text))
    cell = max(1, int(lh * 0.8 / d.matrix.shape[0]))
    mx, my = lx + lw - cell * d.matrix.shape[1] - lh // 10, ly + lh // 10
    mm = np.zeros((h, w), np.float32)
    big = np.kron(d.matrix, np.ones((cell, cell))).astype(np.float32)
    bh, bw = big.shape
    if 0 <= my and my + bh <= h and 0 <= mx and mx + bw <= w:
        mm[my:my + bh, mx:mx + bw] = big
    _over(img, (0.05, 0.05, 0.05), mm)
    sx, sy, sa, sb = d.spec                          # 镜面高光（饱和）
    spec = _mask(h, w, lambda m: cv2.ellipse(m, (sx, sy), (sa, sb), 0, 0, 360, 255, -1, cv2.LINE_AA))
    spec = cv2.GaussianBlur(spec, (0, 0), max(1.0, sb / 2))
    img += (1.6 * spec)[..., None]
    if defect:                                       # NG：划痕 + 污点
        x1, y1 = d.poly.mean(axis=0).astype(int) + rng.integers(-w // 8, w // 8, 2)
        x2, y2 = x1 + rng.integers(-w // 6, w // 6), y1 + rng.integers(-h // 10, h // 10)
        _over(img, (0.75, 0.80, 0.75), _mask(h, w, lambda m: cv2.line(m, (int(x1), int(y1)), (int(x2), int(y2)), 255, 1, cv2.LINE_AA)))
        _over(img, (0.08, 0.10, 0.08), _mask(h, w, lambda m: cv2.circle(m, (int(x2), int(y2)), max(2, w // 200), 255, -1, cv2.LINE_AA)))
    return img * alpha[..., None], alpha


def compose(bg, prod_pm, alpha, dx: float = 0.0, dy: float = 0.0) -> np.ndarray:
    if dx or dy:
        h, w = alpha.shape
        m = np.float32([[1, 0, dx], [0, 1, dy]])
        prod_pm = cv2.warpAffine(prod_pm, m, (w, h), flags=cv2.INTER_LINEAR, borderValue=0)
        alpha = cv2.warpAffine(alpha, m, (w, h), flags=cv2.INTER_LINEAR, borderValue=0)
    illum = illumination(*alpha.shape)[..., None]
    return bg * (1.0 - alpha[..., None]) + prod_pm * illum


def scene(h, w, rng, serial="004213", defect=False) -> np.ndarray:
    d = Design(h, w, int(rng.integers(1 << 31)))
    pm, a = render_product(d, rng, serial, defect)
    return compose(background(h, w, rng), pm, a)


def to_gray(rgb: np.ndarray) -> np.ndarray:
    return rgb @ np.float32([0.299, 0.587, 0.114])


@dataclass
class Sensor:
    bits: int
    full_well: float = 10000.0
    read_e: float = 6.0
    black: float = 0.02
    prnu: np.ndarray | None = None      # 固定模式噪声（同一相机所有帧相同）
    col_offset_e: np.ndarray | None = None
    hot_e: np.ndarray | None = None     # 热像素（固定位置的额外暗电流）

    @classmethod
    def with_fpn(cls, bits, h, w, rng, strength: float = 1.0, hot: float = 0.0, **kw):
        """strength 放大 PRNU 与列偏置；hot 为热像素比例。默认值与 T1 生成完全一致。"""
        prnu = (1.0 + 0.004 * strength * rng.standard_normal((h, w))).astype(np.float32)
        col = (3.0 * strength * rng.standard_normal(w)).astype(np.float32)
        hot_e = None
        if hot > 0:
            hot_e = ((rng.random((h, w)) < hot) * rng.uniform(200, 2000, (h, w))).astype(np.float32)
        return cls(bits, prnu=prnu, col_offset_e=col, hot_e=hot_e, **kw)

    def expose(self, radiance: np.ndarray, rng) -> np.ndarray:
        e = np.clip(radiance, 0, None).astype(np.float32) * self.full_well
        if self.prnu is not None:
            e *= self.prnu if e.ndim == 2 else self.prnu[..., None]
        e += np.sqrt(e) * rng.standard_normal(e.shape, dtype=np.float32)
        e += self.read_e * rng.standard_normal(e.shape, dtype=np.float32)
        if self.col_offset_e is not None:
            e += self.col_offset_e if e.ndim == 2 else self.col_offset_e[:, None]
        if self.hot_e is not None:
            e += self.hot_e if e.ndim == 2 else self.hot_e[..., None]
        maxv = (1 << self.bits) - 1
        dn = self.black * maxv + e * (maxv * (1 - self.black) / self.full_well)
        out = np.clip(np.rint(dn), 0, maxv)
        return out.astype(np.uint8 if self.bits <= 8 else np.uint16)


def mosaic(rgb: np.ndarray, pattern: str) -> np.ndarray:
    """按 Bayer 排列取样，并施加未白平衡的原始响应。返回单通道辐亮度。"""
    h, w = rgb.shape[:2]
    out = np.empty((h, w), np.float32)
    idx = {"R": 0, "G": 1, "B": 2}
    for k, c in enumerate(pattern):
        dy, dx = divmod(k, 2)
        out[dy::2, dx::2] = rgb[dy::2, dx::2, idx[c]] * RAW_GAINS[c]
    return out
