"""T6 CPU 影响测试（仅 Windows）：后台压缩会不会拖慢产线检测。

- 模拟检测负载：K 个进程（每进程单线程 OpenCV），各自按固定周期处理一张图（滤波、边缘、阈值、形态学、
  轮廓、模板匹配），单次处理耗时 / 周期 ≈ 指定负载比例；记录每次处理耗时。
- 后台压缩：C 个进程，以低优先级（BELOW_NORMAL / IDLE）、限定在 C 个逻辑核上，循环执行产品写入路径
  （识别 → 拆分 → JPEG-LS 编码 → 解码 → 重组 → SHA-256 校验），统计吞吐。
- 对比有 / 无后台压缩时检测耗时的均值、P95、P99、超周期次数。

为模拟客户工控机，全部进程限制在一组逻辑核内（默认前 16 个，相当于 8 核 16 线程的 i7）。
"""
from __future__ import annotations

import hashlib
import multiprocessing as mp
import platform
import time
from pathlib import Path

import numpy as np

PRIORITIES = {"normal": "NORMAL_PRIORITY_CLASS", "below_normal": "BELOW_NORMAL_PRIORITY_CLASS",
              "idle": "IDLE_PRIORITY_CLASS"}


def available() -> tuple[bool, str]:
    if platform.system() != "Windows":
        return False, "非 Windows 平台，跳过 CPU 影响测试（任务书要求仅在 Windows 上执行）"
    try:
        import psutil  # noqa: F401
    except ImportError:
        return False, "缺少 psutil，跳过"
    return True, "Windows"


def _set_proc(cpus: list[int], priority: str):
    import psutil
    p = psutil.Process()
    p.cpu_affinity(cpus)
    p.nice(getattr(psutil, PRIORITIES[priority]))


# ---------------------------------------------------------------- 模拟检测

def inspect_once(img: np.ndarray, tmpl: np.ndarray) -> int:
    """一次“检测”：典型的传统视觉算子组合，返回一个结果值防止被优化掉。"""
    import cv2
    b = cv2.GaussianBlur(img, (5, 5), 1.2)
    gx = cv2.Sobel(b, cv2.CV_16S, 1, 0)
    gy = cv2.Sobel(b, cv2.CV_16S, 0, 1)
    mag = cv2.convertScaleAbs(cv2.addWeighted(cv2.convertScaleAbs(gx), 0.5, cv2.convertScaleAbs(gy), 0.5, 0))
    _, th = cv2.threshold(b, 0, 255, cv2.THRESH_BINARY + cv2.THRESH_OTSU)
    th = cv2.morphologyEx(th, cv2.MORPH_OPEN, np.ones((5, 5), np.uint8))
    contours, _ = cv2.findContours(th, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    small = cv2.resize(b, None, fx=0.5, fy=0.5)
    res = cv2.matchTemplate(small, tmpl, cv2.TM_CCOEFF_NORMED)
    return len(contours) + int(mag.mean()) + int(res.argmax())


def load_image(path: str) -> tuple[np.ndarray, np.ndarray]:
    import cv2
    img = cv2.imdecode(np.fromfile(path, np.uint8), cv2.IMREAD_GRAYSCALE)
    small = cv2.resize(img, None, fx=0.5, fy=0.5)
    h, w = small.shape
    return img, small[h // 3: h // 3 + 48, w // 3: w // 3 + 64].copy()


def calibrate(path: str, n: int = 30) -> float:
    """在单核上测一次检测的中位耗时（秒）。"""
    import cv2
    cv2.setNumThreads(1)
    img, tmpl = load_image(path)
    for _ in range(3):
        inspect_once(img, tmpl)
    ts = []
    for _ in range(n):
        t0 = time.perf_counter()
        inspect_once(img, tmpl)
        ts.append(time.perf_counter() - t0)
    return float(np.median(ts))


def calibrate_on(path: str, core: int) -> float:
    """在指定逻辑核上单核标定（供专用子进程调用，不改动主进程亲和性）。"""
    import psutil
    psutil.Process().cpu_affinity([core])
    return calibrate(path)


def inspector(path: str, cpus: list[int], period: float, t_start: float, t_end: float, q):
    """按固定周期执行检测；只记录 [t_start, t_end) 内开始的那些次的处理耗时。"""
    import cv2
    cv2.setNumThreads(1)
    _set_proc(cpus, "normal")
    img, tmpl = load_image(path)
    lat = []
    nxt = time.perf_counter()
    while True:
        now = time.perf_counter()
        if now >= t_end:
            break
        if now < nxt:
            time.sleep(nxt - now)
        t0 = time.perf_counter()
        inspect_once(img, tmpl)
        dt = time.perf_counter() - t0
        if t_start <= t0 < t_end:
            lat.append(dt)
        nxt += period
        if nxt < time.perf_counter():          # 落后时不补跑，避免雪崩
            nxt = time.perf_counter()
    q.put(lat)


# ---------------------------------------------------------------- 后台压缩

def compressor(files: list[tuple[str, dict]], cpus: list[int], priority: str, t_start: float, t_end: float, q):
    """循环执行产品写入路径，返回 [t_start, t_end) 内完成的原文件字节数与文件数。"""
    from .codecs import PIXEL_CODECS
    from .compress import decode_rebuild, encode_split
    from .layout import split
    _set_proc(cpus, priority)
    codec = PIXEL_CODECS["jpegls"]
    done_bytes = done_files = bad = 0
    i = 0
    while time.perf_counter() < t_end:
        path, rec = files[i % len(files)]
        i += 1
        t0 = time.perf_counter()
        data = Path(path).read_bytes()
        s = split(data, rec)
        streams, bz = encode_split(s, codec)
        out = decode_rebuild(s, codec, streams, bz)
        ok = hashlib.sha256(out).hexdigest() == rec["sha256"]
        if t_start <= t0 and time.perf_counter() < t_end:
            done_bytes += len(data)
            done_files += 1
            bad += not ok
    q.put((done_bytes, done_files, bad))


# ---------------------------------------------------------------- 一组实验

def run_phase(img_path: str, files, cpus: list[int], n_inspect: int, period: float, comp_cpus: list[int],
              priority: str | None, warmup: float, duration: float) -> dict:
    import psutil
    ctx = mp.get_context("spawn")
    q_i, q_c = ctx.Queue(), ctx.Queue()
    t0 = time.perf_counter() + 2.0                 # 留出子进程启动时间（spawn 较慢）
    t_start, t_end = t0 + warmup, t0 + warmup + duration
    procs = [ctx.Process(target=inspector, args=(img_path, cpus, period, t_start, t_end, q_i))
             for _ in range(n_inspect)]
    if priority:
        procs += [ctx.Process(target=compressor, args=(files, comp_cpus, priority, t_start, t_end, q_c))
                  for _ in comp_cpus]
    for p in procs:
        p.start()
    while time.perf_counter() < t_start:
        time.sleep(0.05)
    psutil.cpu_percent(percpu=True)
    time.sleep(max(0.0, t_end - time.perf_counter()))
    usage = psutil.cpu_percent(percpu=True)
    lat = []
    for _ in range(n_inspect):
        lat += q_i.get()
    comp = [q_c.get() for _ in (comp_cpus if priority else [])]
    for p in procs:
        p.join()
    a = np.array(lat) * 1000
    cpu_use = float(np.mean([usage[c] for c in cpus]))
    res = {"n": len(a), "mean_ms": round(float(a.mean()), 3), "p50_ms": round(float(np.percentile(a, 50)), 3),
           "p95_ms": round(float(np.percentile(a, 95)), 3), "p99_ms": round(float(np.percentile(a, 99)), 3),
           "max_ms": round(float(a.max()), 3), "over_period": int((a > period * 1000).sum()),
           "cpu_busy_pct": round(cpu_use, 1)}
    if priority:
        b = sum(c[0] for c in comp)
        res.update(compress_MBps=round(b / duration / 1e6, 2), compress_files=sum(c[1] for c in comp),
                   verify_fail=sum(c[2] for c in comp))
    return res
