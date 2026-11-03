"""阶段 1 T1 对照：内核库 iis.dll 与阶段 0 imagecodecs 在同一批像素平面上逐字节比对，并测单线程速度。

用法：
  python tools/native_parity.py bench/out/synth --raw-config bench/out/synth/raw_config.yaml \
      [--pgm-out .tmp/planes] [--repeat 3]

对每个可处理文件：阶段 0 识别 + 版面拆分 → 每个像素平面
  1. JPEG-LS：两边编码结果逐字节相同；两边都能解对方的码流且等于原平面
  2. 非像素字节（骨架）的 zstd-19：两边输出逐字节相同，互相可解
  3. SHA-256 与 hashlib 一致
--pgm-out 时把单分量平面导出为 PGM，供 iis-perf 测原生吞吐。
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
import time
from pathlib import Path

import imagecodecs as ic
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from bench import codecs as cx  # noqa: E402
from bench.detect import scan_dir  # noqa: E402
from bench.layout import split  # noqa: E402
from bench.native import Iis  # noqa: E402


def write_pgm(path: Path, a: np.ndarray, bits: int):
    maxval = (1 << bits) - 1
    head = f"P5\n{a.shape[1]} {a.shape[0]}\n{maxval}\n".encode()
    body = a.astype(">u2").tobytes() if maxval > 255 else a.astype(np.uint8).tobytes()
    path.write_bytes(head + body)


def median_time(fn, repeat: int) -> float:
    fn()
    ts = []
    for _ in range(repeat):
        t = time.perf_counter()
        fn()
        ts.append(time.perf_counter() - t)
    return sorted(ts)[len(ts) // 2]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root", type=Path)
    ap.add_argument("--raw-config", type=Path)
    ap.add_argument("--pgm-out", type=Path)
    ap.add_argument("--repeat", type=int, default=3)
    ap.add_argument("--json", type=Path, help="结果写入 JSON")
    args = ap.parse_args()

    iis = Iis()
    print(iis.build_info)
    recs = scan_dir(args.root, args.raw_config, log=lambda *a, **k: None)
    if args.pgm_out:
        args.pgm_out.mkdir(parents=True, exist_ok=True)

    n_planes = n_files = 0
    diffs: list[str] = []
    t_py_enc = t_py_dec = t_nat_enc = t_nat_dec = 0.0
    px_bytes = 0
    for rec in recs:
        if not rec.get("processable"):
            continue
        path = args.root / rec["path"]
        data = path.read_bytes()
        try:
            s = split(data, rec)
        except Exception as e:  # 阶段 0 本身不处理的文件不在对照范围
            print("跳过", rec["path"], e)
            continue
        n_files += 1
        if iis.sha256(data).hex() != hashlib.sha256(data).hexdigest():
            diffs.append(f"{rec['path']}: SHA-256 不同")
        for i, p in enumerate(s.planes):
            n_planes += 1
            a = cx._prep(p)
            bits = max(2, min(p.bits, a.itemsize * 8))
            py = cx._jpegls(p)
            nat = iis.jpegls_encode(a, bits)
            if py != nat:
                diffs.append(f"{rec['path']}#{i}: JPEG-LS 码流不同（{len(py)} vs {len(nat)} 字节）")
            if not np.array_equal(iis.jpegls_decode(py), a) or not np.array_equal(ic.jpegls_decode(nat), a):
                diffs.append(f"{rec['path']}#{i}: 交叉解码不一致")
            px_bytes += a.nbytes
            t_py_enc += median_time(lambda: cx._jpegls(p), args.repeat)
            t_nat_enc += median_time(lambda: iis.jpegls_encode(a, bits), args.repeat)
            t_py_dec += median_time(lambda: ic.jpegls_decode(py), args.repeat)
            t_nat_dec += median_time(lambda: iis.jpegls_decode(nat), args.repeat)
            if args.pgm_out and a.ndim == 2:
                stem = rec["path"].replace("/", "_").rsplit(".", 1)[0]
                write_pgm(args.pgm_out / f"{stem}_{i}.pgm", a, bits)
        skel = b"".join(b for _, b in s.skeleton)
        if skel:
            zp, zn = ic.zstd_encode(skel, level=19), iis.zstd_compress(skel, 19)
            if zp != zn:
                diffs.append(f"{rec['path']}: zstd-19 输出不同")
            if iis.zstd_decompress(zp, len(skel)) != skel or ic.zstd_decode(zn) != skel:
                diffs.append(f"{rec['path']}: zstd 交叉解压不一致")

    mb = px_bytes / 1e6
    res = {
        "build": iis.build_info, "files": n_files, "planes": n_planes, "pixel_mb": round(mb, 1),
        "diffs": diffs,
        "speed_mb_s": {
            "imagecodecs_encode": round(mb / t_py_enc, 1), "native_encode": round(mb / t_nat_enc, 1),
            "imagecodecs_decode": round(mb / t_py_dec, 1), "native_decode": round(mb / t_nat_dec, 1),
        },
    }
    print(json.dumps(res, ensure_ascii=False, indent=1))
    if args.json:
        args.json.write_text(json.dumps(res, ensure_ascii=False, indent=1), encoding="utf-8")
    sys.exit(1 if diffs else 0)


if __name__ == "__main__":
    main()
