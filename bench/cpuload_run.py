"""T6 编排：标定检测耗时 → 基线 → 各（优先级 × 压缩核数）组合 → 基线复测 → 对比表。"""
from __future__ import annotations

import csv
import json
import time
from pathlib import Path

from . import cpuload as cl
from .compress import cpu_name
from .detect import detect_file, iter_files
from .formats import raw as fraw
from .progress import NULL


def run(root: Path, out: Path, cores: int, load: float, inspect_procs: int | None, compress_cores: list[int],
        priorities: list[str], duration: float, warmup: float, raw_config: Path | None, log=print,
        rep=None) -> dict:
    rep = rep or NULL
    import psutil
    out.mkdir(parents=True, exist_ok=True)
    allowed = sorted(psutil.Process().cpu_affinity())
    cpus = allowed[:cores]
    n_inspect = inspect_procs or len(cpus)
    rules = fraw.load_config(raw_config if raw_config and raw_config.is_file() else None)

    recs = [(p, detect_file(p, root, rules)) for p in iter_files(root)]
    imgs = [(str(p), r) for p, r in recs if r["processable"] and r["class"] in ("bmp", "tiff_uncompressed", "raw", "bayer")]
    gray8 = [p for p, r in imgs if r["container"] == "bmp" and r["channels"] == 1 and r["class"] == "bmp"]
    if not imgs or not gray8:
        raise SystemExit("样本中没有可用于测试的图像")
    img_path = gray8[0]
    files = imgs[:: max(1, len(imgs) // 40)][:40]              # 压缩样本：均匀抽取最多 40 个
    ambient = psutil.cpu_percent(interval=3.0, percpu=True)
    ambient_pct = round(sum(ambient[c] for c in cpus) / len(cpus), 1)
    log(f"[cpuload] 测试前外部负载：所选逻辑核平均 {ambient_pct}%"
        + ("  ⚠ 超过 10%，有其他程序在占用这些核，结果不可靠" if ambient_pct > 10 else ""))
    from concurrent.futures import ProcessPoolExecutor
    with ProcessPoolExecutor(max_workers=1) as ex:      # 子进程里绑核标定，主进程亲和性不动
        t_single = ex.submit(cl.calibrate_on, img_path, cpus[0]).result()
    # 单核标定不代表多进程同时运行的耗时（超线程兄弟核、内存带宽互相拖慢），
    # 所以让全部检测进程连续满负荷跑一小段，用那时的平均单次耗时定周期，负载比例才名副其实
    full = cl.run_phase(img_path, files, cpus, n_inspect, 0.0, [], None, 1.0, 3.0)
    t_job = full["mean_ms"] / 1000
    period = t_job / load
    log(f"[cpuload] 模拟工控机：逻辑核 {cpus[0]}~{cpus[-1]}（{len(cpus)} 个）；检测进程 {n_inspect} 个；"
        f"单次检测 单核空闲 {t_single * 1000:.1f} ms / 全部并发 {t_job * 1000:.1f} ms；"
        f"周期 {period * 1000:.1f} ms（目标负载 {load:.0%}）")
    log(f"[cpuload] 后台压缩样本 {len(files)} 个文件；每组预热 {warmup:g}s + 测量 {duration:g}s")

    # 交替测量（A-B-A）：每组配置前后都测基线，Δ 相对前后两次基线的均值，抵消机器状态漂移
    configs = [(f"后台压缩 {pr} × {c} 核", pr, c) for pr in priorities for c in compress_cores]
    plan = [("基线", None, 0)]
    for cfg in configs:
        plan += [cfg, ("基线", None, 0)]
    rows = []
    for k, (name, pr, c) in enumerate(plan, 1):
        rep.check()
        rep.step("cpuload", k - 1, len(plan), msg=name)
        comp_cpus = cpus[-c:] if c else []
        r = cl.run_phase(img_path, files, cpus, n_inspect, period, comp_cpus, pr, warmup, duration)
        r = {"config": name, "priority": pr or "-", "compress_cores": c, **r}
        rows.append(r)
        log(f"[cpuload] {name:<26} 均值 {r['mean_ms']:7.2f} ms  P95 {r['p95_ms']:7.2f}  P99 {r['p99_ms']:7.2f}"
            f"  超周期 {r['over_period']:4d}  CPU {r['cpu_busy_pct']:5.1f}%"
            + (f"  压缩 {r['compress_MBps']:6.1f} MB/s" if pr else ""))

    for i, r in enumerate(rows):
        if r["priority"] == "-":
            continue
        before, after = rows[i - 1], rows[i + 1]
        for k in ("mean_ms", "p95_ms", "p99_ms"):
            ref_k = (before[k] + after[k]) / 2
            r[f"{k[:-3]}_delta_pct"] = round((r[k] / ref_k - 1) * 100, 1)
        r["over_period_baseline"] = round((before["over_period"] + after["over_period"]) / 2, 1)
    base = [r for r in rows if r["priority"] == "-"]
    means = [b["mean_ms"] for b in base]
    drift = round((max(means) / min(means) - 1) * 100, 1)
    summary = {"machine": {"cpu": cpu_name(), "logical_cpus_total": psutil.cpu_count(),
                           "physical_cpus_total": psutil.cpu_count(logical=False)},
               "params": {"cpus": cpus, "inspect_procs": n_inspect, "target_load": load,
                          "job_ms_single": round(t_single * 1000, 3), "job_ms_concurrent": round(t_job * 1000, 3), "period_ms": round(period * 1000, 3),
                          "duration_s": duration, "warmup_s": warmup, "inspect_image": img_path,
                          "compress_files": len(files), "compress_path": "识别后拆分 → JPEG-LS → 解码 → 重组 → SHA-256"},
               "ambient_busy_pct": ambient_pct, "reliable": ambient_pct <= 10 and drift <= 10,
               "baseline_drift_pct": drift, "rows": rows}
    (out / "cpu_impact.json").write_text(json.dumps(summary, ensure_ascii=False, indent=1), encoding="utf-8")
    with open(out / "cpu_impact.csv", "w", newline="", encoding="utf-8-sig") as f:
        w = csv.DictWriter(f, list(dict.fromkeys(k for r in rows for k in r)))
        w.writeheader()
        w.writerows(rows)
    return summary
