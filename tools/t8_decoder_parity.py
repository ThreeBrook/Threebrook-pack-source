"""阶段 1 T8 对照：开源解码器 iis-decode 能否把内核生成的每个 .iis 还原为与原文件逐字节相同的文件。

对目录里每个文件：内核压缩（RAW 规则与同名参数文件同 t3_parity）→ 写出 .iis 到工作目录 → 一次性交给 iis-decode --check
→ 比对它报告的 SHA-256 与原文件。只用于开发对照；样本目录只读。

用法：python tools/t8_decoder_parity.py bench/out/synth --raw-config bench/out/synth/raw_config.yaml
"""
from __future__ import annotations

import argparse
import hashlib
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
from bench.detect import iter_files  # noqa: E402
from bench.formats import raw as fraw  # noqa: E402
from bench.native import Iis  # noqa: E402

DECODE = ROOT / "build/win-x64/decoder/Release/iis-decode.exe"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root", type=Path)
    ap.add_argument("--raw-config", type=Path)
    ap.add_argument("--work", type=Path, default=ROOT / ".tmp/t8_decoder")
    args = ap.parse_args()
    iis = Iis()
    rules = fraw.load_config(args.raw_config)
    hrules = iis.raw_rules(rules) if rules else None
    shutil.rmtree(args.work, ignore_errors=True)
    args.work.mkdir(parents=True)
    expect, counts = {}, {"image": 0, "zstd": 0, "store": 0}
    for path in iter_files(args.root):
        rel = path.relative_to(args.root).as_posix()
        side = path.with_suffix(".json")
        sidecar = side.read_text(encoding="utf-8") if side != path and side.is_file() else None
        data = path.read_bytes()
        method, cont, _detail, _t = iis.compress(data, rel, hrules, sidecar)
        counts[{1: "image", 2: "zstd", 3: "store"}[method]] += 1
        if method == 3:
            continue
        out = args.work / f"{len(expect):04d}.iis"
        out.write_bytes(cont)
        expect[str(out)] = (rel, hashlib.sha256(data).hexdigest(), len(data), method)
    if hrules:
        iis.free_rules(hrules)
    got = {}
    names = sorted(expect)
    for i in range(0, len(names), 40):  # 命令行长度有限：分批
        r = subprocess.run([str(DECODE), "--check", *names[i:i + 40]], capture_output=True)
        for line in r.stdout.decode("utf-8", "replace").splitlines():
            m = re.match(r"(.+?)：可还原，(\d+) 字节，SHA-256 ([0-9a-f]{64})$", line)
            if m:
                got[m.group(1)] = (int(m.group(2)), m.group(3))
            else:
                print("✗", line)
    bad = 0
    for name, (rel, sha, size, method) in expect.items():
        g = got.get(name)
        if g != (size, sha):
            bad += 1
            print("✗", rel, "方式", method, "解码器结果", g)
    image = sum(1 for v in expect.values() if v[3] == 1)
    print(f"{len(expect)} 个容器（图像 {image}，整文件 zstd {len(expect) - image}；保留原文件的 {counts['store']} 个不产生容器）："
          f"开源解码器还原一致 {len(expect) - bad}，不一致 {bad}")
    shutil.rmtree(args.work, ignore_errors=True)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
