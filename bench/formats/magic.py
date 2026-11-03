"""按魔数识别容器（不看扩展名）。"""
from __future__ import annotations

from .bmp import is_bmp
from .tiff import is_tiff

# 已知但不处理的格式：(名称, 判断函数)
_OTHERS = [
    ("jpeg", lambda h: h[:3] == b"\xff\xd8\xff"),
    ("png", lambda h: h[:8] == b"\x89PNG\r\n\x1a\n"),
    ("gif", lambda h: h[:4] == b"GIF8"),
    ("webp", lambda h: h[:4] == b"RIFF" and h[8:12] == b"WEBP"),
    ("jpeg2000", lambda h: h[:12] == b"\0\0\0\x0cjP  \r\n\x87\n" or h[:4] == b"\xff\x4f\xff\x51"),
    ("jpegxl", lambda h: h[:2] == b"\xff\x0a" or h[:12] == b"\0\0\0\x0cJXL \r\n\x87\n"),
    ("heif/avif", lambda h: h[4:8] == b"ftyp" and h[8:12] in (b"heic", b"heix", b"mif1", b"avif", b"avis")),
    ("mp4/mov", lambda h: h[4:8] == b"ftyp"),
    ("dicom", lambda h: h[128:132] == b"DICM"),
    ("fits", lambda h: h[:9] == b"SIMPLE  ="),
    ("zip", lambda h: h[:4] == b"PK\x03\x04"),
    ("gzip", lambda h: h[:2] == b"\x1f\x8b"),
    ("7z", lambda h: h[:6] == b"7z\xbc\xaf\x27\x1c"),
    ("rar", lambda h: h[:4] == b"Rar!"),
    ("pdf", lambda h: h[:4] == b"%PDF"),
    ("iis", lambda h: h[:8] == b"IISCONT\0"),
]

HEAD_LEN = 256


def sniff(head: bytes) -> str | None:
    """返回容器名；无法识别返回 None。"""
    if is_bmp(head):
        return "bmp"
    if is_tiff(head):
        return "tiff"
    for name, test in _OTHERS:
        if test(head):
            return name
    return None


def _decode(s: bytes) -> str | None:
    for enc in ("utf-8", "gbk"):            # 国内工控机日志常见 GBK
        for cut in range(4):                # 容忍采样截断在多字节字符中间
            try:
                return s[: len(s) - cut].decode(enc)
            except UnicodeDecodeError:
                continue
    return None


def looks_like_text(data: bytes, sample: int = 4096) -> bool:
    """文本判断要严格：暗场 8 位图像的字节大多落在可打印 ASCII 区间，只看可打印比例会误判。
    额外要求有行结构（平均行长 ≤ 400 字符）或整个文件是短的单行文本。"""
    s = data[:sample]
    if not s or b"\0" in s:
        return False
    txt = _decode(s)
    if txt is None:
        return False
    printable = sum(ch.isprintable() or ch in "\r\n\t" for ch in txt)
    if printable < 0.97 * len(txt):
        return False
    return txt.count("\n") * 400 >= len(txt) or len(data) <= 400
