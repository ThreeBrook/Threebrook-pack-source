"""无文件头 RAW 自动识别的评估：识别准确率，以及压缩后大小（按真值参数 / 按自动识别 / 只用 zstd）。

测试集：
  - 阶段 0 合成样本中有真值的 13 个 RAW（bench/out/synth，只读）；
  - 由阶段 0 序列图与分形纹理生成的 RAW：8 / 12 / 16 位、Mono10p / Mono12p / Mono12Packed，
    奇数与常见宽度、不同噪声、部分模拟 Bayer。生成的文件放在 .tmp/rawguess-eval，用完删除。

    python -I tools/rawguess_eval.py build/win-x64/src/tools/Release/iis-devtool.exe [--keep]
"""
import json
import shutil
import struct
import subprocess
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
SYN = ROOT / "bench" / "out" / "synth"
WORK = ROOT / ".tmp" / "rawguess-eval"

TRUTH = {  # 宽、高、像素格式、行跨距（与 raw_config.yaml 一致）
    "raw/mono8.raw": (1280, 1024, "Mono8", 0),
    "raw/mono8_stride.raw": (1277, 1024, "Mono8", 1280),
    "raw/mono8_noconfig.raw": (1280, 1024, "Mono8", 0),
    "raw/mono16_full.raw": (1280, 1024, "Mono16", 0),
    "raw/mono12_lsb.raw": (1280, 1024, "Mono12", 0),
    "raw/mono16_12bit_msb.raw": (1280, 1024, "Mono16", 0),
    "raw/mono10p.raw": (1278, 1024, "Mono10p", 0),
    "raw/mono12p_odd.raw": (1279, 1023, "Mono12p", 0),
    "raw/mono12packed.raw": (1280, 1024, "Mono12Packed", 0),
    "raw/mono12p_sidecar.raw": (1280, 1024, "Mono12p", 0),
    "bayer/bayer_RGGB_raw8.raw": (1280, 1024, "Mono8", 0),
    "bayer/bayer_BGGR_raw12.raw": (1279, 1023, "Mono12", 0),
    "bayer/bayer_GRBG_raw8.raw": (1279, 1024, "Mono8", 0),
    "bayer/bayer_GBRG_raw12.raw": (1280, 1024, "Mono16", 0),
}


def read_bmp8(p):
    b = p.read_bytes()
    off = struct.unpack("<I", b[10:14])[0]
    w, h = struct.unpack("<ii", b[18:26])
    stride = (w + 3) & ~3
    a = np.frombuffer(b[off: off + stride * abs(h)], np.uint8).reshape(abs(h), stride)[:, :w]
    return a[::-1] if h > 0 else a


def fractal(h, w, seed, octaves=6):
    r = np.random.default_rng(seed)
    img = np.zeros((h, w))
    for o in range(octaves):
        s = 2 ** (o + 2)
        g = np.kron(r.standard_normal((h // s + 2, w // s + 2)), np.ones((s, s)))[:h, :w]
        img += g / (o + 1)
    return (img - img.min()) / (np.ptp(img) + 1e-9)


def to_bytes(v, fmt):
    v = v.reshape(-1).astype(np.uint16)
    n0 = len(v)
    if fmt == "Mono8":
        return v.astype(np.uint8).tobytes()
    if fmt in ("Mono12", "Mono16"):
        return v.astype("<u2").tobytes()
    if fmt in ("Mono12p", "Mono12Packed"):
        if len(v) % 2:
            v = np.append(v, 0)
        p0, p1 = v[0::2], v[1::2]
        if fmt == "Mono12p":
            g = [p0 & 0xFF, ((p0 >> 8) & 0xF) | ((p1 & 0xF) << 4), p1 >> 4]
        else:
            g = [p0 >> 4, (p0 & 0xF) | ((p1 & 0xF) << 4), p1 >> 4]
        return np.stack(g, 1).astype(np.uint8).reshape(-1).tobytes()[: (n0 * 12 + 7) // 8]
    if fmt == "Mono10p":
        v = np.append(v, np.zeros((-len(v)) % 4, np.uint16)).astype(np.uint64)
        g = v.reshape(-1, 4)
        word = g[:, 0] | (g[:, 1] << 10) | (g[:, 2] << 20) | (g[:, 3] << 30)
        return np.stack([(word >> (8 * k)) & 0xFF for k in range(5)], 1).astype(np.uint8).reshape(-1).tobytes()[: (n0 * 10 + 7) // 8]
    raise ValueError(fmt)


def corpus():
    r = np.random.default_rng(7)
    bases = [(p.name, read_bmp8(p) / 255.0) for p in sorted((SYN / "sequence").glob("*.bmp"))[::12]]
    bases += [(f"fractal{k}", fractal(1100, 2500, k)) for k in range(4)]
    widths = [1024, 1023, 997, 640, 2448, 1279]
    fmts = ["Mono8", "Mono12", "Mono16", "Mono10p", "Mono12p", "Mono12Packed", "Mono8"]
    bits = {"Mono8": 8, "Mono12": 12, "Mono16": 16, "Mono10p": 10, "Mono12p": 12, "Mono12Packed": 12}
    for i, (name, base) in enumerate(bases):
        for j, fmt in enumerate(fmts):
            w, h = widths[(i + j) % len(widths)], [768, 512, 1000, 600][(i * 3 + j) % 4]
            img = base[:h, :w] if w <= base.shape[1] and h <= base.shape[0] else np.kron(base, np.ones((3, 3)))[:h, :w]
            b = bits[fmt]
            sigma = [0, 1.5, 4, 12][(i + 2 * j) % 4] * 2 ** (b - 8)
            v = np.clip(img * (2 ** b - 1) * r.uniform(0.3, 1.0) + r.standard_normal(img.shape) * sigma, 0, 2 ** b - 1)
            if (i + j) % 5 == 0:  # 模拟 Bayer：各颜色增益不同
                g = np.ones_like(v)
                g[0::2, 0::2], g[1::2, 1::2] = 0.55, 0.8
                v = v * g
            yield f"{name}-{fmt}-{w}x{h}-噪声{sigma:.0f}", to_bytes(v.astype(np.uint16), fmt), (w, h, fmt, 0)


def family(f):
    return "16" if f in ("Mono10", "Mono12", "Mono14", "Mono16") else f


def main():
    tool = Path(sys.argv[1]).resolve()
    WORK.mkdir(parents=True, exist_ok=True)
    cases = [(rel, (SYN / rel), t) for rel, t in TRUTH.items() if (SYN / rel).exists()]
    for k, (name, data, t) in enumerate(corpus()):
        p = WORK / f"{k:03d}.raw"
        p.write_bytes(data)
        cases.append((name, p, t))
    exact = fmt_ok = n = 0
    tot = {"truth": 0, "auto": 0, "zstd": 0, "size": 0}
    worst = []
    ms = []
    for name, p, (w, h, f, st) in cases:
        out = subprocess.run([str(tool), "rawguess", str(p), str(w), str(h), f, str(st)], capture_output=True, text=True,
                             encoding="utf-8")
        j = json.loads(out.stdout)
        g = j.get("guess")
        n += 1
        ms.append(j["ms"])
        ok_f = bool(g) and family(g["pixel_format"]) == family(f)
        ok = ok_f and g["width"] == w and g["height"] == h and g["stride"] == st
        exact += ok
        fmt_ok += ok_f
        for k in tot:
            tot[k] += j[k]
        loss = j["auto"] / j["truth"] - 1
        worst.append((loss, name, g, j["truth"], j["auto"], j["zstd"]))
    worst.sort(reverse=True)
    print(f"共 {n} 个：参数完全正确 {exact}，像素格式正确 {fmt_ok}")
    print(f"原始 {tot['size'] / 1e6:.1f} MB → 按真值 {tot['truth'] / 1e6:.2f} MB（{tot['size'] / tot['truth']:.2f} 倍）"
          f"，按自动识别 {tot['auto'] / 1e6:.2f} MB（{tot['size'] / tot['auto']:.2f} 倍）"
          f"，只用 zstd {tot['zstd'] / 1e6:.2f} MB（{tot['size'] / tot['zstd']:.2f} 倍）")
    print(f"识别耗时：中位数 {sorted(ms)[len(ms) // 2]:.0f} ms，最长 {max(ms):.0f} ms")
    print("比按真值差最多的 10 个（自动识别 / 真值 − 1）：")
    for loss, name, g, t, a, z in worst[:10]:
        print(f"  {loss * 100:6.1f}%  {name}  识别为 {g and (g['width'], g['height'], g['pixel_format'], g['stride'])}"
              f"  真值 {t}  自动 {a}  zstd {z}")
    if "--keep" not in sys.argv:
        shutil.rmtree(WORK, ignore_errors=True)


if __name__ == "__main__":
    main()
