""".iisz 包的 ZIP 兼容性检查：用 iis-cli 打包，再用 Python 自带的 zipfile（与 IIS 无关的实现）读。

检查：zipfile 能打开、CRC 全部正确（testzip）；目录与文件条目齐全；压不小的文件原样存放、内容与原文件一致；
图像存为“原名.iis”；每个文件条目带 IIS 附加字段（0x4949）；iis-cli unpack 解出的文件与原文件逐字节一致。

    python -I tools/pkg_zip_check.py build/win-x64/src/cli/Release/iis-cli.exe
"""
import hashlib
import os
import random
import shutil
import struct
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def bmp24(w, h, seed):
    rnd = random.Random(seed)
    stride = (w * 3 + 3) & ~3
    head = b"BM" + struct.pack("<IHHI", 54 + stride * h, 0, 0, 54)
    head += struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, stride * h, 2835, 2835, 0, 0)
    rows = bytearray()
    for y in range(h):
        for x in range(w):
            rows += bytes(((x + rnd.randrange(8)) & 255, (y * 2) & 255, (x + y) & 255))
        rows += bytes(stride - w * 3)
    return head + bytes(rows)


def main():
    cli = Path(sys.argv[1]).resolve()
    work = ROOT / ".tmp" / "pkg-zip-check"
    shutil.rmtree(work, ignore_errors=True)
    src = work / "样本 包"
    (src / "子 目录").mkdir(parents=True)
    (src / "空目录").mkdir()
    files = {
        "a.bmp": bmp24(320, 200, 1),
        "子 目录/b.bmp": bmp24(100, 80, 2),
        "照片.jpg": random.Random(3).randbytes(50000),  # 压不小：原样存放
        "日志.txt": ("station=3 result=OK\n" * 5000).encode(),
        "empty.dat": b"",
    }
    for rel, data in files.items():
        (src / rel).write_bytes(data)

    ok = True

    def check(cond, what):
        nonlocal ok
        print(("通过  " if cond else "失败  ") + what)
        ok = ok and cond

    r = subprocess.run([str(cli), "pack", str(src)], capture_output=True)
    check(r.returncode == 0, "iis-cli pack")
    pkg = work / "样本 包.iisz"
    with zipfile.ZipFile(pkg) as z:
        check(z.testzip() is None, "zipfile 打开，CRC 全部正确")
        names = {i.filename: i for i in z.infolist()}
        check({"样本 包/", "样本 包/子 目录/", "样本 包/空目录/"} <= set(names), "目录条目齐全（含空目录）")
        check("样本 包/a.bmp.iis" in names and "样本 包/子 目录/b.bmp.iis" in names, "图像存为 原名.iis")
        check(z.read("样本 包/照片.jpg") == files["照片.jpg"], "压不小的文件原样存放、内容一致")
        check(all(i.compress_type == zipfile.ZIP_STORED for i in names.values()), "条目都只存不压")
        check(all(b"\x49\x49" in i.extra for n, i in names.items() if not n.endswith("/")), "文件条目带 IIS 附加字段")
    out = work / "解出"
    r = subprocess.run([str(cli), "unpack", str(pkg), "-o", str(out)], capture_output=True)
    check(r.returncode == 0, "iis-cli unpack")
    same = all(hashlib.sha256((out / "样本 包" / rel).read_bytes()).digest() == hashlib.sha256(d).digest()
               for rel, d in files.items())
    check(same and (out / "样本 包" / "空目录").is_dir(), "解出的文件与原文件逐字节一致")
    shutil.rmtree(work, ignore_errors=True)
    print(".iisz ZIP 兼容性检查" + ("通过" if ok else "失败"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
