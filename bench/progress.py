"""进度上报与停止开关（GUI 用；命令行不传时为空操作）。

引擎各阶段在循环里调用 rep.step(...) 上报进度、rep.check() 检查停止；
停止时抛 Cancelled，由调用方收尾（取消未开始的子任务）。回调在工作线程里执行，应只做轻量记录。
"""
from __future__ import annotations

import threading
import time
from typing import Callable

PHASES = {
    # 键: (中文名, 估计耗时权重)
    "detect": ("识别格式", 2),
    "layout": ("拆分重组自检", 5),
    "compress": ("压缩对比", 50),
    "speed": ("单核测速", 15),
    "ntfs": ("NTFS 对照", 3),
    "ceiling": ("天花板测试", 15),
    "bgpotential": ("背景潜力评估", 2),
    "cpuload": ("CPU 影响", 20),
    "report": ("生成报告", 1),
}


class Cancelled(Exception):
    """用户停止了测试。"""


class Reporter:
    def __init__(self, callback: Callable[[dict], None] | None = None, cancel: threading.Event | None = None):
        self.callback = callback
        self.cancel = cancel or threading.Event()

    @property
    def cancelled(self) -> bool:
        return self.cancel.is_set()

    def check(self):
        if self.cancel.is_set():
            raise Cancelled()

    def emit(self, **ev):
        if self.callback:
            ev.setdefault("t", time.time())
            self.callback(ev)

    def plan(self, keys: list[str]):
        self.emit(kind="plan", phases=keys)

    def start(self, key: str, total: int | None = None, msg: str = ""):
        self.emit(kind="start", phase=key, total=total, msg=msg)

    def step(self, key: str, done: int, total: int | None = None, msg: str = "", **extra):
        self.emit(kind="step", phase=key, done=done, total=total, msg=msg, **extra)

    def finish(self, key: str, msg: str = "", **extra):
        self.emit(kind="finish", phase=key, msg=msg, **extra)

    def skip(self, key: str, msg: str = ""):
        self.emit(kind="skip", phase=key, msg=msg)

    def log(self, msg: str):
        self.emit(kind="log", msg=msg)


NULL = Reporter()


def kill_pool(ex):
    """停止时立刻结束进程池：取消未开始的任务并终止正在运行的子进程（不等慢任务跑完）。"""
    for proc in list(getattr(ex, "_processes", {}).values()):
        try:
            proc.terminate()
        except Exception:
            pass
    ex.shutdown(wait=False, cancel_futures=True)
