"""把识别结果与合成数据的真值（manifest.json）逐字段比对。"""
from __future__ import annotations

from collections import Counter, defaultdict

FULL = ["class", "container", "width", "height", "channels", "bits_stored", "bits_effective", "shift",
        "byteorder", "row_order", "row_padding", "pages", "layout", "compression", "palette", "bayer",
        "pixel_format", "writer", "trailer_bytes", "raw_config", "damaged"]
BY_CLASS = {
    "unknown": ["class", "container", "damaged"],
    "unsupported": ["class", "container", "width", "height", "channels"],
}
DAMAGED = ["class", "container", "width", "height", "bits_stored", "damaged"]


def _fields(exp: dict) -> list[str]:
    if exp.get("damaged"):
        return DAMAGED
    return BY_CLASS.get(exp["class"], FULL)


def _match(field: str, want, rec: dict) -> bool:
    if field == "bayer":
        if want is None:
            return not rec["bayer_candidates"]
        return want == rec["bayer"] or want in rec["bayer_candidates"]
    return rec.get(field) == want


def evaluate(records: list[dict], manifest: dict) -> dict:
    by_path = {r["path"]: r for r in records}
    rows, per_cat, per_field = [], defaultdict(lambda: [0, 0]), Counter()
    bayer_exact, raw_rank = [], []
    for f in manifest["files"]:
        exp = f["expect"]
        if exp is None:
            continue
        rec = by_path.get(f["path"])
        if rec is None:
            rows.append({"path": f["path"], "ok": False, "diff": {"_": "未扫描到"}})
            per_cat[f["category"]][1] += 1
            continue
        diff = {k: (exp.get(k), rec.get(k)) for k in _fields(exp) if not _match(k, exp.get(k), rec)}
        for k in diff:
            per_field[k] += 1
        ok = not diff
        per_cat[f["category"]][0] += ok
        per_cat[f["category"]][1] += 1
        rows.append({"path": f["path"], "ok": ok, "diff": diff})
        if exp.get("bayer") and rec["bayer_candidates"]:
            bayer_exact.append((rec["bayer"] or rec["bayer_guess"]) == exp["bayer"])
        if exp.get("raw_config") == "none":
            cands = rec.get("raw_candidates") or []
            key = (exp["width"], exp["height"], exp["pixel_format"])
            rank = next((i + 1 for i, c in enumerate(cands)
                         if (c["width"], c["height"], c["pixel_format"]) == key), None)
            raw_rank.append({"path": f["path"], "rank": rank, "candidates": len(cands)})
    total = len(rows)
    good = sum(r["ok"] for r in rows)
    return {
        "files": total, "correct": good, "accuracy": good / total if total else 0.0,
        "per_category": {k: {"correct": v[0], "total": v[1]} for k, v in sorted(per_cat.items())},
        "field_errors": dict(per_field.most_common()),
        "bayer_pattern_exact": {"correct": sum(bayer_exact), "total": len(bayer_exact)},
        "raw_size_candidates": raw_rank,
        "mismatches": [r for r in rows if not r["ok"]],
    }
