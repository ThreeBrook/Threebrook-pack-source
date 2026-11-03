"""阶段 1 T3 对照：内核格式处理器（C++）与阶段 0 Python 参考实现，在一个目录的全部文件上逐项比对。

用法：
  python tools/t3_parity.py bench/out/synth --raw-config bench/out/synth/raw_config.yaml [--json 输出.json]

比对内容见 bench/native_check.py。产品只认 JSON 同名参数文件（xxx.json），这里按同样规则把它交给内核。
单核速度验收另见 tools/t3_speed.py（这里的耗时受 Python 交替运行影响，只作参考）。
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from bench import container as ct  # noqa: E402
from bench.detect import iter_files  # noqa: E402
from bench.formats import raw as fraw  # noqa: E402
from bench.native import Iis  # noqa: E402
from bench.native_check import check_bytes  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root", type=Path)
    ap.add_argument("--raw-config", type=Path)
    ap.add_argument("--json", type=Path)
    args = ap.parse_args()

    iis = Iis()
    ct.crc32c = iis.crc32c  # 容器格式已单独对照过；这里用内核 CRC 只为提速
    print(iis.build_info)
    rules = fraw.load_config(args.raw_config)
    hrules = iis.raw_rules(rules) if rules else None
    rows, bad = [], []
    counts = {"files": 0, "image": 0, "zstd": 0, "store": 0}
    for path in iter_files(args.root):
        rel = path.relative_to(args.root).as_posix()
        side = path.with_suffix(".json")
        sidecar = side.read_text(encoding="utf-8") if side != path and side.is_file() else None
        r = check_bytes(iis, rel, path.read_bytes(), rules, hrules, sidecar, path=path)
        counts["files"] += 1
        counts[{1: "image", 2: "zstd", 3: "store"}[r["method"]]] += 1
        rows.append(r)
        if r["diffs"]:
            bad.append(r)
            print("✗", rel, "；".join(r["diffs"][:3]))
    summary = {"build": iis.build_info, **counts, "mismatch_files": len(bad),
               "image_ratio": round(sum(r["orig"] for r in rows if r["method"] == 1) /
                                    max(1, sum(r["stored"] for r in rows if r["method"] == 1)), 3)}
    print(json.dumps(summary, ensure_ascii=False, indent=1))
    if args.json:
        args.json.write_text(json.dumps({"summary": summary, "files": rows}, ensure_ascii=False, indent=1),
                             encoding="utf-8")
    if hrules:
        iis.free_rules(hrules)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
