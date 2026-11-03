"""图形界面（pywebview + Edge WebView2）：快速测试、测试记录、容量与 ROI、设置。

界面在 web/（HTML/CSS/JS，外观按《三溪外观规范》）；这里是暴露给页面的接口（Api）与后台任务。
测试本身调用与命令行相同的引擎（bench.scan / bench.report），在后台线程运行，进度经 Reporter 回调汇总，
页面每 0.5 秒取一次状态。全部在本机处理，不联网；样本目录只读。
"""
from __future__ import annotations

import json
import os
import shutil
import sys
import threading
import time
import traceback
from pathlib import Path

from .. import __version__
from ..progress import PHASES, Cancelled, Reporter

APP_NAME = "图像压缩评估"
WEB = Path(__file__).with_name("web")
NOT_SPLIT = {"unknown", "unsupported"}
QUICK_SPEED = ["jpegls", "htj2k", "zstd3"]


def data_dir() -> Path:
    """设置、默认结果目录、WebView 缓存都放这里：打包版放 exe 旁的 data/，源码运行放 bench/out/gui/（不落 C 盘）。"""
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent / "data"
    return Path(__file__).resolve().parents[2] / "bench" / "out" / "gui"


DEFAULT_SETTINGS = {
    "theme": "system",             # system / light / dark
    "output_root": "",             # 空 = data/results
    "jobs": max(1, (os.cpu_count() or 2) // 2 - 2),
    "mode": "quick",               # quick / full
    "ntfs": True,
    "ceiling": True,
    "cpuload": False,
    "roi": {"daily_gb": 200.0, "debug_daily_gb": 600.0, "disk_tb": 10.0, "retention_days": 90.0, "reserve": 0.10},
    "roi_set": False,              # 用户在 ROI 页写入过真实参数后为 True，报告不再标“示例参数”
}


def cluster_size(path: Path) -> int:
    """所在分区的簇大小（字节）；拿不到时按 4096。"""
    try:
        import ctypes
        root = os.path.splitdrive(str(Path(path).resolve()))[0] + "\\"
        spc, bps = ctypes.c_ulong(), ctypes.c_ulong()
        if ctypes.windll.kernel32.GetDiskFreeSpaceW(root, ctypes.byref(spc), ctypes.byref(bps), None, None):
            return spc.value * bps.value
    except Exception:
        pass
    return 4096


def folder_sizes(p: Path) -> tuple[int, int, int]:
    """(文件数, 实际大小, 磁盘占用空间)。占用空间按簇补齐估算，与资源管理器“占用空间”同口径（很小的文件可能存在 MFT 里，略有出入）。"""
    cl = cluster_size(p)
    n = size = disk = 0
    for f in p.rglob("*"):
        if f.is_file():
            s = f.stat().st_size
            n += 1
            size += s
            disk += -(-s // cl) * cl
    return n, size, disk


class Run:
    """一次测试的状态（工作线程写，界面线程读，靠锁保护）。"""

    def __init__(self, sample: Path, out: Path, opts: dict):
        self.sample, self.out, self.opts = sample, out, opts
        self.lock = threading.Lock()
        self.cancel = threading.Event()
        self.t0 = time.time()
        self.t_end = None
        self.status = "running"         # running / done / cancelled / error
        self.error = None
        self.plan: list[str] = []
        self.phases: dict[str, dict] = {}
        self.live: dict = {}
        self.live_bytes: dict = {}      # 方案 → [已核对文件的原字节, 存储字节]
        self.total_bytes = opts.get("bytes") or 0
        self.disk_bytes = opts.get("disk_bytes") or 0
        self.verified = self.mismatch = 0
        self.failures: list[dict] = []
        self.result: dict | None = None
        self.logs: list[str] = []

    def on_event(self, ev: dict):
        with self.lock:
            k = ev["kind"]
            if k == "plan":
                self.plan = ev["phases"] + ["report"]
                self.phases = {p: {"status": "wait", "done": 0, "total": None, "msg": ""} for p in self.plan}
                return
            if k == "log":
                self.logs = (self.logs + [ev["msg"]])[-200:]
                return
            ph = self.phases.setdefault(ev["phase"], {"status": "wait", "done": 0, "total": None, "msg": ""})
            if k == "start":
                ph.update(status="run", t0=ev["t"])
            elif k == "step":
                ph.update(status="run", done=ev["done"], total=ev["total"], msg=ev.get("msg", ""))
                if ev["phase"] == "compress":
                    self.live = ev.get("live", self.live)
                    self.live_bytes = {**self.live_bytes, **ev.get("live_bytes", {})}
                    self.verified, self.mismatch = ev.get("verified", 0), ev.get("mismatch", 0)
                if ev["phase"] == "layout":
                    self.mismatch = max(self.mismatch, ev.get("mismatch", 0))
            elif k == "finish":
                ph.update(status="done", t1=ev["t"], msg=ev.get("msg", ""))
                if ph.get("total"):
                    ph["done"] = ph["total"]
                if ev["phase"] == "detect":
                    self._load_detect_failures()
                if ev.get("ratio") and ev["phase"] == "ntfs":
                    self.live = {**self.live, "ntfs": ev["ratio"]}
                    self.live_bytes = {**self.live_bytes, "ntfs": [ev["orig"], ev["stored"]]}
            elif k == "skip":
                ph.update(status="skip", msg=ev.get("msg", ""))

    def _load_detect_failures(self):
        try:
            recs = json.loads((self.out / "detect" / "detect.json").read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return
        self.failures = summarize_failures(recs)

    def snapshot(self) -> dict:
        with self.lock:
            now = self.t_end or time.time()
            phases = []
            weight_total = weight_done = 0.0
            for key in self.plan:
                p = self.phases.get(key, {})
                label, w = PHASES.get(key, (key, 1))
                frac = 1.0 if p.get("status") in ("done", "skip") else (
                    (p.get("done", 0) / p["total"]) if p.get("total") else 0.0)
                weight_total += w
                weight_done += w * frac
                phases.append({"key": key, "label": label, "status": p.get("status", "wait"), "done": p.get("done", 0),
                               "total": p.get("total"), "msg": p.get("msg", ""),
                               "secs": round((p.get("t1") or now) - p["t0"], 1) if p.get("t0") else None})
            cur = next((p for p in phases if p["status"] == "run"), None)
            return {"status": self.status, "error": self.error, "out": str(self.out), "sample": str(self.sample),
                    "elapsed": round(now - self.t0, 1), "phases": phases, "current": cur,
                    "overall": round(100 * weight_done / weight_total, 1) if weight_total else 0.0,
                    "live": self.live, "live_bytes": self.live_bytes, "total_bytes": self.total_bytes, "disk_bytes": self.disk_bytes,
                    "verified": self.verified, "mismatch": self.mismatch,
                    "failures": self.failures, "result": self.result, "opts": self.opts,
                    "report": str(self.out / "report.html") if (self.out / "report.html").is_file() else None}


def summarize_failures(recs: list[dict], fails: list[dict] | None = None) -> list[dict]:
    """“没按图像压缩的文件”：未确认参数 / 损坏的逐个列出，不处理与未知的按类别合并。"""
    out = []
    groups: dict[str, list[dict]] = {}
    for r in recs:
        name, folder = r["path"].split("/")[-1], "/".join(r["path"].split("/")[:-1]) or "."
        if r["class"] in NOT_SPLIT:
            if r.get("damaged"):
                out.append({"name": name, "dir": folder, "tag": "已损坏",
                            "reason": (r.get("error") or "文件损坏") + "；按 zstd 或原样保存"})
            else:
                groups.setdefault(r["class"], []).append(r)
        elif not r.get("processable"):
            cand = (r.get("raw_candidates") or [{}])[0]
            guess = f"猜测 {cand.get('pixel_format')} {cand.get('width')}×{cand.get('height')}，" if cand else ""
            out.append({"name": name, "dir": folder, "tag": "需确认参数",
                        "reason": f"没有 RAW 配置，{guess}确认后写入 raw_config.yaml 再测"})
    labels = {"unsupported": "不是要处理的格式（JPEG、PNG 等），原样或 zstd 保存",
              "unknown": "不是图像或无法识别（文本、参数文件等），原样或 zstd 保存"}
    for cls, rs in groups.items():
        first = rs[0]["path"].split("/")[-1]
        out.append({"name": first if len(rs) == 1 else f"{first} 等 {len(rs)} 个", "dir": "", "tag": None,
                    "reason": labels[cls]})
    for f in fails or []:
        if "SHA-256" in (f.get("reason") or ""):
            out.append({"name": f["path"].split("/")[-1], "dir": "", "tag": "还原不一致",
                        "reason": f"{f['scheme']}：{f['reason']}（已按兜底计入，不算进压缩比）"})
    return out


class Api:
    """暴露给页面的方法（window.pywebview.api.xxx），返回值都是可 JSON 序列化的。
    内部状态一律用下划线开头的属性：pywebview 会递归暴露公开属性，窗口对象等不能暴露给页面。"""

    def __init__(self):
        self._window = None
        self._run: Run | None = None
        self._dir = data_dir()
        self._dir.mkdir(parents=True, exist_ok=True)
        self._settings = self._load_settings()

    # ---------- 设置
    def _load_settings(self) -> dict:
        s = json.loads(json.dumps(DEFAULT_SETTINGS))
        try:
            s.update(json.loads((self._dir / "settings.json").read_text(encoding="utf-8")))
        except (OSError, ValueError):
            pass
        return s

    def _output_root(self) -> Path:
        return Path(self._settings["output_root"]) if self._settings.get("output_root") else self._dir / "results"

    def get_info(self) -> dict:
        from ..compress import cpu_name
        from ..codecs import LIB_VERSIONS
        return {"app": APP_NAME, "version": __version__, "cpu": cpu_name(), "cpus": os.cpu_count(),
                "settings": self._settings, "output_root": str(self._output_root()), "libs": LIB_VERSIONS,
                "data_dir": str(self._dir)}

    def save_settings(self, s: dict) -> dict:
        allowed = set(DEFAULT_SETTINGS)
        self._settings.update({k: v for k, v in s.items() if k in allowed})
        (self._dir / "settings.json").write_text(json.dumps(self._settings, ensure_ascii=False, indent=1), encoding="utf-8")
        return {"ok": True, "output_root": str(self._output_root())}

    # ---------- 窗口
    def win_minimize(self):
        self._window.minimize()

    def win_toggle_max(self):
        if getattr(self, "_maxed", False):
            self._window.restore()
        else:
            self._window.maximize()
        self._maxed = not getattr(self, "_maxed", False)

    def win_close(self):
        if self._run and self._run.status == "running":
            self._run.cancel.set()
        self._window.destroy()

    # ---------- 选择与检查文件夹
    def pick_folder(self, initial: str = "") -> str | None:
        import webview
        r = self._window.create_file_dialog(webview.FileDialog.FOLDER, directory=initial or "")
        return r[0] if r else None

    def inspect(self, path: str) -> dict:
        p = Path(path)
        if not path or not p.is_dir():
            return {"ok": False, "error": "文件夹不存在"}
        n, size, disk = folder_sizes(p)
        frames = sorted(p.glob("sequence/frame_*"))
        bgs = sorted(p.glob("sequence/background_*"))
        out_root = self._output_root().resolve()
        return {"ok": True, "path": str(p), "files": n, "bytes": size, "disk_bytes": disk,
                "raw_config": (p / "raw_config.yaml").is_file(),
                "frames": len(frames), "backgrounds": len(bgs),
                "inside_output": p.resolve().is_relative_to(out_root) or out_root.is_relative_to(p.resolve())}

    # ---------- 测试
    def start(self, opts: dict) -> dict:
        if self._run and self._run.status == "running":
            return {"ok": False, "error": "已有测试在运行"}
        sample = Path(opts.get("path", ""))
        if not sample.is_dir():
            return {"ok": False, "error": "请先选择样本文件夹"}
        root = self._output_root()
        if root.resolve().is_relative_to(sample.resolve()):
            return {"ok": False, "error": "输出目录在样本文件夹里面。样本文件夹只读，请在设置里换一个输出目录"}
        name = time.strftime("%Y%m%d-%H%M%S") + "_" + (sample.name or "样本")
        out = root / name
        _, size, disk = folder_sizes(sample)
        opts = {**opts, "bytes": size, "disk_bytes": disk}
        self._run = Run(sample, out, opts)
        threading.Thread(target=self._work, args=(self._run,), daemon=True).start()
        return {"ok": True, "out": str(out)}

    def _work(self, run: Run):
        from .. import scan
        from ..report import build
        rep = Reporter(run.on_event, run.cancel)
        o = run.opts
        try:
            cfg = run.sample / "raw_config.yaml"
            scan.run(run.sample, run.out, schemes=None, quick=o.get("mode", "quick") == "quick",
                     jobs=int(self._settings.get("jobs") or 1), raw_config=cfg if cfg.is_file() else None,
                     speed_per_group=1, repeat=1 if o.get("mode", "quick") == "quick" else 3,
                     ntfs=bool(o.get("ntfs", True)),
                     frames="sequence/frame_*", background="sequence/background_*",
                     ceiling="auto" if o.get("ceiling", True) else "off", cpuload=bool(o.get("cpuload", False)),
                     cpu_cores=16, cpu_load=0.6, log=lambda m: rep.log(str(m)), rep=rep,
                     # 快速模式只给决策相关的方案测速（推荐方案、高吞吐备选、通用压缩对照），完整模式全测
                     speed_schemes=QUICK_SPEED if o.get("mode", "quick") == "quick" else None)
            rep.check()
            rep.start("report")
            roi = self._settings.get("roi") or DEFAULT_SETTINGS["roi"]
            data = build(run.out, {"example": not self._settings.get("roi_set"), "params": dict(roi)})
            rep.finish("report")
            with run.lock:
                run.result = result_brief(data)
                recs = json.loads((run.out / "detect" / "detect.json").read_text(encoding="utf-8"))
                run.failures = summarize_failures(recs, data.get("failures"))
                run.status = "done"
        except Cancelled:
            with run.lock:
                run.status = "cancelled"
        except BaseException as e:      # 包括 SystemExit（参数错误）
            with run.lock:
                run.status, run.error = "error", f"{e}"
                run.logs.append(traceback.format_exc())
        finally:
            run.t_end = time.time()
            if run.status in ("cancelled", "error"):
                mark_meta(run.out, run.status, run.t_end - run.t0)

    def poll(self) -> dict | None:
        return self._run.snapshot() if self._run else None

    def stop(self) -> dict:
        if self._run and self._run.status == "running":
            self._run.cancel.set()
        return {"ok": True}

    # ---------- 打开文件
    def open_path(self, path: str) -> dict:
        p = Path(path)
        if not p.exists():
            return {"ok": False, "error": "路径不存在"}
        os.startfile(str(p))  # noqa: S606  仅在 Windows 上提供 GUI
        return {"ok": True}

    def open_report(self, path: str) -> dict:
        import webview
        p = Path(path)
        if not p.is_file():
            return {"ok": False, "error": "报告不存在"}
        webview.create_window(f"评估报告 · {p.parent.name}", url=p.resolve().as_uri(), width=1180, height=860)
        return {"ok": True}

    # ---------- 测试记录
    def history(self) -> list[dict]:
        root = self._output_root()
        out = []
        if not root.is_dir():
            return out
        for d in sorted(root.iterdir(), reverse=True):
            meta = d / "scan_meta.json"
            if not meta.is_file():
                continue
            try:
                m = json.loads(meta.read_text(encoding="utf-8"))
                s = json.loads((d / "summary.json").read_text(encoding="utf-8")) if (d / "summary.json").is_file() else None
            except (OSError, ValueError):
                continue
            out.append({"path": str(d), "name": d.name, "sample": m.get("sample_dir"), "started": m.get("started"),
                        "status": m.get("status", "done"),
                        "elapsed": m.get("elapsed_s"), "report": str(d / "report.html") if (d / "report.html").is_file() else None,
                        "brief": result_brief(s) if s else None})
        return out

    def delete_run(self, path: str) -> dict:
        d = Path(path).resolve()
        root = self._output_root().resolve()
        if not d.is_relative_to(root) or d == root or not (d / "scan_meta.json").is_file():
            return {"ok": False, "error": "只能删除测试结果目录"}
        if self._run and self._run.status == "running" and self._run.out.resolve() == d:
            return {"ok": False, "error": "这次测试还在运行"}
        shutil.rmtree(d)
        return {"ok": True}

    # ---------- ROI
    def roi(self, path: str, params: dict) -> dict:
        from .. import roi as roi_mod
        try:
            s = json.loads((Path(path) / "summary.json").read_text(encoding="utf-8"))
            ratio = s["recommended"]["ratio"]
            p = {k: float(params[k]) for k in ("daily_gb", "disk_tb", "retention_days", "reserve")}
            p["debug_daily_gb"] = float(params.get("debug_daily_gb") or p["daily_gb"])
            d = roi_mod.device(ratio=ratio, **p)
        except (OSError, ValueError, KeyError, TypeError) as e:
            return {"ok": False, "error": f"无法计算：{e}"}
        return {"ok": True, "label": s["recommended"]["label"], "ratio": ratio, **d, "text": roi_mod.describe(d)}

    def roi_apply(self, path: str, params: dict) -> dict:
        from ..report import build
        r = self.roi(path, params)
        if not r.get("ok"):
            return r
        p = {k: float(params[k]) for k in ("daily_gb", "disk_tb", "retention_days", "reserve")}
        p["debug_daily_gb"] = float(params.get("debug_daily_gb") or p["daily_gb"])
        build(Path(path), {"example": False, "params": p})
        self._settings.update(roi=p, roi_set=True)
        self.save_settings({})
        return {"ok": True}


def mark_meta(out: Path, status: str, elapsed: float):
    """把未完成测试的状态写进 scan_meta.json（停止 / 出错），记录页据此显示。"""
    f = out / "scan_meta.json"
    try:
        m = json.loads(f.read_text(encoding="utf-8"))
        m.update(status=status, elapsed_s=round(elapsed, 1))
        f.write_text(json.dumps(m, ensure_ascii=False, indent=1), encoding="utf-8")
    except (OSError, ValueError):
        pass


def result_brief(data: dict) -> dict:
    """报告数据里界面要用的部分。"""
    c = data["compress"]
    rec = c["overall"][data["recommended"]["scheme"]]
    st = rec["status"]
    ntfs = c.get("ntfs") or {}
    parts = (c.get("parts") or {}).get(data["recommended"]["scheme"])
    return {"label": data["recommended"]["label"], "ratio": data["recommended"]["ratio"],
            "orig": rec["orig"], "stored": rec["stored"], "parts": parts,
            "zstd19": c["overall"].get("zstd19", {}).get("ratio"), "ntfs": ntfs.get("ratio"),
            "ntfs_stored": ntfs.get("stored"),
            "schemes": [{"id": k, "label": c["labels"][k], "ratio": c["overall"][k]["ratio"],
                         "orig": c["overall"][k]["orig"], "stored": c["overall"][k]["stored"]} for k in c["schemes"]],
            "ok": st.get("ok", 0), "mismatch": st.get("mismatch", 0), "files": data["sample"]["files"],
            "bytes": data["sample"]["bytes"], "sample": data["sample"]["dir"],
            "ceiling": [x.strip() for x in (data.get("ceiling") or {}).get("conclusions", []) if " 对 " in x],
            "bg": (data.get("bg_potential") or {}).get("verdict"), "roi": (data.get("roi") or {}).get("text")}


def _selftest(win, sample: str, report: Path):
    """隐藏窗口自检（打包版验证用）：页面里选文件夹 → 开始 → 等完成 → 读回页面显示，写 JSON 后关闭。"""
    js, out = win.evaluate_js, {"steps": []}
    try:
        win.events.loaded.wait(30)
        for _ in range(100):
            try:
                if js("typeof S !== 'undefined' && !!S.info"):
                    break
            except Exception:
                pass
            time.sleep(0.3)
        out["title"] = js("document.querySelector('.ptitle').textContent")
        js(f"setFolder({json.dumps(sample)})")
        time.sleep(1.5)
        out["folder"] = js("document.querySelector('#path-meta').textContent")
        js("S.opts.mode='quick'; S.opts.ntfs=true; S.opts.ceiling=true; S.opts.cpuload=false; startRun()")
        t0 = time.time()
        while time.time() - t0 < 900:
            if js("S.snap && S.snap.status !== 'running'"):
                break
            time.sleep(0.5)
        time.sleep(1)
        out.update(status=js("S.snap.status"), error=js("S.snap.error"),
                   tiles=js("[...document.querySelectorAll('#live .tile')].map(t=>t.textContent.trim())"),
                   hint=js("document.querySelector('#run-body .hint') ? document.querySelector('#run-body .hint').textContent : ''"),
                   report=js("S.snap.report"), font=js("getComputedStyle(document.querySelector('.ptitle')).fontFamily"))
    except Exception as e:      # 自检不能让窗口挂住
        out["exception"] = repr(e)
    report.write_text(json.dumps(out, ensure_ascii=False, indent=1), encoding="utf-8")
    win.destroy()


def main(debug: bool = False, selftest: str | None = None) -> int:
    import webview
    api = Api()
    if selftest:
        win = webview.create_window("selftest", url=str(WEB / "index.html"), js_api=api, hidden=True)
        api._window = win
        webview.start(_selftest, (win, str(Path(selftest).resolve()), api._dir / "selftest.json"),
                      private_mode=True, storage_path=str(api._dir / "webview"))
        return 0
    win = webview.create_window(APP_NAME, url=str(WEB / "index.html"), js_api=api, width=1280, height=860,
                                min_size=(1100, 720), frameless=True, easy_drag=False,
                                background_color="#F6FAF6")
    api._window = win
    webview.start(debug=debug, private_mode=False, storage_path=str(api._dir / "webview"))
    if api._run and api._run.status == "running":
        api._run.cancel.set()
    return 0
