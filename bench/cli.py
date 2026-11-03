"""命令行入口：python -m bench <子命令> ..."""
from __future__ import annotations

import argparse
import os
import sys
import time
from pathlib import Path


def cmd_synth(a) -> int:
    from .synth.generate import PRESETS, generate

    t0 = time.perf_counter()
    ctx = generate(Path(a.out), a.size, a.seed, a.seq_frames, a.only)
    total = sum(e["size"] for e in ctx.entries)
    print(f"[synth] 完成：{len(ctx.entries)} 个文件，{total / 1e6:.1f} MB，"
          f"用时 {time.perf_counter() - t0:.1f}s → {a.out}")
    return 0


def cmd_detect(a) -> int:
    import json
    from collections import Counter

    from .detect import scan_dir, write_outputs
    from .detect.evaluate import evaluate

    root = Path(a.dir)
    if not root.is_dir():
        print(f"目录不存在：{root}")
        return 2
    cfg = Path(a.raw_config) if a.raw_config else (root / "raw_config.yaml")
    t0 = time.perf_counter()
    recs = scan_dir(root, cfg if cfg.is_file() else None)
    out = Path(a.out)
    write_outputs(recs, out)
    print(f"[detect] {len(recs)} 个文件，用时 {time.perf_counter() - t0:.1f}s → {out}")
    for k, v in Counter(r["class"] for r in recs).most_common():
        print(f"  {k:<18}{v:5d}")
    errs = [r for r in recs if r["error"]]
    if errs:
        print(f"  有错误 / 警告的文件 {len(errs)} 个，见 detect.csv 的 error 列")
    truth = Path(a.truth) if a.truth else root / "manifest.json"
    if truth.is_file():
        ev = evaluate(recs, json.loads(truth.read_text(encoding="utf-8")))
        (out / "detect_eval.json").write_text(json.dumps(ev, ensure_ascii=False, indent=1), encoding="utf-8")
        print(f"[detect] 对照真值：{ev['correct']}/{ev['files']} 正确（{ev['accuracy']:.1%}）")
        for k, v in ev["per_category"].items():
            print(f"  {k:<10}{v['correct']:4d}/{v['total']:<4d}")
        b = ev["bayer_pattern_exact"]
        print(f"  Bayer 完整排列（含 R/B 低置信猜测）命中 {b['correct']}/{b['total']}")
        for c in ev["raw_size_candidates"]:
            print(f"  无配置 RAW {c['path']}：真值排在候选第 {c['rank']} 位（共 {c['candidates']} 个）")
        for m in ev["mismatches"][:30]:
            print(f"  ✗ {m['path']}: " + "; ".join(f"{k} 期望 {w!r} 实得 {g!r}" for k, (w, g) in m["diff"].items()))
        return 0 if ev["correct"] == ev["files"] else 1
    return 0


def cmd_layout(a) -> int:
    from .layout.verify import print_summary, run_dir

    root = Path(a.dir)
    cfg = Path(a.raw_config) if a.raw_config else root / "raw_config.yaml"
    summary, rows = run_dir(root, Path(a.out), cfg)
    print_summary(summary, rows)
    return 1 if summary["mismatch"] else 0


def cmd_compress(a) -> int:
    from . import codecs as cx
    from .compress_run import print_summary, run

    schemes = a.schemes or cx.ALL_SCHEMES
    bad = [s for s in schemes if s not in cx.ALL_SCHEMES]
    if bad:
        print(f"未知方案 {bad}，可选 {cx.ALL_SCHEMES}")
        return 2
    root = Path(a.dir)
    cfg = Path(a.raw_config) if a.raw_config else root / "raw_config.yaml"
    summary = run(root, Path(a.out), schemes, a.jobs, a.speed_per_group, a.repeat, a.min_gain, cfg, not a.no_ntfs)
    print_summary(summary)
    return 0


def cmd_ceiling(a) -> int:
    from .ceiling_run import run

    root = Path(a.dir)
    cfg = Path(a.raw_config) if a.raw_config else root / "raw_config.yaml"
    res = run(root, Path(a.out), a.frames, a.background, a.jobs, cfg, a.block, a.radius)
    print(f"[ceiling] 用时 {res['elapsed_s']:.0f}s → {a.out}   CPU：{res['machine']['cpu']}")
    for name in ("sequence", "bayer"):
        t = res.get(name)
        if not t:
            continue
        print(f"  {name:<9}" + "  ".join(f"{g} {t[g]['ratio']:.2f}" for g in ("G1", "G2", "G3a", "G3b", "G3c") if g in t))
    print("[ceiling] 自动结论：")
    for line in res["conclusions"]:
        print("  " + line)
    return 0


def cmd_cpuload(a) -> int:
    from . import cpuload as cl
    from .cpuload_run import run

    ok, note = cl.available()
    if not ok:
        print(f"[cpuload] {note}")
        return 0
    root = Path(a.dir)
    cfg = Path(a.raw_config) if a.raw_config else root / "raw_config.yaml"
    s = run(root, Path(a.out), a.cores, a.load, a.inspect_procs, a.compress_cores, a.priorities,
            a.duration, a.warmup, cfg)
    print(f"[cpuload] 完成 → {a.out}   CPU：{s['machine']['cpu']}")
    print(f"  测试前外部负载 {s['ambient_busy_pct']}%，各次基线最大差异 {s['baseline_drift_pct']}%"
          + ("" if s["reliable"] else "  ⚠ 机器不安静（外部负载 >10% 或基线差异 >10%），结果仅供参考"))
    print(f"  {'配置（Δ 相对前后两次基线）':<26}{'均值Δ':>8}{'P95Δ':>8}{'P99Δ':>8}{'超周期':>7}{'基线超周期':>9}{'压缩MB/s':>10}")
    for r in s["rows"]:
        if r["priority"] == "-":
            continue
        print(f"  {r['config']:<26}{r['mean_delta_pct']:>+7.1f}%{r['p95_delta_pct']:>+7.1f}%{r['p99_delta_pct']:>+7.1f}%"
              f"{r['over_period']:>7}{r['over_period_baseline']:>9}{r['compress_MBps']:>10.1f}")
    return 0


def cmd_bgpotential(a) -> int:
    from . import bgmodel

    root = Path(a.dir)
    cfg = Path(a.raw_config) if a.raw_config else root / "raw_config.yaml"
    r = bgmodel.run_dir(root, Path(a.out), a.background, a.frames or None, cfg, a.max_frames)
    print(f"[bgpotential] 空载背景 {r['frames_background']} 张，产品帧 {r['frames_used']} 张")
    for line in r["explain"]:
        print("  " + line)
    print(f"  结论：{r['verdict']}")
    return 0


def cmd_bgsweep(a) -> int:
    from . import bgsweep

    s = bgsweep.run(Path(a.out), a.jobs)
    rows = s["rows"]
    print(f"[bgsweep] 完成 {s['configs']} 个配置，用时 {s['elapsed_s']}s → {a.out}")
    print(f"  估计与实测的绝对误差：平均 {s['estimator_abs_error']['mean']:.1%}，最大 {s['estimator_abs_error']['max']:.1%}")
    for fpn in s["grid"]["fpn"]:
        for prod in s["grid"]["product"]:
            for line in bgsweep.pivot(rows, fpn, prod):
                print(line)
    return 0


def cmd_gui(a) -> int:
    from .gui.app import main as gui_main
    return gui_main(debug=a.debug)


def _roi_args(parser):
    r = parser.add_argument_group("ROI 代入（工控机本地硬盘；不提供则用示例参数并在报告中注明）")
    r.add_argument("--daily-gb", type=float, help="每天出图量（GB）")
    r.add_argument("--daily-images", type=float, help="每天出图张数（与 --image-mb 一起代替 --daily-gb）")
    r.add_argument("--image-mb", type=float, help="平均单张大小（MB）")
    r.add_argument("--debug-daily-gb", type=float, help="调试期每天出图量（GB），默认同正常期")
    r.add_argument("--disk-tb", type=float, help="图像盘容量（TB）")
    r.add_argument("--retention-days", type=float, help="招标要求保留天数")
    r.add_argument("--reserve", type=float, default=0.10, help="硬盘预留空间比例（默认 0.10）")
    r.add_argument("--roi-scheme", default="jpegls", help="ROI 采用哪个方案的实测总体压缩比（默认 jpegls）")


def cmd_scan(a) -> int:
    from .report import build, roi_from_args
    from .scan import run

    root, out = Path(a.dir), Path(a.out)
    cfg = Path(a.raw_config) if a.raw_config else root / "raw_config.yaml"
    run(root, out, schemes=a.schemes, quick=a.quick, jobs=a.jobs, raw_config=cfg if cfg.is_file() else None,
        speed_per_group=a.speed_per_group, repeat=a.repeat, ntfs=not a.no_ntfs, frames=a.frames,
        background=a.background, ceiling=a.ceiling, cpuload=a.cpuload, cpu_cores=a.cpu_cores, cpu_load=a.cpu_load)
    data = build(out, roi_from_args(a), a.roi_scheme)
    print(f"[scan] 报告：{out / 'report.html'}（另有 details.csv、summary.json）")
    print(f"  推荐方案 {data['recommended']['label']} 总体压缩比 {data['recommended']['ratio']:.2f}")
    for line in (data["roi"] or {}).get("text", []):
        print("  " + line)
    return 0


def cmd_report(a) -> int:
    from .report import build, roi_from_args

    out = Path(a.outdir)
    data = build(out, roi_from_args(a), a.roi_scheme)
    print(f"[report] {out / 'report.html'}（另有 details.csv、summary.json）"
          + ("；ROI 使用示例参数" if data["roi"]["example"] else ""))
    return 0


def main(argv=None) -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace", line_buffering=True)
    p = argparse.ArgumentParser(prog="python -m bench", description="IIS 阶段 0 评估工具")
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("synth", help="T1：生成合成测试数据集")
    s.add_argument("--out", default="bench/out/synth", help="输出目录（必须为空或不存在）")
    s.add_argument("--size", default="medium", choices=["small", "medium", "large"], help="尺寸预设")
    s.add_argument("--seed", type=int, default=2026, help="随机种子")
    s.add_argument("--seq-frames", type=int, default=200, help="时序序列帧数")
    s.add_argument("--only", nargs="+", choices=["bmp", "tiff", "raw", "bayer", "sequence", "other"],
                   help="只生成指定类别")
    s.set_defaults(func=cmd_synth)

    d = sub.add_parser("detect", help="T2：格式识别（只读样本目录）")
    d.add_argument("dir", help="样本目录")
    d.add_argument("--out", default="bench/out/detect", help="输出目录")
    d.add_argument("--raw-config", help="RAW 参数配置（YAML/JSON），默认取 <样本目录>/raw_config.yaml")
    d.add_argument("--truth", help="真值 manifest.json（合成数据自带），给出识别正确率")
    d.set_defaults(func=cmd_detect)

    lo = sub.add_parser("layout", help="T3：拆分再重组（不压缩），逐文件 SHA-256 比对，统计复现式还原")
    lo.add_argument("dir", help="样本目录")
    lo.add_argument("--out", default="bench/out/layout", help="输出目录")
    lo.add_argument("--raw-config", help="RAW 参数配置，默认取 <样本目录>/raw_config.yaml")
    lo.set_defaults(func=cmd_layout)

    c = sub.add_parser("compress", help="T4：多方案压缩对比（往返 SHA-256 校验、体积算全、单核测速、NTFS 对照）")
    c.add_argument("dir", help="样本目录")
    c.add_argument("--out", default="bench/out/compress", help="输出目录")
    c.add_argument("--raw-config", help="RAW 参数配置，默认取 <样本目录>/raw_config.yaml")
    c.add_argument("--schemes", nargs="+", help="方案，默认全部：zstd3 zstd19 png jpegls htj2k jxl3 jxl7 jxl9")
    c.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2 - 2),
                   help="压缩比阶段并行进程数（每进程单线程）")
    c.add_argument("--speed-per-group", type=int, default=1, help="测速时每个格式组抽几个文件（0 = 不测速）")
    c.add_argument("--repeat", type=int, default=3, help="测速重复次数（预热后取中位数）")
    c.add_argument("--min-gain", type=float, default=0.0, help="收益低于此比例则原样存储（0.05 = 5%%）")
    c.add_argument("--no-ntfs", action="store_true", help="跳过 NTFS 压缩对照")
    c.set_defaults(func=cmd_compress)

    g = sub.add_parser("ceiling", help="T5：天花板测试 G1 / G2 / G3a / G3b / G3c 与自动结论")
    g.add_argument("dir", help="样本目录（含时序序列与空载背景；其中的 Bayer 文件用于 G3c）")
    g.add_argument("--frames", default="sequence/frame_*", help="序列帧 glob（相对样本目录，按文件名排序）")
    g.add_argument("--background", default="sequence/background_*", help="空载背景 glob")
    g.add_argument("--out", default="bench/out/ceiling", help="输出目录")
    g.add_argument("--raw-config", help="RAW 参数配置，默认取 <样本目录>/raw_config.yaml")
    g.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2 - 2), help="并行进程数")
    g.add_argument("--block", type=int, default=16, help="G3b 运动补偿块大小")
    g.add_argument("--radius", type=int, default=4, help="G3b 运动搜索半径（像素）")
    g.set_defaults(func=cmd_ceiling)

    u = sub.add_parser("cpuload", help="T6：CPU 影响测试（仅 Windows）：后台压缩对模拟检测耗时的影响")
    u.add_argument("dir", help="样本目录（取其中的图像作为检测输入与压缩样本）")
    u.add_argument("--out", default="bench/out/cpuload", help="输出目录")
    u.add_argument("--raw-config", help="RAW 参数配置，默认取 <样本目录>/raw_config.yaml")
    u.add_argument("--cores", type=int, default=16, help="模拟工控机使用的逻辑核数（默认 16 ≈ 8 核 16 线程 i7）")
    u.add_argument("--load", type=float, default=0.6, help="模拟检测负载比例（0~1）")
    u.add_argument("--inspect-procs", type=int, help="检测进程数，默认等于 --cores")
    u.add_argument("--compress-cores", type=int, nargs="+", default=[1, 2, 4], help="后台压缩占用的逻辑核数（可多组）")
    u.add_argument("--priorities", nargs="+", default=["below_normal", "idle"],
                   choices=["normal", "below_normal", "idle"], help="后台压缩优先级（可多组）")
    u.add_argument("--duration", type=float, default=20, help="每组测量秒数")
    u.add_argument("--warmup", type=float, default=3, help="每组预热秒数")
    u.set_defaults(func=cmd_cpuload)

    sc = sub.add_parser("scan", help="一站式评估：识别 → 拆分重组 → 压缩对比 → 天花板 → (CPU 影响) → 报告")
    sc.add_argument("dir", help="样本目录（只读）")
    sc.add_argument("--out", default="bench/out/scan", help="输出目录（不能在样本目录内）")
    sc.add_argument("--raw-config", help="RAW 参数配置，默认取 <样本目录>/raw_config.yaml")
    sc.add_argument("--schemes", nargs="+", help="压缩方案，默认全部")
    sc.add_argument("--quick", action="store_true", help="快速模式：跳过 JPEG XL e7 / e9")
    sc.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2 - 2), help="并行进程数")
    sc.add_argument("--speed-per-group", type=int, default=1, help="测速时每个格式组抽几个文件（0 = 不测速）")
    sc.add_argument("--repeat", type=int, default=3, help="测速重复次数")
    sc.add_argument("--no-ntfs", action="store_true", help="跳过 NTFS 压缩对照")
    sc.add_argument("--frames", default="sequence/frame_*", help="天花板测试：序列帧 glob")
    sc.add_argument("--background", default="sequence/background_*", help="天花板测试：空载背景 glob")
    sc.add_argument("--ceiling", choices=["auto", "on", "off"], default="auto", help="天花板测试（auto：有序列才做）")
    sc.add_argument("--cpuload", action="store_true", help="运行 CPU 影响测试（仅 Windows，约 5 分钟，需机器空闲）")
    sc.add_argument("--cpu-cores", type=int, default=16, help="CPU 影响测试：模拟工控机的逻辑核数")
    sc.add_argument("--cpu-load", type=float, default=0.6, help="CPU 影响测试：模拟检测负载比例")
    _roi_args(sc)
    sc.set_defaults(func=cmd_scan)

    bp = sub.add_parser("bgpotential", help="背景压缩潜力评估：用空载背景估计背景参考帧在无损前提下最多能多省多少")
    bp.add_argument("dir", help="样本目录（只读）")
    bp.add_argument("--background", default="sequence/background_*", help="空载背景 glob（建议 10~20 张）")
    bp.add_argument("--frames", default="sequence/frame_*", help="产品帧 glob（可选，用于估计背景占比；空字符串表示不用）")
    bp.add_argument("--max-frames", type=int, default=20, help="每类最多读取几张")
    bp.add_argument("--out", default="bench/out/bgpotential", help="输出目录")
    bp.add_argument("--raw-config", help="RAW 参数配置，默认取 <样本目录>/raw_config.yaml")
    bp.set_defaults(func=cmd_bgpotential)

    bs = sub.add_parser("bgsweep", help="背景参考收益的敏感性扫描（合成数据：噪声 × 纹理 × FPN × 产品比例）")
    bs.add_argument("--out", default="bench/out/bgsweep", help="输出目录")
    bs.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2 - 2), help="并行进程数")
    bs.set_defaults(func=cmd_bgsweep)

    gu = sub.add_parser("gui", help="图形界面：快速测试、测试记录、容量与 ROI、设置")
    gu.add_argument("--debug", action="store_true", help="打开 WebView 开发者工具")
    gu.set_defaults(func=cmd_gui)

    rp = sub.add_parser("report", help="T7：由 scan 输出目录重新生成报告（可换 ROI 参数，不重跑测试）")
    rp.add_argument("outdir", help="scan 的输出目录")
    _roi_args(rp)
    rp.set_defaults(func=cmd_report)

    a = p.parse_args(argv)
    if a.cmd != "synth" and getattr(a, "dir", None) and getattr(a, "out", None):
        if Path(a.out).resolve().is_relative_to(Path(a.dir).resolve()):   # 样本目录只读
            print("输出目录不能放在样本目录里（样本目录只读）")
            return 2
    return a.func(a)
