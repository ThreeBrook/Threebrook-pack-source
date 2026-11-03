"""T3 验收：不压缩，只“拆分 → 脱离原文件 → 重组 → SHA-256 比对”。"""
from __future__ import annotations

import csv
import hashlib
import json
import time
from collections import Counter
from pathlib import Path

from . import SPLITTABLE, LayoutError, rebuild, split


def roundtrip(data: bytes, rec: dict) -> dict:
    out = {"path": rec["path"], "class": rec["class"], "size": rec["size"], "status": None, "reason": None,
           "planes": None, "skeleton_bytes": None, "extras_bytes": None, "recipe_bytes": None,
           "compression": rec.get("compression"), "encoder": None, "libtiff_lzw_segments": None,
           "split_s": None, "rebuild_s": None}
    if rec["class"] not in SPLITTABLE:
        out.update(status="not_image", reason=f"类别 {rec['class']}，按兜底处理")
        return out
    t0 = time.perf_counter()
    try:
        s = split(data, rec)
    except LayoutError as e:
        out.update(status="fallback", reason=str(e))
        return out
    except Exception as e:  # 不中断
        out.update(status="fallback", reason=f"拆分异常 {type(e).__name__}: {e}")
        return out
    t1 = time.perf_counter()
    try:
        rebuilt = rebuild(s.detach())
        good = hashlib.sha256(rebuilt).hexdigest() == rec["sha256"]
        out["status"] = "ok" if good else "mismatch"
        if not good:
            out["reason"] = "重组 SHA-256 不一致"
    except Exception as e:
        out.update(status="mismatch", reason=f"重组异常 {type(e).__name__}: {e}")
    t2 = time.perf_counter()
    pages = s.recipe.get("pages") or []
    encs = {p["encoder"] for p in pages if p.get("encoder")}
    out.update(planes=";".join(f"{p.name}{'x'.join(map(str, p.data.shape))}/{p.data.dtype}/{p.bits}b" for p in s.planes),
               skeleton_bytes=s.skeleton_bytes, extras_bytes=s.extras_bytes,
               recipe_bytes=len(json.dumps(s.recipe, ensure_ascii=False, separators=(",", ":")).encode()),
               encoder="+".join(sorted(encs)) or None,
               libtiff_lzw_segments=sum((p.get("encoders") or "").count("L") for p in pages) or None,
               split_s=round(t1 - t0, 4), rebuild_s=round(t2 - t1, 4))
    return out


def run_dir(root: Path, out: Path, raw_config: Path | None = None, log=print, rep=None) -> tuple[dict, list[dict]]:
    """对目录内每个文件：识别 → 拆分 → 脱离原文件重组 → SHA-256；统计复现式还原。"""
    from ..detect import detect_file, iter_files
    from ..formats import raw as fraw
    rules = fraw.load_config(raw_config if raw_config and raw_config.is_file() else None)
    out.mkdir(parents=True, exist_ok=True)
    from ..progress import NULL
    rep = rep or NULL
    t0 = time.perf_counter()
    rows = []
    files = list(iter_files(root))
    for i, p in enumerate(files, 1):
        rep.check()
        rows.append(roundtrip(p.read_bytes(), detect_file(p, root, rules)))
        bad = sum(r["status"] == "mismatch" for r in rows)
        rep.step("layout", i, len(files), msg=p.name, ok=sum(r["status"] == "ok" for r in rows), mismatch=bad)
        if log and i % 50 == 0:
            log(f"[layout] 已处理 {i} 个文件")
    if rows:
        with open(out / "layout.csv", "w", newline="", encoding="utf-8-sig") as f:
            w = csv.DictWriter(f, list(rows[0]))
            w.writeheader()
            w.writerows(rows)
    st = Counter(r["status"] for r in rows)
    comp = [r for r in rows if r["class"] == "tiff_compressed"]
    comp_ok = [r for r in comp if r["status"] == "ok"]
    summary = {
        "files": len(rows), "status": dict(st), "splittable_ok": st["ok"], "mismatch": st["mismatch"],
        "reproduce": {"total": len(comp), "ok": len(comp_ok),
                      "by_encoder": dict(Counter(r["encoder"] for r in comp_ok)),
                      "libtiff_lzw_segments": sum(r["libtiff_lzw_segments"] or 0 for r in comp_ok),
                      "failed": [{"path": r["path"], "reason": r["reason"]} for r in comp if r["status"] != "ok"]},
        "fallback": [{"path": r["path"], "class": r["class"], "reason": r["reason"]} for r in rows
                     if r["status"] == "fallback"],
        "elapsed_s": round(time.perf_counter() - t0, 1),
    }
    (out / "layout_summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=1), encoding="utf-8")
    return summary, rows


def print_summary(summary: dict, rows: list[dict], log=print):
    st = summary["status"]
    log(f"[layout] {summary['files']} 个文件，用时 {summary['elapsed_s']}s")
    log(f"  拆分再重组 SHA-256 一致 {st.get('ok', 0)}，不一致 {st.get('mismatch', 0)}，"
        f"兜底 {st.get('fallback', 0)}，非图像 {st.get('not_image', 0)}")
    rp = summary["reproduce"]
    if rp["total"]:
        log(f"  复现式还原 {rp['ok']}/{rp['total']}（{rp['ok'] / rp['total']:.0%}）：{rp['by_encoder']}")
        for f in rp["failed"]:
            log(f"    ✗ {f['path']}：{f['reason']}")
    for f in summary["fallback"]:
        log(f"  兜底 {f['path']}（{f['class']}）：{f['reason']}")
    for r in rows:
        if r["status"] == "mismatch":
            log(f"  ✗ 不一致 {r['path']}：{r['reason']}")
