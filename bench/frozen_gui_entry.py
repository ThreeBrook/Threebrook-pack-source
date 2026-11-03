"""PyInstaller 打包入口（图形界面版，无控制台窗口）。

Windows 上多进程以 spawn 启动子进程，打包后的子进程会重新执行本 exe，必须先调用 freeze_support()。
"""
import multiprocessing

if __name__ == "__main__":
    multiprocessing.freeze_support()
    import sys

    from bench.gui.app import main
    # 隐藏的自检开关（打包后验证用）：iis-bench-gui.exe --selftest <样本目录>，结果写到 data/selftest.json
    st = sys.argv[sys.argv.index("--selftest") + 1] if "--selftest" in sys.argv[:-1] else None
    raise SystemExit(main(selftest=st))
