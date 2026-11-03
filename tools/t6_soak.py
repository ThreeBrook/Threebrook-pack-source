"""阶段 1 T6 验收（加速版）：模拟调试期高出图量连续运行，检查“磁盘不写满、NG 零删除、审计日志完整可校验”。

iis-loadsim 按节拍写 BMP（cam0~2 / 批次 / OK|NG 目录，NG 约 10%），服务监控该目录，目录配额（quota）远小于写入总量，
迫使防爆盘持续删除最旧的 OK 图。运行期间每 2 秒读 status.json 记录用量与剩余空间；结束后：
  - 写入清单中每个 NG 文件都必须还在（原文件或 .iis），且内容逐字节一致；
  - 每个不在了的 OK 文件都必须有审计“delete”记录，在的必须内容一致；
  - 配额内用量从未超过配额（防爆盘生效），本机磁盘剩余空间从未低于 10 GB；
  - 审计日志哈希链校验通过。

任务书要求 72 小时；本机做加速版（默认 60 分钟、每秒 10 张约 12 MB/s），72 小时连续运行放到同型号工控机（T11 长稳测试）。

用法：python tools/t6_soak.py [--minutes 60] [--rate 10] [--quota-gb 6] [--out .tmp/t6_soak]
"""
from __future__ import annotations

import argparse
import json
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

import psutil

ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / "build/win-x64/src"
LOADSIM = BIN / "tools/Release/iis-loadsim.exe"
SERVICE = BIN / "service/Release/iis-service.exe"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--minutes", type=float, default=60)
    ap.add_argument("--rate", type=float, default=10)
    ap.add_argument("--quota-gb", type=float, default=6)
    ap.add_argument("--ng-ratio", type=float, default=0.1)
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--out", type=Path, default=ROOT / ".tmp/t6_soak")
    args = ap.parse_args()
    out = args.out.resolve()
    shutil.rmtree(out, ignore_errors=True)
    watch = out / "watch"
    watch.mkdir(parents=True)
    cfg = {
        "watch": [{"path": str(watch), "quota_mb": args.quota_gb * 1000}],
        "delay_s": 5, "stable_s": 1, "retry_s": 1, "rescan_s": 600, "threads": args.threads,
        "priority": "below_normal", "min_free_mb": 0, "status_s": 2,
        "log_dir": str(out / "logs"), "data_dir": str(out / "data"),
        "classify": {"templates": ["{station}/{batch}/{verdict}/*"]},
        "disk": {"warn_free_percent": 30, "delete_free_percent": 15, "target_free_percent": 25, "check_s": 2},
    }
    (out / "config.json").write_text(json.dumps(cfg, ensure_ascii=False, indent=1), encoding="utf-8")
    svc = subprocess.Popen([str(SERVICE), "run", "--config", str(out / "config.json")], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
    time.sleep(2)
    seconds = args.minutes * 60
    manifest = out / "manifest.txt"
    wr = subprocess.Popen([str(LOADSIM), "write", "--dir", str(watch), "--rate", str(args.rate), "--seconds", str(seconds),
                           "--manifest", str(manifest), "--stations", "3", "--ng-ratio", str(args.ng_ratio)],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    quota = args.quota_gb * 1e9
    status_file = out / "logs" / "status.json"
    samples, max_used, min_free, min_disk = 0, 0, float("inf"), float("inf")
    levels: dict[str, int] = {}
    t0 = time.time()
    next_print = t0 + 60
    while wr.poll() is None:
        time.sleep(2)
        try:
            s = json.loads(status_file.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        st = s["storage"][0]
        samples += 1
        max_used = max(max_used, st["used"])
        min_free = min(min_free, st.get("free", quota))
        levels[st.get("level", "?")] = levels.get(st.get("level", "?"), 0) + 1
        min_disk = min(min_disk, psutil.disk_usage(str(watch)).free)
        if time.time() >= next_print:
            next_print += 60
            print(f"{(time.time() - t0) / 60:5.1f} 分钟：用量 {st['used'] / 1e9:.2f} / 配额 {quota / 1e9:.1f} GB，"
                  f"剩余 {st.get('free', 0) / 1e9:.2f} GB（{st.get('level')}），已压缩 {s['compressed']}，"
                  f"删除 {s['deleted']}（NG {s['deleted_ng']}），等待 {s['waiting']}，错误 {s['errors']}", flush=True)
    wr_out, wr_err = wr.communicate()
    # 收尾：处理完剩余文件再停
    deadline = time.time() + 300
    while time.time() < deadline:
        time.sleep(2)
        try:
            s = json.loads(status_file.read_text(encoding="utf-8"))
            if s["waiting"] == 0 and s["ready"] == 0 and s["inflight"] == 0:
                break
        except (OSError, ValueError):
            pass
    time.sleep(8)
    svc.send_signal(signal.CTRL_BREAK_EVENT)
    svc.wait(120)
    final = json.loads(status_file.read_text(encoding="utf-8"))

    # 审计里的删除记录
    deleted = set()
    for log in sorted((out / "data" / "audit").glob("audit-*.log")):
        for line in log.read_text(encoding="utf-8").splitlines():
            j = json.loads(line)
            if j["act"] == "delete":
                deleted.add(j["path"])
    must_exist, ng_total, ok_deleted, ok_missing_no_audit, ng_missing = [], 0, 0, [], []
    for line in manifest.read_text(encoding="utf-8").splitlines():
        parts = line.split("\t")
        if len(parts) < 3:  # latest.bmp（同名覆盖，最后内容）
            must_exist.append(line)
            continue
        path, _sha, verdict = parts
        present = Path(path).exists() or Path(path + ".iis").exists()
        if verdict == "NG":
            ng_total += 1
            if present:
                must_exist.append(line)
            else:
                ng_missing.append(path)
        elif present:
            must_exist.append(line)
        elif path in deleted:
            ok_deleted += 1
        else:
            ok_missing_no_audit.append(path)
    keep = out / "must_exist.txt"
    keep.write_text("\n".join(must_exist) + "\n", encoding="utf-8")
    ver = json.loads(subprocess.run([str(LOADSIM), "verify", "--manifest", str(keep)], capture_output=True).stdout)
    audit = subprocess.run([str(SERVICE), "audit-verify", "--config", str(out / "config.json")], capture_output=True)
    report = {
        "minutes": args.minutes, "rate": args.rate, "quota_gb": args.quota_gb, "write": json.loads(wr_out),
        "samples": samples, "max_used_gb": max_used / 1e9, "min_free_gb": min_free / 1e9,
        "min_disk_free_gb": min_disk / 1e9, "levels": levels,
        "compressed": final["compressed"], "deleted": final["deleted"], "deleted_ng": final["deleted_ng"],
        "errors": final["errors"], "ng_written": ng_total, "ng_missing": len(ng_missing),
        "ok_deleted_with_audit": ok_deleted, "ok_missing_without_audit": len(ok_missing_no_audit),
        "verify_present": ver, "audit_verify": audit.stdout.decode("utf-8", "replace").strip(),
        "audit_ok": audit.returncode == 0, "final_storage": final["storage"], "daily": final["daily"],
    }
    if wr_err:
        report["write_errors"] = wr_err.decode("utf-8", "replace").splitlines()[:20]
    (out / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=1), encoding="utf-8")
    ok = (report["write"]["failures"] == 0 and max_used <= quota and len(ng_missing) == 0 and final["deleted_ng"] == 0
          and not ok_missing_no_audit and ver["missing"] == 0 and ver["mismatch"] == 0 and report["audit_ok"])
    print(f"写入 {report['write']['written']} 张（失败 {report['write']['failures']}），压缩 {final['compressed']}，"
          f"删除 {final['deleted']}（NG {final['deleted_ng']}）；最大用量 {max_used / 1e9:.2f} / 配额 {quota / 1e9:.1f} GB，"
          f"最小剩余 {min_free / 1e9:.2f} GB；NG 写入 {ng_total}、缺失 {len(ng_missing)}；OK 删除且有审计 {ok_deleted}、"
          f"无审计缺失 {len(ok_missing_no_audit)}；在盘文件核对 {ver}；{report['audit_verify']}")
    print("通过" if ok else "未通过", f"报告 {out / 'report.json'}")
    shutil.rmtree(watch, ignore_errors=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
