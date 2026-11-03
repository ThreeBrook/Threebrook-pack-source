"""背景压缩潜力评估：固定相机、背景基本不变时，“背景参考帧”在无损前提下最多还能多省多少。

只需每个工位 10~20 张空载背景（可选再给若干产品帧），回答“这个工位值不值得做背景建模”。

原理：每帧像素 = 静态场景（背景纹理 + 固定模式噪声）+ 时间噪声。背景参考帧能去掉前者，去不掉后者；
无损压缩必须存下时间噪声，其信息量有下限（离散高斯熵 ≈ log2(σ) + 2.05 bit/像素）。
帧内编码（JPEG-LS）对平滑背景本已接近下限，背景纹理越多、固定模式噪声越强、时间噪声越小，背景参考的收益越大。

输出：
  temporal_sigma      时间噪声（DN，按原始位深）
  static_detail       静态高频细节强度（DN；纹理 + 固定模式噪声，已扣除噪声贡献）
  column_fpn / hot_pixels   列条纹强度、热像素数
  intra_bpp           背景帧帧内编码（JPEG-LS）bit/像素
  residual_bpp        留一法：以其余背景帧均值为参考，残差实际编码 bit/像素（与 T5 G3a 相同的标准编码器取小）
  floor_bpp           理论下限：完美参考时只剩时间噪声的熵
  bg_saving           背景区实际可省比例 = 1 − residual / intra；bg_saving_ceiling 为理论上限
  bg_fraction / frame_saving_est   给了产品帧时：背景块比例与全帧收益估计
判读与 T5 一致：<5% 无明显收益，5~15% 有收益，≥15% 显著。
"""
from __future__ import annotations

import math

import cv2
import imagecodecs as ic
import numpy as np

from . import ceiling as ce


def gaussian_entropy_bits(sigma: float) -> float:
    """整数量化的高斯噪声的离散熵（bit）。σ 很小时趋于 0，σ ≳ 1 时 ≈ log2(σ) + 2.05。"""
    if sigma <= 1e-6:
        return 0.0
    k = np.arange(-int(10 * sigma) - 2, int(10 * sigma) + 3)
    # 每个整数格的概率 = 正态在 [k-0.5, k+0.5] 上的积分
    cdf = 0.5 * (1 + np.vectorize(math.erf)((k + 0.5) / (sigma * math.sqrt(2))))
    cdf0 = 0.5 * (1 + np.vectorize(math.erf)((k - 0.5) / (sigma * math.sqrt(2))))
    p = cdf - cdf0
    p = p[p > 0]
    return float(-(p * np.log2(p)).sum())


def zero_order_entropy(a: np.ndarray) -> float:
    _, c = np.unique(a, return_counts=True)
    p = c / c.sum()
    return float(-(p * np.log2(p)).sum())


def _jpegls_bits(a: np.ndarray, bits: int) -> float:
    return len(ic.jpegls_encode(a, level=0, bitspersample=bits)) * 8


def potential(bgs: list[np.ndarray], bits: int, frames: list[np.ndarray] | None = None,
              block: int = 16, max_loo: int = 4) -> dict:
    if len(bgs) < 3:
        raise ValueError("至少需要 3 张空载背景")
    B = np.stack([b.astype(np.int64) for b in bgs])
    n, h, w = B.shape
    npx = h * w
    mean = B.mean(axis=0)
    resid = B - mean
    sigma_t = float(np.sqrt((resid ** 2).sum() / ((n - 1) * npx)))

    # 静态高频细节：均值图的高通（扣除平均后残留噪声 σ²/n 的贡献）
    m32 = mean.astype(np.float32)
    hp = m32 - cv2.GaussianBlur(m32, (0, 0), 2.0)
    var_hp = float(hp.var())
    noise_hp = sigma_t ** 2 / n * 0.9            # 高斯模糊 σ=2 的高通保留了约 90% 的白噪声方差
    static_detail = math.sqrt(max(0.0, var_hp - noise_hp))
    col = hp.mean(axis=0)
    column_fpn = float(max(0.0, col.std() - sigma_t / math.sqrt(n * h)))
    med = cv2.medianBlur(np.clip(mean, 0, 65535).astype(np.uint16 if bits > 8 else np.uint8), 3).astype(np.float64)
    hot_pixels = int((np.abs(mean - med) > max(8.0, 6 * sigma_t)).sum())

    # 留一法实测：参考 = 其余背景帧均值（四舍五入整数）
    intra_b, resid_b, ent_b = [], [], []
    dt = np.uint16 if bits > 8 else np.uint8
    for k in range(min(n, max_loo)):
        cur = B[k]
        ref = ((B.sum(axis=0) - cur) * 2 + (n - 1)) // (2 * (n - 1))
        r = (cur - ref).astype(np.int32)
        intra_b.append(_jpegls_bits(cur.astype(dt), bits))
        st, back, _ = ce.encode_residual(r, bits)
        if not np.array_equal(back, r):
            raise AssertionError("残差往返不一致")
        resid_b.append(len(st) * 8)
        ent_b.append(zero_order_entropy(r) * npx)
    intra_bpp = float(np.mean(intra_b)) / npx
    residual_bpp = float(np.mean(resid_b)) / npx
    # 理论下限：参考帧取无穷多张平均时只剩当前帧的时间噪声
    floor_bpp = gaussian_entropy_bits(sigma_t)
    out = {
        "frames_background": n, "size": [h, w], "bits": bits,
        "temporal_sigma": round(sigma_t, 3), "static_detail": round(static_detail, 3),
        "column_fpn": round(column_fpn, 3), "hot_pixels": hot_pixels,
        "intra_bpp": round(intra_bpp, 3), "residual_bpp": round(residual_bpp, 3),
        "residual_entropy_bpp": round(float(np.mean(ent_b)) / npx, 3), "floor_bpp": round(floor_bpp, 3),
        "bg_saving": round(1 - residual_bpp / intra_bpp, 4),
        "bg_saving_ceiling": round(1 - floor_bpp / intra_bpp, 4),
    }

    if frames:
        ref = np.rint(mean).astype(np.int32)
        fr = []
        for f in frames:
            dy, dx = ce.align(f, ref, 6, bits)
            inter = ce.shifted(ref, dy, dx, 6)
            _, _, frac, _, _ = ce.mode_select(f, inter, block)
            fr.append(1 - frac)
        bgf = float(np.mean(fr))
        out["bg_fraction"] = round(bgf, 3)
        out["frame_saving_est"] = round(bgf * out["bg_saving"], 4)
        out["frame_saving_ceiling"] = round(bgf * out["bg_saving_ceiling"], 4)
    est = out.get("frame_saving_est", out["bg_saving"])
    out["verdict"] = verdict(est)
    out["explain"] = explain(out)
    return out


def verdict(g: float) -> str:
    if g < 0.05:
        return "无明显收益（<5%），不建议为此工位做背景建模"
    if g < 0.15:
        return "有收益（5%~15%），可作为阶段 1 可选项，需权衡参考帧管理代价"
    return "收益显著（≥15%），值得做背景建模"


def explain(o: dict) -> list[str]:
    lines = [f"时间噪声 σ = {o['temporal_sigma']} DN：无损下每像素至少约 {o['floor_bpp']} bit，"
             f"帧内编码实际 {o['intra_bpp']} bit"]
    if o["static_detail"] > max(1.0, o["temporal_sigma"]):
        lines.append(f"静态细节（纹理 + 固定模式噪声）{o['static_detail']} DN，明显强于噪声：背景参考能去掉这部分，是主要收益来源")
    else:
        lines.append(f"静态细节 {o['static_detail']} DN，不比噪声强：背景本身已被帧内编码处理得很好，背景参考空间有限")
    if o["column_fpn"] > 0.3 or o["hot_pixels"] > 0:
        lines.append(f"固定模式噪声：列条纹 {o['column_fpn']} DN，热像素 {o['hot_pixels']} 个（背景参考可消除）")
    lines.append(f"背景区：相对 JPEG-LS 帧内编码实际可省 {o['bg_saving']:.1%}（理论上限 {o['bg_saving_ceiling']:.1%}）")
    if "bg_fraction" in o:
        lines.append(f"背景约占画面 {o['bg_fraction']:.0%} → 全帧估计省 {o['frame_saving_est']:.1%}"
                     f"（上限 {o['frame_saving_ceiling']:.1%}；未计模式图与分区边界开销，略偏乐观）")
    return lines


def run_dir(root, out, background: str, frames: str | None, raw_config=None, max_frames: int = 20) -> dict:
    """从样本目录读取空载背景（与可选产品帧）做评估，结果写 out/bg_potential.json。"""
    import json
    from pathlib import Path

    from .ceiling import gray_split
    from .detect import detect_file
    from .formats import raw as fraw
    root, out = Path(root), Path(out)
    rules = fraw.load_config(raw_config if raw_config and Path(raw_config).is_file() else None)

    def planes(glob):
        arrs, bits = [], 8
        for p in sorted(root.glob(glob))[:max_frames]:
            pl = gray_split(p.read_bytes(), detect_file(p, root, rules)).planes[0]
            bits = pl.data.itemsize * 8
            arrs.append(pl.data)
        return arrs, bits

    bgs, bits = planes(background)
    frs, _ = planes(frames) if frames else ([], bits)
    r = potential(bgs, bits, frs or None)
    r.update(background_glob=background, frames_glob=frames or None, frames_used=len(frs))
    out.mkdir(parents=True, exist_ok=True)
    (out / "bg_potential.json").write_text(json.dumps(r, ensure_ascii=False, indent=1), encoding="utf-8")
    return r
