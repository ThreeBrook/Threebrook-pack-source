"""阶段 1 T3 吞吐验收：内核与阶段 0 同编码器（JPEG-LS）在同一批文件、同一测法下的单核速度对比。

基准取阶段 0 T4 实测的 speed.csv（每类代表文件、绑定空闲核、预热 1 次 + 3 次取中位数）：
  阶段 0 编码 = 拆分 + JPEG-LS 编码 + 非像素 zstd；解码 = 像素解码 + 非像素解压 + 重组
内核：编码 = 识别 + 拆分 + JPEG-LS 编码 + 写容器；还原 = 打开容器 + CRC 校验 + 解码 + 重组 + SHA-256 比对。
另报内核完整写入路径（再加自检）的速度，供 T4 / T5 估算核数。

用法：python tools/t3_speed.py [--speed bench/out/t4_compress/speed.csv] [--root bench/out/synth] [--repeat 3]
"""
from __future__ import annotations

import argparse
import csv
import io
import json
import subprocess
import sys
import time
from pathlib import Path

import psutil
import yaml

ROOT = Path(__file__).resolve().parent.parent
PERF = ROOT / "build/win-x64/src/tools/Release/iis-perf.exe"


def idle_core(limit: int = 64) -> int:
    """采样 1 秒，选最空闲的逻辑核（只在第一个处理器组内选，SetThreadAffinityMask 只作用于本组）。"""
    use = psutil.cpu_percent(interval=2.0, percpu=True)[:limit]
    cands = [i for i in range(2, len(use), 2)]          # 偶数号核：避开超线程兄弟核与 0 号核上的系统中断
    return min(cands, key=lambda i: use[i])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--speed", type=Path, default=ROOT / "bench/out/t4_compress/speed.csv")
    ap.add_argument("--root", type=Path, default=ROOT / "bench/out/synth")
    ap.add_argument("--repeat", type=int, default=3)
    ap.add_argument("--json", type=Path)
    ap.add_argument("--cpu", type=int, help="指定绑定的逻辑核（默认自动选最空闲的偶数号核，避开 0 号）")
    args = ap.parse_args()

    base = {}
    with open(args.speed, encoding="utf-8-sig") as f:
        for r in csv.DictReader(f):
            if r["scheme"] == "jpegls":
                base[r["path"]] = r
    rules = yaml.safe_load((args.root / "raw_config.yaml").read_text(encoding="utf-8"))
    rules_json = ROOT / ".tmp/t3_raw_rules.json"
    rules_json.parent.mkdir(exist_ok=True)
    rules_json.write_text(json.dumps(rules, ensure_ascii=False), encoding="utf-8")

    cpu = args.cpu if args.cpu is not None else idle_core()
    cmd = [str(PERF), "files", str(args.root), "--rules", str(rules_json), "--repeat", str(args.repeat),
           "--cpu", str(cpu), *base]
    t0 = time.time()
    out = subprocess.run(cmd, capture_output=True, check=True).stdout.decode("utf-8")
    rows = list(csv.DictReader(io.StringIO(out)))

    tot = {"bytes": 0, "py_enc": 0.0, "py_dec": 0.0, "c_enc": 0.0, "c_full": 0.0, "c_dec": 0.0}
    table = []
    for r in rows:
        b = base[r["path"]]
        n = int(r["orig"])
        if r["method"] != "1":
            print("未按图像压缩：", r["path"], r["method"])
            continue
        c_enc = float(r["analyze_s"]) + float(r["encode_s"])
        c_full = c_enc + float(r["verify_s"])
        c_dec = float(r["restore_s"])
        py_enc, py_dec = float(b["enc_s"]), float(b["dec_s"])
        tot["bytes"] += n
        tot["py_enc"] += py_enc
        tot["py_dec"] += py_dec
        tot["c_enc"] += c_enc
        tot["c_full"] += c_full
        tot["c_dec"] += c_dec
        mb = n / 1e6
        table.append({"path": r["path"], "group": b["group"], "py_enc_MBps": round(mb / py_enc, 1),
                      "cpp_enc_MBps": round(mb / c_enc, 1), "py_dec_MBps": round(mb / py_dec, 1),
                      "cpp_restore_MBps": round(mb / c_dec, 1), "cpp_full_write_MBps": round(mb / c_full, 1)})
    mb = tot["bytes"] / 1e6
    summary = {
        "cpu": cpu, "files": len(table), "elapsed_s": round(time.time() - t0, 1),
        "py_enc_MBps": round(mb / tot["py_enc"], 1), "cpp_enc_MBps": round(mb / tot["c_enc"], 1),
        "enc_ratio": round(tot["py_enc"] / tot["c_enc"], 2),
        "py_dec_MBps": round(mb / tot["py_dec"], 1), "cpp_restore_MBps": round(mb / tot["c_dec"], 1),
        "dec_ratio": round(tot["py_dec"] / tot["c_dec"], 2),
        "cpp_full_write_MBps": round(mb / tot["c_full"], 1),
    }
    w = max(len(t["group"]) for t in table)
    print(f"{'类别':<{w}}  阶段0编码  内核编码  阶段0解码  内核还原  内核完整写入（MB/s）")
    for t in table:
        print(f"{t['group']:<{w}}  {t['py_enc_MBps']:>8}  {t['cpp_enc_MBps']:>8}  {t['py_dec_MBps']:>8}  "
              f"{t['cpp_restore_MBps']:>8}  {t['cpp_full_write_MBps']:>8}")
    print(json.dumps(summary, ensure_ascii=False, indent=1))
    if args.json:
        args.json.write_text(json.dumps({"summary": summary, "files": table}, ensure_ascii=False, indent=1),
                             encoding="utf-8")
    sys.exit(0 if summary["enc_ratio"] >= 0.9 and summary["dec_ratio"] >= 0.9 else 1)


if __name__ == "__main__":
    main()
