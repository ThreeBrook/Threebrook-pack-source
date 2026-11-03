"""Windows NTFS 透明压缩（LZNT1）对照：把文件复制到启用压缩的临时目录，
用 GetCompressedFileSizeW 取实际占用。非 Windows / 非 NTFS 自动跳过并说明。"""
from __future__ import annotations

import os
import platform
import shutil
import subprocess
from pathlib import Path


def available(where: Path) -> tuple[bool, str]:
    if platform.system() != "Windows":
        return False, "非 Windows 平台，跳过 NTFS 压缩对比"
    try:
        import ctypes
        root = os.path.splitdrive(str(where.resolve()))[0] + "\\"
        buf = ctypes.create_unicode_buffer(64)
        ok = ctypes.windll.kernel32.GetVolumeInformationW(root, None, 0, None, None, None, buf, 64)
        if not ok or buf.value.upper() != "NTFS":
            return False, f"{root} 文件系统为 {buf.value or '未知'}，不是 NTFS，跳过"
    except Exception as e:
        return False, f"无法判断文件系统（{e}），跳过"
    return True, "NTFS"


def compressed_size(path: Path) -> int:
    import ctypes
    from ctypes import wintypes
    hi = wintypes.DWORD(0)
    f = ctypes.windll.kernel32.GetCompressedFileSizeW
    f.restype = wintypes.DWORD
    lo = f(str(path), ctypes.byref(hi))
    if lo == 0xFFFFFFFF and ctypes.GetLastError():
        raise OSError(ctypes.GetLastError(), "GetCompressedFileSizeW 失败")
    return (hi.value << 32) | lo


def measure(files: list[tuple[str, Path]], workdir: Path) -> dict:
    """files：[(相对路径, 绝对路径)]。返回 {相对路径: NTFS 压缩后占用字节}；临时目录用完即删。"""
    tmp = workdir / "_ntfs_tmp"
    if tmp.exists():
        shutil.rmtree(tmp)
    tmp.mkdir(parents=True)
    try:
        subprocess.run(["compact", "/c", "/q", str(tmp)], check=True, capture_output=True)
        names = {}
        for i, (rel, src) in enumerate(files):
            dst = tmp / f"{i:06d}{src.suffix}"
            shutil.copyfile(src, dst)
            names[rel] = dst
        # 新写入数据的压缩是延迟的（缓存刷盘时才压缩），刚复制完测得的占用不准；
        # 用 /f 强制同步重新压缩全部文件后再测
        subprocess.run(["compact", "/c", "/f", "/q", "/i", f"/s:{tmp}"], check=True, capture_output=True)
        return {rel: compressed_size(p) for rel, p in names.items()}
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
