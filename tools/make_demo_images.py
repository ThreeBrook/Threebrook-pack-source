"""生成宣传截图用的演示图像（程序合成的“产线图像”，不含任何客户样本）。

  .venv\\Scripts\\python -I tools/make_demo_images.py [输出目录] [--en]      默认 .tmp/marketing-demo；--en 用英文文件名（英文界面截图用）

生成一个文件夹“产线图像 2026-11-03”：
  PCB 板（8 位灰度 BMP）、晶圆 die 阵列（16 位 TIFF，12 位有效）、轴承外圈（8 位 BMP）、编织布纹（8 位 BMP）、
  电池极片（24 位彩色 BMP），以及子文件夹“工位2”里的两张。图像带传感器噪声，压缩比接近真实相机图像，不是纯色块。
随机数种子固定，结果可重复。
"""
import struct
import sys
from pathlib import Path

import numpy as np
import tifffile

rng = np.random.default_rng(20261103)


def noise(shape, sigma):
    return rng.normal(0, sigma, shape)


def blur(a, r):
    """盒式模糊（可分离），r 为半径"""
    k = 2 * r + 1
    c = np.cumsum(np.pad(a, ((r + 1, r), (0, 0)), mode="edge"), axis=0)
    a = (c[k:] - c[:-k]) / k
    c = np.cumsum(np.pad(a, ((0, 0), (r + 1, r)), mode="edge"), axis=1)
    return (c[:, k:] - c[:, :-k]) / k


def bmp(path, img):
    """8 位灰度（带调色板）或 24 位彩色 BMP，自下而上"""
    img = np.clip(img, 0, 255).astype(np.uint8)
    h, w = img.shape[:2]
    if img.ndim == 2:
        row = (w + 3) & ~3
        pal = b"".join(bytes((i, i, i, 0)) for i in range(256))
        data = b"".join(img[y].tobytes() + b"\0" * (row - w) for y in range(h - 1, -1, -1))
        off, bpp, ncol = 54 + 1024, 8, 256
    else:
        row = (w * 3 + 3) & ~3
        pal = b""
        data = b"".join(img[y, :, ::-1].tobytes() + b"\0" * (row - w * 3) for y in range(h - 1, -1, -1))
        off, bpp, ncol = 54, 24, 0
    hdr = b"BM" + struct.pack("<IHHI", off + len(data), 0, 0, off)
    hdr += struct.pack("<IiiHHIIiiII", 40, w, h, 1, bpp, 0, len(data), 2835, 2835, ncol, 0)
    path.write_bytes(hdr + pal + data)


def vignette(h, w, k=0.35):
    y, x = np.mgrid[0:h, 0:w]
    r2 = ((x - w / 2) / (w / 2)) ** 2 + ((y - h / 2) / (h / 2)) ** 2
    return 1 - k * r2


def pcb(h=1200, w=1600):
    img = np.full((h, w), 38.0)
    img += blur(noise((h, w), 18), 6)  # 基板纹理
    for _ in range(90):  # 走线：横竖折线
        x, y = rng.integers(0, w), rng.integers(0, h)
        t = rng.integers(4, 12)
        for _ in range(rng.integers(2, 5)):
            if rng.random() < 0.5:
                x2 = int(np.clip(x + rng.integers(-500, 500), 0, w - 1))
                img[max(0, y - t):y + t, min(x, x2):max(x, x2)] = 150
                x = x2
            else:
                y2 = int(np.clip(y + rng.integers(-400, 400), 0, h - 1))
                img[min(y, y2):max(y, y2), max(0, x - t):x + t] = 150
                y = y2
    yy, xx = np.mgrid[0:h, 0:w]
    for _ in range(140):  # 焊盘与过孔
        cx, cy, r = rng.integers(0, w), rng.integers(0, h), rng.integers(8, 22)
        d = (xx - cx) ** 2 + (yy - cy) ** 2
        img[d < r * r] = 210
        img[d < (r * 0.4) ** 2] = 20
    for _ in range(6):  # 芯片
        x0, y0 = rng.integers(50, w - 300), rng.integers(50, h - 250)
        cw, ch = rng.integers(160, 280), rng.integers(120, 220)
        img[y0:y0 + ch, x0:x0 + cw] = 28 + blur(noise((ch, cw), 6), 2)
        for k in range(x0 + 10, x0 + cw - 10, 22):  # 引脚
            img[y0 - 18:y0, k:k + 10] = 220
            img[y0 + ch:y0 + ch + 18, k:k + 10] = 220
    img = blur(img, 1) * vignette(h, w, 0.25) + noise((h, w), 3.5)
    return img


def wafer(h=1024, w=1280):
    yy, xx = np.mgrid[0:h, 0:w]
    img = np.full((h, w), 900.0)
    pitch, gap = 128, 10
    cell = ((xx % pitch) > gap) & ((yy % pitch) > gap)
    img[cell] = 2400
    # die 内部的结构：细线阵列与焊盘
    lx, ly = xx % pitch, yy % pitch
    img[cell & ((lx // 6) % 2 == 0) & (ly > 40) & (ly < 90)] = 3100
    img[cell & (ly > 100) & (ly < 112) & ((lx // 14) % 2 == 0)] = 3600
    defects = rng.integers(0, w, 6), rng.integers(0, h, 6)
    for cx, cy in zip(*defects):  # 几个缺陷点
        img[(xx - cx) ** 2 + (yy - cy) ** 2 < 36] = 400
    img = blur(img, 1) * vignette(h, w, 0.3) + noise((h, w), 18)
    return np.clip(img, 0, 4095).astype(np.uint16)


def bearing(h=1200, w=1200):
    yy, xx = np.mgrid[0:h, 0:w]
    r = np.hypot(xx - w / 2, yy - h / 2)
    a = np.arctan2(yy - h / 2, xx - w / 2)
    img = np.full((h, w), 25.0) + blur(noise((h, w), 8), 4)
    ring = (r > 330) & (r < 540)
    brushed = 120 + 40 * np.cos(a * 2 - 0.6) + 18 * np.sin(r * 0.9) + blur(noise((h, w), 30), 1)
    img[ring] = brushed[ring]
    img[(r > 395) & (r < 410)] = 215  # 滚道高光
    img[(r > 470) & (r < 478)] = 70
    img[(r > 530) & (r < 540)] = 200
    scratch = (np.abs((xx - 760) * 0.4 - (yy - 300)) < 1.5) & ring & (xx > 700) & (xx < 860)
    img[scratch] = 240  # 一道划痕
    return blur(img, 1) + noise((h, w), 3)


def weave(h=1000, w=1400):
    yy, xx = np.mgrid[0:h, 0:w]
    p = 18
    warp = np.sin(xx * 2 * np.pi / p) ** 2
    weft = np.sin(yy * 2 * np.pi / p) ** 2
    over = ((xx // p + yy // p) % 2 == 0)
    img = np.where(over, 90 + 110 * warp, 80 + 110 * weft)
    img += blur(noise((h, w), 20), 3)
    img[(yy > 420) & (yy < 430) & (xx > 300) & (xx < 1000)] *= 0.55  # 断纬
    return img * vignette(h, w, 0.2) + noise((h, w), 3)


def electrode(h=900, w=1400):
    yy, xx = np.mgrid[0:h, 0:w]
    base = np.zeros((h, w, 3))
    coat = (yy > 120) & (yy < 780)
    tex = blur(noise((h, w), 30), 2)
    base[..., 0] = np.where(coat, 40 + tex * 0.6, 185 + tex * 0.3)  # 涂层深灰，极耳铜色
    base[..., 1] = np.where(coat, 42 + tex * 0.6, 120 + tex * 0.3)
    base[..., 2] = np.where(coat, 46 + tex * 0.6, 80 + tex * 0.3)
    for cx, cy in ((420, 300), (980, 610)):  # 两个气泡缺陷
        d = (xx - cx) ** 2 + (yy - cy) ** 2
        base[d < 400] += 60
    base *= vignette(h, w, 0.2)[..., None]
    return base + noise((h, w, 3), 2.5)


def main():
    args = [a for a in sys.argv[1:] if a != "--en"]
    en = "--en" in sys.argv  # 英文文件名（英文界面的截图用），图像内容相同
    names = (["Line images 2026-11-03", "Station 2", "A01 PCB.bmp", "A02 Wafer dies.tif", "A03 Bearing ring.bmp",
              "A04 Woven fabric.bmp", "B01 Battery electrode.bmp", "B02 PCB (recheck).bmp"] if en else
             ["产线图像 2026-11-03", "工位2", "A01 PCB 板.bmp", "A02 晶圆 die 阵列.tif", "A03 轴承外圈.bmp",
              "A04 编织布纹.bmp", "B01 电池极片.bmp", "B02 PCB 板（复检）.bmp"])
    out = Path(args[0] if args else ".tmp/marketing-demo") / names[0]
    (out / names[1]).mkdir(parents=True, exist_ok=True)
    bmp(out / names[2], pcb())
    tifffile.imwrite(out / names[3], wafer(), photometric="minisblack")
    bmp(out / names[4], bearing())
    bmp(out / names[5], weave())
    bmp(out / names[1] / names[6], electrode())
    bmp(out / names[1] / names[7], pcb(900, 1200))
    for p in sorted(out.rglob("*.*")):
        print(f"{p.stat().st_size / 1e6:6.2f} MB  {p.relative_to(out.parent)}")


if __name__ == "__main__":
    main()
