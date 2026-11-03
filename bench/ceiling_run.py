"""T5 编排：时序序列上的 G1 / G2 / G3a / G3b，Bayer 文件上的 G1 / G2 / G3c，按判读表给结论。"""
from __future__ import annotations

import csv
import json
import time
from concurrent.futures import ProcessPoolExecutor, wait
from pathlib import Path

import imagecodecs as ic

from . import ceiling as ce
from . import codecs as cx
from .compress import cpu_name
from .detect import detect_file, iter_files
from .formats import raw as fraw
from .progress import NULL, Cancelled, kill_pool


def _g_diag(rec: dict) -> str:
    c = rec.get("bayer") or (rec.get("bayer_candidates") or [None])[0]
    return "anti" if c in ("RGGB", "BGGR") else "main"


def _totals(rows: list[dict], key: str) -> dict:
    vals = [r.get(key) for r in rows]
    failed = sum(v is None for v in vals)
    orig = sum(r["orig"] for r in rows)
    stored = sum(v if v is not None else r["orig"] for v, r in zip(vals, rows))   # 校验失败按原样计
    return {"orig": orig, "stored": round(stored), "ratio": round(orig / stored, 4), "failed": failed}


def run(root: Path, out: Path, frames_glob: str, bg_glob: str, jobs: int, raw_config: Path | None,
        block: int = 16, radius: int = 4, log=print, rep=None) -> dict:
    rep = rep or NULL
    out.mkdir(parents=True, exist_ok=True)
    t0 = time.perf_counter()
    rules = fraw.load_config(raw_config if raw_config and raw_config.is_file() else None)
    frames = sorted(root.glob(frames_glob))
    bgs = sorted(root.glob(bg_glob))
    frec = [detect_file(p, root, rules) for p in frames]
    brec = [detect_file(p, root, rules) for p in bgs]
    bayer = [(p, r) for p in iter_files(root) if (r := detect_file(p, root, rules))["class"] == "bayer"]
    log(f"[ceiling] 序列帧 {len(frames)}，空载背景 {len(bgs)}，Bayer 文件 {len(bayer)}；并行 {jobs} 进程")
    if len(frames) < 2 or not bgs:
        raise SystemExit("序列帧不足 2 张或没有空载背景，无法做 G3a/G3b")

    ref, bits = ce.background_ref([str(p) for p in bgs], brec)
    ref_stream = ic.jpegls_encode(ref, level=0, bitspersample=bits)
    ref_share = (len(ref_stream) + cx.HEADER_BYTES + cx.SEGMENT_HEADER_BYTES) / len(frames)

    n_tasks = 3 * len(frames) - 1 + 2 * len(bayer)
    ex = ProcessPoolExecutor(max_workers=max(1, min(jobs, n_tasks)))
    try:
        f12 = [ex.submit(ce.g12, str(p), r) for p, r in zip(frames, frec)]
        b12 = [ex.submit(ce.g12, str(p), r) for p, r in bayer]
        fa = [ex.submit(ce.g3a, str(p), r, ref, bits, ref_share, 6, block) for p, r in zip(frames, frec)]
        fb = [ex.submit(ce.g3b, str(frames[i]), frec[i], str(frames[i - 1]), frec[i - 1], bits, block, radius)
              for i in range(1, len(frames))]
        fc = [ex.submit(ce.g3c, str(p), r, _g_diag(r), block) for p, r in bayer]
        allf = f12 + b12 + fa + fb + fc
        while True:
            done, pending = wait(allf, timeout=0.5)
            rep.step("ceiling", len(done), len(allf), msg="G1 / G2 / G3a / G3b / G3c")
            if not pending:
                break
            rep.check()
        seq = [f.result() for f in f12]
        for row, f in zip(seq, fa):
            row.update({k: v for k, v in f.result().items() if k not in ("path", "orig")})
        log(f"[ceiling] G1/G2/G3a 完成，{time.perf_counter() - t0:.0f}s")
        seq[0]["G3b"] = seq[0]["G3b_pure"] = seq[0]["G2.jpegls"]   # 第一帧没有前一帧，按帧内（G2 JPEG-LS）编码
        seq[0]["moving_blocks"] = "关键帧"
        for row, f in zip(seq[1:], fb):
            row.update({k: v for k, v in f.result().items() if k not in ("path", "orig")})
        bay = [f.result() for f in b12]
        for row, f in zip(bay, fc):
            row.update({k: v for k, v in f.result().items() if k not in ("path", "orig")})
    except Cancelled:
        kill_pool(ex)
        raise
    ex.shutdown(wait=True)
    log(f"[ceiling] 全部完成，{time.perf_counter() - t0:.0f}s")

    def best(rows, g):
        tots = {c: _totals(rows, f"{g}.{c}") for c in ce.G12_CODECS}
        c = min(tots, key=lambda k: tots[k]["stored"])
        return c, tots

    res = {"machine": {"cpu": cpu_name()}, "params": {"block": block, "radius": radius,
                                                      "residual_codecs": list(ce.RESIDUAL_CODECS),
                                                      "g12_codecs": list(ce.G12_CODECS),
                                                      "reference": f"{len(bgs)} 张空载背景整数均值",
                                                      "ref_bytes": len(ref_stream), "ref_share_per_frame": round(ref_share)}}
    conclusions = []
    for name, rows, g3s in (("sequence", seq, ["G3a", "G3b"]), ("bayer", bay, ["G3c"])):
        if not rows:
            continue
        c1, t1 = best(rows, "G1")
        c2, t2 = best(rows, "G2")
        tab = {"G1": {**t1[c1], "codec": c1}, "G2": {**t2[c2], "codec": c2},
               "G2(jpegls)": t2["jpegls"], "G1_all": t1, "G2_all": t2}
        for g in g3s:
            tab[g] = _totals(rows, g)
            tab[f"{g}_pure"] = _totals(rows, f"{g}_pure")
        res[name] = tab
        label = {"sequence": f"时序序列（{len(rows)} 帧）", "bayer": f"Bayer 文件（{len(rows)} 个）"}[name]
        conclusions.append(f"【{label}】G1 {tab['G1']['ratio']:.2f}（{c1}） → G2 {tab['G2']['ratio']:.2f}（{c2}）")
        conclusions.append("  " + ce.judge("G2", ce.gain(tab["G2"]["stored"], tab["G1"]["stored"]), "G1"))
        if tab["G2"]["ratio"] < 2:
            conclusions.append(f"  ⚠ G2 总体压缩比 {tab['G2']['ratio']:.2f} < 2，未达阶段 0 的 2 倍门槛")
        for g in g3s:
            gn = ce.gain(tab[g]["stored"], tab["G2"]["stored"])
            gj = ce.gain(tab[g]["stored"], tab["G2(jpegls)"]["stored"])
            conclusions.append(f"  {g} {tab[g]['ratio']:.2f}（逐块选择参考 / 帧内；纯预测 {tab[g + '_pure']['ratio']:.2f}）；"
                               f"对 JPEG-LS 版 G2（{tab['G2(jpegls)']['ratio']:.2f}）体积 {'-' if gj >= 0 else '+'}{abs(gj):.1%}")
            conclusions.append("  " + ce.judge(g, gn, f"G2（{tab['G2']['codec']}）"))
            if gn >= 0.05:
                conclusions.append("    " + ce.COSTS[g])
            if tab[g]["failed"]:
                conclusions.append(f"    ⚠ {tab[g]['failed']} 个文件往返校验失败，已按原样计入")
    from collections import Counter
    res["residual_codec_choice"] = {g: dict(Counter(r.get(f"{g}_codec") for r in seq if r.get(f"{g}_codec")))
                                    for g in ("G3a", "G3b")}
    res["conclusions"] = conclusions
    res["elapsed_s"] = round(time.perf_counter() - t0, 1)
    (out / "ceiling_summary.json").write_text(json.dumps(res, ensure_ascii=False, indent=1), encoding="utf-8")
    for fname, rows in (("ceiling_sequence.csv", seq), ("ceiling_bayer.csv", bay)):
        if rows:
            keys = list(dict.fromkeys(k for r in rows for k in r))
            with open(out / fname, "w", newline="", encoding="utf-8-sig") as f:
                w = csv.DictWriter(f, keys)
                w.writeheader()
                w.writerows(rows)
    return res
