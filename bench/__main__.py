from .cli import main

if __name__ == "__main__":      # Windows 多进程以 spawn 启动子进程，会重新导入本模块
    raise SystemExit(main())
