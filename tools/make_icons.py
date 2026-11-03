"""生成三溪缩影的图标（assets/icons/*.ico 与界面用的 PNG）。纯 Python（只用标准库），结果确定，可重复生成。

按三溪产品图标家族规则（三溪外观规范 §8、D-149）：
  - 圆角方形（圆角 = 边长 22%），上浅下深的产品色（柿橙 #D9752B），白色主图案；
  - 64 像素及以上左下角加半透明白的“月下三溪”小月亮（下半部三道水纹镂空），48 像素用简化版（两道粗水纹），32 像素及以下不加。

  iis-app.ico      程序：白色“照片”（山 + 太阳）被上下两个箭头夹住 —— 缩小图像
  iis-file.ico     .iis 文件：白色文档，下部柿橙色块里是“照片”
  iis-package.ico  .iisz 包：同上，文档上半部有一条拉链
  app-64.png       界面标题栏、关于框用（不带签名）

用法：python -I tools/make_icons.py [--preview 输出.png]
"""
import math
import struct
import sys
import zlib
from pathlib import Path

SIZES = (256, 128, 64, 48, 32, 24, 16)
SS = 4  # 每个像素 4×4 次采样（抗锯齿）

TOP = (0xE8, 0x87, 0x3A)      # 柿橙渐变：上浅
BOTTOM = (0xC9, 0x63, 0x1E)   # 下深
PRODUCT = (0xD9, 0x75, 0x2B)  # 柿橙
WHITE = (0xFF, 0xFF, 0xFF)
PAPER_EDGE = (0xC5, 0xD7, 0xCC)  # 规范 borderStrong
FOLD = (0xE3, 0xEE, 0xE7)        # 规范 track
SOFT = (0xFB, 0xE6, 0xD4)        # 柿橙浅底
ZIP_DARK = (0x4E, 0x64, 0x61)    # 规范 textSub
SUN = (0xF4, 0xC2, 0x5B)         # 月黄


def in_round_rect(x, y, x0, y0, x1, y1, r):
    if x < x0 or x > x1 or y < y0 or y > y1:
        return False
    cx = min(max(x, x0 + r), x1 - r)
    cy = min(max(y, y0 + r), y1 - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def in_tri(x, y, a, b, c):
    def s(p, q, r):
        return (p[0] - r[0]) * (q[1] - r[1]) - (q[0] - r[0]) * (p[1] - r[1])
    d1, d2, d3 = s((x, y), a, b), s((x, y), b, c), s((x, y), c, a)
    neg = d1 < 0 or d2 < 0 or d3 < 0
    pos = d1 > 0 or d2 > 0 or d3 > 0
    return not (neg and pos)


def in_circle(x, y, cx, cy, r):
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def near_segment(x, y, ax, ay, bx, by, half):
    dx, dy = bx - ax, by - ay
    t = max(0.0, min(1.0, ((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy)))
    return (x - ax - t * dx) ** 2 + (y - ay - t * dy) ** 2 <= half * half


def photo(x, y, x0, y0, x1, y1, ink, sun=None):
    """矩形区域里的“照片”图案：两座山、太阳（ink 为山的颜色）。返回颜色或 None。"""
    u, v = (x - x0) / (x1 - x0), (y - y0) / (y1 - y0)
    if in_tri(u, v, (0.00, 1.00), (0.38, 0.34), (0.76, 1.00)) or in_tri(u, v, (0.42, 1.00), (0.70, 0.56), (1.00, 1.00)):
        return ink
    if in_circle(u, v, 0.76, 0.26, 0.14):
        return sun or ink
    return None


def wave_cut(x, y, base, period, amp, phase, half):
    """水纹（正弦线，线宽 2·half）上的点"""
    w = base + amp * math.sin((x - phase) / period * 2 * math.pi)
    slope = amp * 2 * math.pi / period * math.cos((x - phase) / period * 2 * math.pi)
    return abs(y - w) <= half * math.sqrt(1 + slope * slope)


def signature(x, y, small):
    """左下角的月下三溪（256 坐标），在月亮里且不在水纹上时返回不透明度"""
    X, Y = x * 256, y * 256
    if not small:  # 与三溪寻影 gen_icon.py 的 SIGNATURE 同形
        cx, cy, r = 61, 207, 28
        if not in_circle(X, Y, cx, cy, r):
            return None
        waves = ((211.2, 0), (220.3, -5.6), (229.4, 0))
        if any(wave_cut(X, Y, b, 33.6, 2.8, 26 + ph, 2.1) for b, ph in waves):
            return None
        return 0.72
    cx, cy, r = 59, 203, 37.8
    if not in_circle(X, Y, cx, cy, r):
        return None
    if any(wave_cut(X, Y, b, 55, 4.3, 9, 5.2) for b in (211.6, 232.2)):
        return None
    return 0.85


def app(sig, tiny=False):
    """sig：None 不带签名，"full" 完整签名，"small" 简化签名；tiny（24 像素及以下）：不画箭头，照片放大"""
    def shape(x, y):
        X, Y = x * 256, y * 256
        if not in_round_rect(X, Y, 8, 8, 248, 248, 54):
            return None
        t = (Y - 8) / 240
        bg = tuple(round(a + (b - a) * t) for a, b in zip(TOP, BOTTOM))
        if sig:
            a = signature(x, y, sig == "small")
            if a is not None:
                return tuple(round(c + (255 - c) * a) for c in bg)
        if tiny:
            if in_round_rect(X, Y, 44, 64, 212, 192, 20):
                return photo(X, Y, 66, 86, 190, 172, BOTTOM) or WHITE
            return bg
        # 照片：白色圆角卡片，山与太阳镂空成底色
        if in_round_rect(X, Y, 70, 86, 202, 170, 14):
            return photo(X, Y, 88, 100, 184, 158, BOTTOM) or WHITE
        # 上下两个指向照片的箭头（V 形），表示“缩小”
        for tip_y, arm_y in ((70, 44), (186, 212)):
            if near_segment(X, Y, 136 - 26, arm_y, 136, tip_y, 7.5) or near_segment(X, Y, 136 + 26, arm_y, 136, tip_y, 7.5):
                return WHITE
        return bg
    return shape


def document(zipper):
    def shape(x, y):
        x0, y0, x1, y1, fold = 0.16, 0.04, 0.84, 0.96, 0.20
        in_page = x0 <= x <= x1 and y0 <= y <= y1 and not (x > x1 - fold and y < y0 + fold and (x - (x1 - fold)) > (y - y0))
        if not in_page:
            return None
        edge = 0.025
        if x > x1 - fold and y < y0 + fold and (x - (x1 - fold)) <= (y - y0):  # 折角
            return FOLD
        if x < x0 + edge or x > x1 - edge or y > y1 - edge or y < y0 + edge:
            return PAPER_EDGE
        if x > x1 - fold - edge and y < y0 + fold + edge and abs((x - (x1 - fold)) - (y - y0)) < edge * 1.4:
            return PAPER_EDGE
        if y >= 0.48:  # 下部柿橙色块里的“照片”
            if in_round_rect(x, y, 0.24, 0.52, 0.76, 0.90, 0.06):
                t = (y - 0.52) / 0.38
                bg = tuple(round(a + (b - a) * t) for a, b in zip(TOP, BOTTOM))
                return photo(x, y, 0.31, 0.60, 0.69, 0.84, WHITE) or bg
            return WHITE
        if zipper:  # 拉链：左右交错的齿
            if 0.42 <= x <= 0.58 and 0.06 <= y <= 0.44:
                left = int((y - 0.06) / 0.06) % 2 == 0
                return ZIP_DARK if (x < 0.50) == left else WHITE
            return WHITE
        for ly in (0.18, 0.28, 0.38):  # 文字行
            if ly - 0.02 <= y <= ly + 0.02 and 0.26 <= x <= (0.60 if ly == 0.18 else 0.74):
                return SOFT
        return WHITE
    return shape


def render_rgba(shape, size):
    """返回 (宽, 高, RGBA 字节)；shape(x, y) 返回 RGB 或 (RGB, 不透明度)"""
    out = bytearray()
    n = SS * SS
    for py in range(size):
        for px in range(size):
            r = g = b = a = 0
            for sy in range(SS):
                for sx in range(SS):
                    c = shape((px + (sx + 0.5) / SS) / size, (py + (sy + 0.5) / SS) / size)
                    if c is not None:
                        r += c[0]
                        g += c[1]
                        b += c[2]
                        a += 1
            out += bytes((round(r / a), round(g / a), round(b / a), round(255 * a / n))) if a else b"\0\0\0\0"
    return out


def png(w, h, rgba):
    raw = b"".join(b"\0" + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def ico(images):
    head = struct.pack("<HHH", 0, 1, len(images))
    off = 6 + 16 * len(images)
    dirs, blobs = b"", b""
    for size, data in images:
        dirs += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(data), off + len(blobs))
        blobs += data
    return head + dirs + blobs


def app_for(size):
    return app("full" if size >= 64 else "small" if size == 48 else None, tiny=size <= 24)


def main():
    out = Path(__file__).resolve().parent.parent / "assets" / "icons"
    out.mkdir(parents=True, exist_ok=True)
    if "--preview" in sys.argv:  # 各尺寸并排的预览图（只看效果，不进仓库）
        target = Path(sys.argv[sys.argv.index("--preview") + 1])
        items = [(s, app_for(s)) for s in SIZES] + [(128, document(False)), (128, document(True)), (48, document(True))]
        W = sum(s for s, _ in items) + 16 * (len(items) + 1)
        H = 256 + 32
        canvas = bytearray(b"\xF6\xFA\xF6\xFF" * (W * H))  # 规范 bg
        x = 16
        for s, shape in items:
            img = render_rgba(shape, s)
            for yy in range(s):
                for xx in range(s):
                    r, g, b, a = img[(yy * s + xx) * 4:(yy * s + xx) * 4 + 4]
                    o = ((16 + yy) * W + x + xx) * 4
                    for k, c in enumerate((r, g, b)):
                        canvas[o + k] = round((c * a + canvas[o + k] * (255 - a)) / 255)
            x += s + 16
        target.write_bytes(png(W, H, canvas))
        print(target)
        return
    shapes = {
        "iis-app.ico": app_for,
        "iis-file.ico": lambda s: document(False),
        "iis-package.ico": lambda s: document(True),
    }
    for name, shape_for in shapes.items():
        (out / name).write_bytes(ico([(s, png(s, s, render_rgba(shape_for(s), s))) for s in SIZES]))
        print(out / name)
    (out / "app-64.png").write_bytes(png(64, 64, render_rgba(app(None), 64)))
    print(out / "app-64.png")


if __name__ == "__main__":
    main()
