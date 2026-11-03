"""PyInstaller 打包入口（打成免安装 exe 时使用）。

Windows 上多进程以 spawn 启动子进程，打包后的子进程会重新执行本 exe，必须先调用 freeze_support()。
"""
import multiprocessing

if __name__ == "__main__":
    multiprocessing.freeze_support()
    from bench.cli import main
    raise SystemExit(main())
