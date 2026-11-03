"""一站式评估：python -m bench scan <样本目录> --out bench/out/<名称>

依次运行 识别（T2）→ 拆分重组（T3）→ 压缩对比（T4）→ 天花板（T5，样本中有序列与空载背景时）
→ CPU 影响（T6，Windows 且指定 --cpuload 时）→ 报告（T7）。各阶段结果放在输出目录的子目录里，
报告可用 `python -m bench report <输出目录>` 换 ROI 参数重新生成而不必重跑。

只读样本目录；所有输出写到输出目录。
"""
from __future__ import annotations

import json
import time
from pathlib import Path

from . import codecs as cx

QUICK_SCHEMES = ["zstd3", "zstd19", "png", "jpegls", "htj2k", "jxl3"]


def run(root: Path, out: Path, *, schemes: list[str] | None, quick: bool, jobs: int, raw_config: Path | None,
        speed_per_group: int, repeat: int, ntfs: bool, frames: str, background: str, ceiling: str,
        cpuload: bool, cpu_cores: int, cpu_load: float, log=print, rep=None,
        speed_schemes: list[str] | None = None) -> dict:
    """rep：进度上报与停止开关（GUI 用）；停止时抛 progress.Cancelled，已完成阶段的结果保留在输出目录。"""
    from . import cpuload as cl
    from .detect import scan_dir, write_outputs
    from .progress import NULL
    rep = rep or NULL
    root = root.resolve()
    if out.resolve().is_relative_to(root):          # 先检查再创建：样本目录只读，不能在里面建任何东西
        raise SystemExit("输出目录不能放在样本目录里（样本目录只读）")
    out.mkdir(parents=True, exist_ok=True)
    t0 = time.perf_counter()
    meta = {"sample_dir": str(root), "started": time.strftime("%Y-%m-%d %H:%M:%S"), "phases": {}, "status": "running"}

    def save_meta():
        (out / "scan_meta.json").write_text(json.dumps(meta, ensure_ascii=False, indent=1), encoding="utf-8")
    save_meta()                                     # 开始时就写：中途停止 / 出错的测试也能在记录里看到和删除

    have_seq = len(list(root.glob(frames))) >= 2 and any(root.glob(background))
    do_ceiling = ceiling == "on" or (ceiling == "auto" and have_seq)
    do_bg = len(list(root.glob(background))) >= 3
    cpu_ok, cpu_note = cl.available() if cpuload else (False, "")
    plan = ["detect", "layout", "compress"] + (["speed"] if speed_per_group > 0 else []) + (["ntfs"] if ntfs else []) \
        + (["ceiling"] if do_ceiling else []) + (["bgpotential"] if do_bg else []) + (["cpuload"] if cpu_ok else [])
    rep.plan(plan)

    def phase(name, key, fn, own_finish=False):
        rep.check()
        t = time.perf_counter()
        log(f"[scan] ===== {name} =====")
        rep.start(key)
        r = fn()
        meta["phases"][name] = round(time.perf_counter() - t, 1)
        if not own_finish:
            rep.finish(key)
        return r

    recs = phase("识别", "detect", lambda: scan_dir(root, raw_config, log=None, rep=rep))
    write_outputs(recs, out / "detect")

    from .layout.verify import run_dir
    phase("拆分重组", "layout", lambda: run_dir(root, out / "layout", raw_config, log=None, rep=rep))

    from .compress_run import print_summary, run as run_compress
    sc = schemes or (QUICK_SCHEMES if quick else cx.ALL_SCHEMES)
    summ = phase("压缩对比", "compress", lambda: run_compress(root, out / "compress", sc, jobs, speed_per_group, repeat,
                                                           0.0, raw_config, ntfs, log=log, rep=rep,
                                                           speed_schemes=speed_schemes), own_finish=True)
    print_summary(summ, log=log)

    if do_ceiling:
        from .ceiling_run import run as run_ceiling
        res = phase("天花板测试", "ceiling", lambda: run_ceiling(root, out / "ceiling", frames, background, jobs,
                                                              raw_config, log=log, rep=rep))
        for line in res["conclusions"]:
            log("  " + line)
    else:
        meta["ceiling_skipped"] = "样本中没有时序序列与空载背景（--frames / --background），跳过天花板测试"
        log(f"[scan] {meta['ceiling_skipped']}")

    if do_bg:
        from . import bgmodel
        r = phase("背景潜力评估", "bgpotential", lambda: bgmodel.run_dir(root, out / "bgpotential", background,
                                                                     frames if have_seq else None, raw_config))
        for line in r["explain"] + [f"结论：{r['verdict']}"]:
            log("  " + line)

    if cpuload:
        if cpu_ok:
            from .cpuload_run import run as run_cpu
            phase("CPU 影响", "cpuload", lambda: run_cpu(root, out / "cpuload", cpu_cores, cpu_load, None, [1, 2, 4],
                                                       ["below_normal", "idle"], 20, 3, raw_config, log=log, rep=rep))
        else:
            meta["cpuload_skipped"] = cpu_note
            log(f"[scan] {cpu_note}")
    else:
        meta["cpuload_skipped"] = "未指定 --cpuload，跳过（该测试约需 5 分钟且要求机器空闲）"

    meta["elapsed_s"] = round(time.perf_counter() - t0, 1)
    meta["status"] = "done"
    save_meta()
    return meta
