"""T4 编排：识别 → ① 并行压缩比与校验 → ② 单核测速 → ③ NTFS 对照 → 汇总输出。"""
from __future__ import annotations

import csv
import json
import os
import platform
import time
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

from . import codecs as cx
from . import compress as cp
from . import ntfs
from .detect import detect_file, iter_files
from .formats import raw as fraw
from .progress import NULL, Cancelled, kill_pool


def _write_csv(path: Path, rows: list[dict]):
    if not rows:
        return
    with open(path, "w", newline="", encoding="utf-8-sig") as f:
        w = csv.DictWriter(f, list(rows[0]))
        w.writeheader()
        w.writerows(rows)


def run(root: Path, out: Path, schemes: list[str], jobs: int, speed_per_group: int, repeat: int,
        min_gain: float, raw_config: Path | None, do_ntfs: bool, log=print, rep=None,
        speed_schemes: list[str] | None = None) -> dict:
    """speed_schemes：只给这些方案测速（默认全部）。"""
    rep = rep or NULL
    out.mkdir(parents=True, exist_ok=True)
    rules = fraw.load_config(raw_config if raw_config and raw_config.is_file() else None)
    t_all = time.perf_counter()
    files = list(iter_files(root))
    recs = [detect_file(p, root, rules) for p in files]
    abs_of = {r["path"]: str(p) for r, p in zip(recs, files)}
    log(f"[compress] 识别 {len(recs)} 个文件；方案 {schemes}；并行 {jobs} 进程（每进程单线程）")

    # ① 压缩比 + 往返校验（全部文件）
    rows: list[dict] = []
    errors: list[dict] = []
    t0 = time.perf_counter()
    live = {sc: [0, 0] for sc in schemes}          # 已完成文件的 原字节 / 存储字节（实时结果）
    ref_sc = next((s for s in ("jpegls", "htj2k", "png") if s in schemes), schemes[0])
    verified = mismatch = 0
    ex = ProcessPoolExecutor(max_workers=max(1, min(jobs, len(recs))))      # 文件少时不多开进程
    try:
        futs = {ex.submit(cp.evaluate_file, abs_of[r["path"]], r, schemes, min_gain): r["path"] for r in recs}
        for i, f in enumerate(as_completed(futs), 1):
            try:
                res = f.result()
                rows += res
                for r in res:
                    if r["variant"] in ("default", "file"):
                        live[r["scheme"]][0] += r["orig"]
                        live[r["scheme"]][1] += r["stored"]
                    bad = r["status"] == "mismatch" or "SHA-256" in (r.get("reason") or "")
                    mismatch += bad
                    verified += r["scheme"] == ref_sc and r["status"] == "ok" and r["variant"] == "default"
            except Exception as e:      # 不中断：整文件失败只记录
                errors.append({"path": futs[f], "error": repr(e)})
            if i % 25 == 0 or i == len(futs):
                log(f"[compress] 压缩比与校验 {i}/{len(futs)}，{time.perf_counter() - t0:.0f}s")
            rep.step("compress", i, len(futs), msg=futs[f].split("/")[-1], verified=verified, mismatch=mismatch,
                     live={k: round(o / s, 4) for k, (o, s) in live.items() if s},
                     live_bytes={k: [o, s] for k, (o, s) in live.items() if s})
            rep.check()
    except Cancelled:
        kill_pool(ex)
        raise
    ex.shutdown(wait=True)
    rep.finish("compress")
    order = {r["path"]: i for i, r in enumerate(recs)}
    rows.sort(key=lambda r: (order[r["path"]], schemes.index(r["scheme"]), r["variant"]))
    _write_csv(out / "details.csv", rows)

    # ② 单核测速（抽样）
    speed: list[dict] = []
    pin = None
    if speed_per_group > 0:
        sample = cp.speed_sample(recs, speed_per_group)
        # 专用子进程测速：在子进程内绑核，主进程（及之后各阶段的子进程）亲和性不受影响
        ex = ProcessPoolExecutor(max_workers=1)
        sp_schemes = [s for s in schemes if not speed_schemes or s in speed_schemes]
        total, k = len(sample) * len(sp_schemes), 0
        rep.start("speed", total)
        try:
            pin = ex.submit(cp.pin_self).result()
            log(f"[compress] 单核测速：{len(sample)} 个样本 × {len(sp_schemes)} 方案（{pin}）")
            for r in sample:
                for sc in sp_schemes:
                    rep.check()
                    fut = ex.submit(cp.time_file, abs_of[r["path"]], r, sc, repeat)
                    while True:                        # 慢方案单次可达数十秒：等待期间也响应停止
                        try:
                            t = fut.result(timeout=0.5)
                            break
                        except TimeoutError:
                            rep.check()
                    k += 1
                    rep.step("speed", k, total, msg=f"{cp.group_of(r)} · {cx.PIXEL_CODECS[sc].label if sc in cx.PIXEL_CODECS else sc}")
                    if t.get("ok"):
                        speed.append({"path": r["path"], "group": cp.group_of(r), "scheme": sc, "orig": r["size"],
                                      "enc_s": round(t["enc_s"], 7), "dec_s": round(t["dec_s"], 7),
                                      "enc_MBps": round(r["size"] / t["enc_s"] / 1e6, 2),
                                      "dec_MBps": round(r["size"] / t["dec_s"] / 1e6, 2), "note": t["note"]})
        except Cancelled:
            kill_pool(ex)
            raise
        ex.shutdown(wait=True)
        rep.finish("speed")
        _write_csv(out / "speed.csv", speed)
    sp = cp.speed_summary(speed, schemes)

    # ③ NTFS 压缩对照
    ntfs_res, ntfs_note = None, "已跳过（--no-ntfs）"
    if do_ntfs:
        ok, ntfs_note = ntfs.available(out)
        if ok:
            log("[compress] NTFS 压缩对照：复制到启用压缩的临时目录测实际占用")
            rep.check()
            rep.start("ntfs")
            sizes = ntfs.measure([(r["path"], Path(abs_of[r["path"]])) for r in recs], out)
            _write_csv(out / "ntfs.csv", [{"path": r["path"], "orig": r["size"], "ntfs": sizes[r["path"]]} for r in recs])
            orig = sum(r["size"] for r in recs)
            ntfs_res = {"files": len(sizes), "orig": orig, "stored": sum(sizes.values()),
                        "ratio": round(orig / sum(sizes.values()), 4), "by_group": {}, "by_dir": {}}
            for key, fn in (("by_group", cp.group_of), ("by_dir", lambda r: r["path"].split("/")[0])):
                acc: dict[str, list[int]] = {}
                for r in recs:
                    v = acc.setdefault(fn(r), [0, 0])
                    v[0] += r["size"]
                    v[1] += sizes[r["path"]]
                ntfs_res[key] = {g: round(v[0] / v[1], 4) for g, v in sorted(acc.items())}
            ntfs_note = "NTFS LZNT1（compact /c），GetCompressedFileSizeW 取实际占用"
            rep.finish("ntfs", ratio=ntfs_res["ratio"], orig=ntfs_res["orig"], stored=ntfs_res["stored"])
        else:
            log(f"[compress] {ntfs_note}")
            rep.skip("ntfs", ntfs_note)

    summ = cp.summarize(rows, schemes)
    summary = {
        "machine": {"cpu": cp.cpu_name(), "logical_cpus": os.cpu_count(), "os": platform.platform(),
                    "python": platform.python_version()},
        "sample_dir": str(root), "files": len(recs), "bytes": sum(r["size"] for r in recs), "schemes": schemes,
        "labels": {**cx.FILE_LABELS, **{k: v.label for k, v in cx.PIXEL_CODECS.items()}},
        "lib_versions": cx.LIB_VERSIONS, "speed": sp,
        "speed_protocol": (f"单核（{pin}），预热 1 次 + {repeat} 次取中位数；每个格式组抽 {speed_per_group} 个文件；"
                           "MB/s 按原文件字节计；编码含拆分与非像素 zstd，解码含重组") if pin else "未测速",
        "ratio_protocol": "压缩比 = 原文件总字节 / 存储总字节；存储体积按“像素码流 + 非像素字节 zstd + 配方 + 容器头”算全；"
                          "校验失败或不可处理的文件按兜底（zstd 或原样）计入",
        "ntfs": ntfs_res, "ntfs_note": ntfs_note, "errors": errors, **summ,
        "elapsed_s": round(time.perf_counter() - t_all, 1),
    }
    (out / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=1), encoding="utf-8")
    return summary


def print_summary(summary: dict, log=print):
    schemes = summary["schemes"]
    log(f"[compress] 完成，用时 {summary['elapsed_s']:.0f}s   CPU：{summary['machine']['cpu']}")
    log("  （速度为单核，不含 TIFF-LZW 组：其复现式还原用纯 Python LZW，不代表产品）")
    log(f"  {'方案':<20}{'总体':>7}{'仅图像':>8}{'通过':>6}{'兜底':>5}{'原样':>5}{'编码MB/s':>10}{'解码MB/s':>10}")
    for sc in schemes:
        o, im = summary["overall"][sc], summary["image_only"][sc]
        st = o["status"]
        s = summary["speed"].get(sc, {})
        s = s.get("excl_py_lzw") or s
        log(f"  {summary['labels'][sc]:<20}{o['ratio']:>7.2f}{(im['ratio'] or 0):>8.2f}{st.get('ok', 0):>6}"
            f"{st.get('fallback', 0) + st.get('mismatch', 0):>5}{st.get('stored_raw', 0):>5}"
            f"{s.get('enc_MBps', float('nan')):>10.1f}{s.get('dec_MBps', float('nan')):>10.1f}")
    if summary["ntfs"]:
        log(f"  {'NTFS 压缩':<20}{summary['ntfs']['ratio']:>7.2f}")
    for sc, b in summary["bayer"].items():
        log(f"  Bayer {sc:<7} 直接压 {b['ratio_direct']:.2f} → 拆四平面 {b['ratio_split']:.2f}（体积 -{b['saving']:.1%}）")
    log(f"  校验失败 / 兜底明细 {len(summary['failures'])} 条，整文件异常 {len(summary['errors'])} 个（见 summary.json）")
