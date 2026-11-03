"""T2 格式识别。"""
from __future__ import annotations

import csv
import json
from pathlib import Path

from ..formats import raw as fraw
from .core import FIELDS, detect_bytes, detect_file

__all__ = ["FIELDS", "detect_bytes", "detect_file", "scan_dir", "write_outputs"]


def iter_files(root: Path):
    for p in sorted(root.rglob("*")):
        if p.is_file():
            yield p


def scan_dir(root: Path, raw_config: Path | None = None, log=print, rep=None) -> list[dict]:
    from ..progress import NULL
    rep = rep or NULL
    rules = fraw.load_config(raw_config)
    files = list(iter_files(root))
    out = []
    for i, p in enumerate(files, 1):
        rep.check()
        out.append(detect_file(p, root, rules))
        rep.step("detect", i, len(files), msg=p.name)
        if log and i % 50 == 0:
            log(f"[detect] 已识别 {i} 个文件")
    return out


def write_outputs(records: list[dict], out: Path):
    out.mkdir(parents=True, exist_ok=True)
    (out / "detect.json").write_text(json.dumps(records, ensure_ascii=False, indent=1), encoding="utf-8")
    with open(out / "detect.csv", "w", newline="", encoding="utf-8-sig") as f:   # BOM：Excel 直接打开不乱码
        w = csv.DictWriter(f, FIELDS)
        w.writeheader()
        for r in records:
            row = dict(r)
            row["notes"] = "；".join(r["notes"])
            row["bayer_candidates"] = "/".join(r["bayer_candidates"])
            row["raw_params"] = json.dumps(r["raw_params"]) if r["raw_params"] else ""
            row["raw_candidates"] = " | ".join(f"{c['pixel_format']} {c['width']}x{c['height']} ({c['cost']})"
                                               for c in r["raw_candidates"] or [])
            w.writerow(row)
