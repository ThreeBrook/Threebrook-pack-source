"""背景参考帧（G3a）收益的敏感性扫描：在合成数据上系统改变
  时间噪声 σ、背景纹理强度、固定模式噪声强度、产品占画面比例，
实测“背景参考 + 逐块模式选择 + 分区编码”相对 JPEG-LS 帧内编码省多少，并与 bgmodel 的估计对照。

每个配置：8 张空载背景 + 若干产品帧（同型号、轻微位移、独立噪声），512×384 8 位灰度。
所有 G3a 结果都逐像素往返校验（还原与原帧完全一致才计入）。
"""
from __future__ import annotations

import csv
import itertools
import json
import time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import imagecodecs as ic
import numpy as np

from . import bgmodel
from . import ceiling as ce
from .synth import scene as sc

GRID = {
    "sigma": [0.3, 0.6, 1.2, 2.5],      # 目标时间噪声（DN，8 位，场景亮度约 0.25 处）
    "texture": [0.0, 0.05, 0.15],       # 背景高频纹理强度（0 = 平滑传送带）
    "fpn": [1.0, 5.0],                  # 固定模式噪声倍数（5 时另加 0.1% 热像素）
    "product": [0.5, 1.0],              # 产品尺寸比例（0.5 ≈ 占画面 9%，1.0 ≈ 35%）
}
REF_AMORTIZE = 200                      # 参考帧体积按 200 帧分摊


def gen_sequence(cfg: dict, h: int, w: int, n_bg: int, n_frames: int, seed: int):
    rng = np.random.default_rng([seed, int(cfg["sigma"] * 100), int(cfg["texture"] * 1000), int(cfg["fpn"] * 10),
                                 int(cfg["product"] * 10)])
    full_well = (125.0 / cfg["sigma"]) ** 2
    sensor = sc.Sensor.with_fpn(8, h, w, rng, strength=cfg["fpn"], hot=0.001 if cfg["fpn"] > 1 else 0.0,
                                full_well=full_well, read_e=min(6.0, 0.3 * np.sqrt(full_well)))
    bg = sc.background(h, w, rng, texture=cfg["texture"])
    bgs = [sensor.expose(sc.to_gray(bg), rng) for _ in range(n_bg)]
    design = sc.Design(h, w, int(rng.integers(1 << 31)), scale=cfg["product"])
    frames, dx, dy = [], 0.0, 0.0
    for i in range(n_frames):
        dx = float(np.clip(dx + rng.normal(0, 0.6), -4, 4))
        dy = float(np.clip(dy + rng.normal(0, 0.6), -4, 4))
        pm, a = sc.render_product(design, rng, f"{1000 + i}")
        frames.append(sensor.expose(sc.to_gray(sc.compose(bg, pm, a, dx, dy)), rng))
    return bgs, frames


def run_config(cfg: dict, h: int = 384, w: int = 512, n_bg: int = 8, n_frames: int = 6, seed: int = 7,
               block: int = 16) -> dict:
    t0 = time.perf_counter()
    bgs, frames = gen_sequence(cfg, h, w, n_bg, n_frames, seed)
    B = np.stack([b.astype(np.int64) for b in bgs])
    ref = ((B.sum(axis=0) * 2 + n_bg) // (2 * n_bg)).astype(np.int32)      # 背景整数均值（四舍五入）
    ref_bytes = len(ic.jpegls_encode(ref.astype(np.uint8), level=0))
    intra = g3a = 0
    variants = []
    for f in frames:
        intra += len(ic.jpegls_encode(f, level=0))
        dy, dx = ce.align(f, ref, 6, 8)
        inter = ce.shifted(ref, dy, dx, 6)
        cp = ce.code_predicted(f, inter, block, 8)
        if not np.array_equal(cp["recon"], f.astype(np.int32)):
            raise AssertionError(f"G3a 往返不一致：{cfg}")
        g3a += sum(cp["lens"]) + 2 + ref_bytes / REF_AMORTIZE
        variants.append(cp["variant"])
    est = bgmodel.potential(bgs, 8, frames, block)
    return {**cfg, "temporal_sigma": est["temporal_sigma"], "static_detail": est["static_detail"],
            "bg_fraction": est["bg_fraction"], "intra_bpp": round(intra * 8 / (len(frames) * h * w), 3),
            "g3a_bpp": round(g3a * 8 / (len(frames) * h * w), 3), "measured_saving": round(1 - g3a / intra, 4),
            "estimated_saving": est["frame_saving_est"], "ceiling_saving": est["frame_saving_ceiling"],
            "bg_saving": est["bg_saving"], "verdict": bgmodel.verdict(1 - g3a / intra),
            "regions_frames": variants.count("regions"), "elapsed_s": round(time.perf_counter() - t0, 1)}


def run(out: Path, jobs: int, grid: dict | None = None, log=print) -> dict:
    grid = grid or GRID
    keys = list(grid)
    cfgs = [dict(zip(keys, v)) for v in itertools.product(*(grid[k] for k in keys))]
    log(f"[bgsweep] {len(cfgs)} 个配置，并行 {jobs} 进程")
    t0 = time.perf_counter()
    with ProcessPoolExecutor(max_workers=jobs) as ex:
        rows = list(ex.map(run_config, cfgs))
    out.mkdir(parents=True, exist_ok=True)
    with open(out / "bg_sweep.csv", "w", newline="", encoding="utf-8-sig") as f:
        wr = csv.DictWriter(f, list(rows[0]))
        wr.writeheader()
        wr.writerows(rows)
    err = [abs(r["estimated_saving"] - r["measured_saving"]) for r in rows]
    summary = {"grid": grid, "configs": len(rows), "elapsed_s": round(time.perf_counter() - t0, 1),
               "estimator_abs_error": {"mean": round(float(np.mean(err)), 4), "max": round(float(np.max(err)), 4)},
               "rows": rows}
    (out / "bg_sweep.json").write_text(json.dumps(summary, ensure_ascii=False, indent=1), encoding="utf-8")
    return summary


def pivot(rows: list[dict], fpn: float, product: float) -> list[str]:
    """σ × 纹理 的实测收益表（固定 FPN 与产品比例）。"""
    sig = sorted({r["sigma"] for r in rows})
    tex = sorted({r["texture"] for r in rows})
    lines = [f"  FPN×{fpn:g}，产品比例 {product:g}：行 = 时间噪声 σ（实测），列 = 背景纹理强度；值 = 实测 / 估计"]
    lines.append("    " + f"{'σ':>10}" + "".join(f"{'纹理 ' + format(t, 'g'):>18}" for t in tex))
    for s in sig:
        cells = []
        for t in tex:
            r = next(x for x in rows if x["sigma"] == s and x["texture"] == t and x["fpn"] == fpn and x["product"] == product)
            m, e = (0.0 if abs(v) < 0.0005 else v for v in (r["measured_saving"], r["estimated_saving"]))
            cells.append(f"{m:>+8.1%} / {e:>+6.1%}")
        meas = next(x for x in rows if x["sigma"] == s and x["fpn"] == fpn and x["product"] == product)["temporal_sigma"]
        lines.append("    " + f"{format(s, 'g') + '（' + format(meas, 'g') + '）':>10}" + "".join(f"{c:>18}" for c in cells))
    return lines
