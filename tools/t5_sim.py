"""阶段 1 T5 验收模拟：后台服务对“检测”的影响与“写图无失败”。

在本机模拟一台 8 核视觉工控机：检测负载（iis-loadsim inspect）8 个线程绑在 8 个最空闲的物理核上，按 50 ms 节拍处理帧，
记录每帧耗时；写图负载（iis-loadsim write）按固定速率写 1024×1200 BMP（另有一个文件每次同名覆盖）。

本机是共用的多路服务器，其他负载会让检测耗时整体漂移（顺序跑“基线”与“服务”两段，基线自身就能差 60%）。
所以采用交替窗口：检测与写图连续运行，服务按“关 → 开 → 关 → 开 …”每 window 秒切换一次，
每一对相邻窗口比较 P99，再取各对的中位数——慢变化的漂移在一对窗口内基本抵消。服务开启的窗口还要处理关闭期间积压的文件，
负载比稳态更重（偏保守）。

两组场景各自独立运行：
  B  默认资源控制：后台优先级、2 个压缩线程、只用这 8 个核中的 2 个
  C  不做资源控制：普通优先级、8 个压缩线程、与检测共用全部 8 个核——对照
最后让服务把剩余文件处理完，核对写入清单：每个文件原文件完好，或 .iis 还原逐字节一致。

用法：python tools/t5_sim.py [--pairs 6] [--window 20] [--rate 10] [--out .tmp/t5_sim]
"""
from __future__ import annotations

import argparse
import json
import shutil
import signal
import statistics
import subprocess
import sys
import time
from pathlib import Path

import psutil

ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / "build/win-x64/src"
LOADSIM = BIN / "tools/Release/iis-loadsim.exe"
SERVICE = BIN / "service/Release/iis-service.exe"


def quiet_cores(n: int) -> list[int]:
    """采样 3 秒，取 n 个最空闲的偶数号逻辑核（避开 0 号核与超线程兄弟核）。"""
    use = psutil.cpu_percent(interval=3.0, percpu=True)[:64]
    return sorted(sorted(range(2, len(use), 2), key=lambda i: use[i])[:n])


def p99(v: list[float]) -> float:
    v = sorted(v)
    return v[min(len(v) - 1, int(0.99 * (len(v) - 1) + 0.5))] if v else 0.0


class Service:
    def __init__(self, cfg_path: Path):
        self.cfg_path, self.proc = cfg_path, None

    def start(self):
        self.proc = subprocess.Popen([str(SERVICE), "run", "--config", str(self.cfg_path)], stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL, creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)

    def stop(self) -> float:
        t0 = time.time()
        self.proc.send_signal(signal.CTRL_BREAK_EVENT)
        try:
            self.proc.wait(120)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            raise RuntimeError("服务 120 秒内未停止")
        return time.time() - t0


def scenario(name: str, svc_opts: dict, args, cores: list[int], work: Path) -> dict:
    base = work / name
    shutil.rmtree(base, ignore_errors=True)
    watch = base / "watch"
    watch.mkdir(parents=True)
    cfg = {"watch": [{"path": str(watch)}], "delay_s": args.delay, "stable_s": 1, "retry_s": 1, "rescan_s": 0,
           "min_free_mb": 0, "log_dir": str(base / "logs"), "status_s": 2, **svc_opts}
    cfg_path = base / "config.json"
    cfg_path.write_text(json.dumps(cfg, ensure_ascii=False, indent=1), encoding="utf-8")
    svc = Service(cfg_path)
    total = args.pairs * 2 * args.window
    trace, manifest = base / "trace.csv", base / "manifest.txt"
    insp = subprocess.Popen([str(LOADSIM), "inspect", "--threads", str(len(cores)), "--cores", ",".join(map(str, cores)),
                             "--seconds", str(total), "--period-ms", "50", "--work", str(args.work), "--trace", str(trace)],
                            stdout=subprocess.PIPE)
    wr = subprocess.Popen([str(LOADSIM), "write", "--dir", str(watch), "--rate", str(args.rate), "--seconds", str(total),
                           "--manifest", str(manifest)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    windows, stops = [], []
    t = time.time()
    for k in range(args.pairs):
        for on in (False, True):
            if on:
                svc.start()
            end = t + args.window
            time.sleep(max(0.0, end - time.time()))
            if on:
                stops.append(svc.stop())
            windows.append((on, t, time.time()))
            t = time.time()
    insp_out = json.loads(insp.communicate()[0])
    wr_out, wr_err = wr.communicate()
    # 收尾：让服务把剩余文件处理完
    svc.start()
    status = base / "logs" / "status.json"
    deadline = time.time() + 300
    while time.time() < deadline:
        time.sleep(2)
        try:
            s = json.loads(status.read_text(encoding="utf-8"))
            if s["waiting"] == 0 and s["ready"] == 0 and s["inflight"] == 0 and time.time() - s["time"] < 3:
                break
        except (OSError, ValueError, KeyError):
            pass
    time.sleep(args.delay + 3)
    svc.stop()
    frames = []
    for line in trace.read_text().splitlines():
        a, b = line.split(",")
        frames.append((float(a), float(b)))
    per = []
    for on, s, e in windows:
        v = [d for ts, d in frames if s + 0.5 <= ts < e - 0.5]
        per.append({"on": on, "frames": len(v), "p50": statistics.median(v) if v else 0, "p99": p99(v)})
    ratios = [per[2 * k + 1]["p99"] / per[2 * k]["p99"] - 1 for k in range(args.pairs) if per[2 * k]["p99"] > 0]
    on_all = [d for (on, s, e) in windows if on for ts, d in frames if s + 0.5 <= ts < e - 0.5]
    off_all = [d for (on, s, e) in windows if not on for ts, d in frames if s + 0.5 <= ts < e - 0.5]
    ver = subprocess.run([str(LOADSIM), "verify", "--manifest", str(manifest)], capture_output=True)
    res = {
        "scenario": name, "service": svc_opts, "inspect": insp_out, "write": json.loads(wr_out),
        "windows": per, "pair_p99_increase": ratios, "median_p99_increase": statistics.median(ratios),
        "pooled_p99_off": p99(off_all), "pooled_p99_on": p99(on_all),
        "pooled_p99_increase": p99(on_all) / p99(off_all) - 1,
        "service_stop_seconds_max": max(stops), "verify": json.loads(ver.stdout),
        "leftover_temps": len(list(watch.rglob("*.iistmp"))),
        "status": json.loads(status.read_text(encoding="utf-8")),
    }
    if wr_err:
        res["write_errors"] = wr_err.decode("utf-8", "replace").splitlines()[:20]
    shutil.rmtree(watch, ignore_errors=True)  # 释放磁盘空间；日志、清单与帧记录保留
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pairs", type=int, default=6)
    ap.add_argument("--window", type=float, default=20)
    ap.add_argument("--rate", type=float, default=10, help="每秒写几张 1024×1200 8 位 BMP（约 1.2 MB）")
    ap.add_argument("--work", type=int, default=4, help="每帧检测工作量（512×512 均值滤波次数）")
    ap.add_argument("--delay", type=float, default=3, help="服务写完判定延迟（秒）")
    ap.add_argument("--out", type=Path, default=ROOT / ".tmp/t5_sim")
    args = ap.parse_args()
    args.out = args.out.resolve()  # 配置里的相对路径按配置文件所在目录解析，这里一律给绝对路径
    args.out.mkdir(parents=True, exist_ok=True)
    cores = quiet_cores(8)
    print("检测负载使用逻辑核：", cores, flush=True)
    scenarios = [
        ("B", {"threads": 2, "cores": cores[-2:], "priority": "background"}),
        ("C", {"threads": 8, "cores": cores, "priority": "normal"}),
    ]
    results = []
    for name, opts in scenarios:
        r = scenario(name, opts, args, cores, args.out)
        results.append(r)
        w, v, s = r["write"], r["verify"], r["status"]
        print(f"{name}：P99 增加（各对中位数）{r['median_p99_increase'] * 100:+.1f}%，"
              f"合并 {r['pooled_p99_off']:.2f} → {r['pooled_p99_on']:.2f} ms（{r['pooled_p99_increase'] * 100:+.1f}%）；"
              f"各对 {', '.join(f'{x * 100:+.0f}%' for x in r['pair_p99_increase'])}；"
              f"写图 {w['written']} 张，失败 {w['failures']}，写入 P99 {w['write_p99']:.1f} ms；"
              f"服务压缩 {s['compressed']} 个（{s['bytes_in'] / 1e6:.0f} → {s['bytes_out'] / 1e6:.0f} MB），错误 {s['errors']}；"
              f"核对完好 {v['ok']}（.iis {v['as_iis']}），缺失 {v['missing']}，不一致 {v['mismatch']}，"
              f"残留临时文件 {r['leftover_temps']}；停止服务最长 {r['service_stop_seconds_max']:.1f} 秒", flush=True)
    summary = {
        "cores": cores, "pairs": args.pairs, "window": args.window, "rate": args.rate, "work": args.work,
        "write_failures": sum(r["write"]["failures"] for r in results),
        "integrity_problems": sum(r["verify"]["missing"] + r["verify"]["mismatch"] + r["leftover_temps"] for r in results),
        "scenarios": results,
    }
    (args.out / "report.json").write_text(json.dumps(summary, ensure_ascii=False, indent=1), encoding="utf-8")
    print(f"写图失败 {summary['write_failures']}；完整性问题 {summary['integrity_problems']}；报告 {args.out / 'report.json'}")
    return 0 if summary["write_failures"] == 0 and summary["integrity_problems"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
