"""内核格式处理器（T3）：随机生成各格式变体 → 内核压缩 → 只凭容器还原 → 逐字节一致，并与阶段 0 参考实现逐项对照。

另做“坏文件”模糊测试：在合法文件头部随机改字节，内核必须不崩溃，且无论走哪条路径，还原结果都与输入完全相同。
未构建内核库时跳过。
"""
import io
import json
import random

import numpy as np
import pytest
import tifffile

from bench import container as ct
from bench import native
from bench.formats.raw import load_config
from bench.layout import packing
from bench.native_check import check_bytes
from bench.synth.bmpwrite import encode_bmp, gray_palette

pytestmark = pytest.mark.skipif(native.find_lib() is None, reason="内核库未构建")


@pytest.fixture(scope="module")
def iis():
    lib = native.Iis()
    ct.crc32c = lib.crc32c
    return lib


def image(rng, h, w, bits, ch=1, smooth=True):
    y, x = np.mgrid[0:h, 0:w]
    base = ((x * 5 + y * 3) % (1 << bits)) if smooth else rng.integers(0, 1 << bits, (h, w))
    base = base[..., None].repeat(ch, 2) if ch > 1 else base
    a = base + rng.integers(0, 3, base.shape)
    return np.minimum(a, (1 << bits) - 1).astype(np.uint8 if bits <= 8 else np.uint16)


def assert_ok(r):
    assert not r["diffs"], f"{r['path']}: " + "；".join(r["diffs"])


# ---------------------------------------------------------------- 合成数据集

def test_small_synth_set(iis, small):
    root, _ = small
    rules = load_config(root / "raw_config.yaml")
    h = iis.raw_rules(rules)
    try:
        n = images = 0
        for p in sorted(root.rglob("*")):
            if not p.is_file() or p.name in ("manifest.json", "raw_config.yaml", "README.md"):
                continue
            rel = p.relative_to(root).as_posix()
            side = p.with_suffix(".json")
            sidecar = side.read_text(encoding="utf-8") if side != p and side.is_file() else None
            r = check_bytes(iis, rel, p.read_bytes(), rules, h, sidecar, path=p)
            assert_ok(r)
            n += 1
            images += r["method"] == 1
        assert n > 50 and images > 40
    finally:
        iis.free_rules(h)


# ---------------------------------------------------------------- 随机 BMP

@pytest.mark.parametrize("seed", range(40))
def test_random_bmp(iis, seed):
    rng = np.random.default_rng(seed)
    r = random.Random(seed)
    h, w = r.randint(1, 40), r.randint(1, 70)
    kind = r.choice(["gray", "gray", "palette", "bgr", "bgra", "bgra_const"])
    pal = None
    if kind in ("gray", "palette"):
        img = image(rng, h, w, 8)
        if kind == "palette":
            pal = gray_palette().copy()
            pal[:, 2] = 255 - pal[:, 2]                     # 彩色调色板：按索引平面处理
    else:
        img = image(rng, h, w, 8, 3 if kind == "bgr" else 4, smooth=r.random() < 0.7)
        if kind == "bgra_const":
            img[..., 3] = 255
    pad = None if r.random() < 0.6 else bytes(r.randrange(256) for _ in range(7))
    data = encode_bmp(img, palette=pal, top_down=r.random() < 0.4, header=r.choice([40, 124]), pad_bytes=pad,
                      trailer=b"" if r.random() < 0.7 else b"TAIL" * r.randint(1, 5))
    assert_ok(check_bytes(iis, f"bmp_{seed}.bmp", data))


# ---------------------------------------------------------------- 随机 TIFF（tifffile 写出各种版面）

@pytest.mark.parametrize("seed", range(60))
def test_random_tiff(iis, seed):
    rng = np.random.default_rng(1000 + seed)
    r = random.Random(1000 + seed)
    bits = r.choice([8, 16])
    ch = r.choice([1, 1, 1, 3, 2])
    h, w = r.randint(1, 50), r.randint(1, 60)
    eff = r.choice([bits, 12, 10]) if bits == 16 else 8
    pages = r.choice([1, 1, 2])
    kw = {"byteorder": r.choice(["<", ">"]), "bigtiff": r.random() < 0.3}
    opt = {}
    comp = r.choice([None, None, "lzw", "zlib", "packbits"])
    if comp:
        opt["compression"] = comp
        if comp in ("lzw", "zlib") and r.random() < 0.5:
            opt["predictor"] = 2
    if r.random() < 0.3:
        opt["tile"] = (16, 16)
    else:
        opt["rowsperstrip"] = r.randint(1, max(1, h))
    if ch == 3 and r.random() < 0.3:
        opt["planarconfig"] = "separate"
    if ch == 2:
        opt["photometric"] = "minisblack"
        opt["planarconfig"] = "contig"
        opt["extrasamples"] = [2]
    buf = io.BytesIO()
    with tifffile.TiffWriter(buf, **kw) as t:
        for _ in range(pages):
            a = image(rng, h, w, eff, ch)
            if eff < bits and r.random() < 0.5:
                a = (a << (bits - eff)).astype(np.uint16)  # MSB 对齐
            t.write(a, photometric=opt.pop("photometric", "rgb" if ch == 3 else "minisblack"), **opt)
    assert_ok(check_bytes(iis, f"tiff_{seed}.tif", buf.getvalue()))


# ---------------------------------------------------------------- 随机 RAW（含打包格式、行跨距、文件头）

@pytest.mark.parametrize("seed", range(40))
def test_random_raw(iis, seed):
    rng = np.random.default_rng(2000 + seed)
    r = random.Random(2000 + seed)
    fmt = r.choice(["Mono8", "Mono12", "Mono16", "Mono10p", "Mono12p", "Mono12Packed", "BayerRG8", "BayerGR12"])
    h, w = r.randint(2, 40), r.randint(2, 50)
    bits = {"Mono8": 8, "Mono12": 12, "Mono16": 16, "Mono10p": 10, "Mono12p": 12, "Mono12Packed": 12,
            "BayerRG8": 8, "BayerGR12": 12}[fmt]
    img = image(rng, h, w, bits)
    header = bytes(r.randrange(256) for _ in range(r.choice([0, 0, 16, 64])))
    rule = {"match": "cam*/*.raw", "width": w, "height": h, "pixel_format": fmt}
    if header:
        rule["offset"] = len(header)
    if fmt in packing.PACKED_FORMATS:
        body = packing.pack(fmt, img)
    else:
        bpp = 1 if bits == 8 else 2
        rows = img.astype("<u2").view(np.uint8).reshape(h, w * bpp) if bpp == 2 else img
        if r.random() < 0.4:
            extra = r.choice([1, 3, 8])
            pad = rng.integers(0, 256, (h, extra), np.uint8) if r.random() < 0.5 else np.zeros((h, extra), np.uint8)
            rows = np.concatenate([rows, pad], axis=1)
            rule["stride"] = w * bpp + extra
        body = rows.tobytes()
    data = header + body + (b"" if r.random() < 0.7 else b"\x00TRAILER")
    rules = [rule]
    hr = iis.raw_rules(rules)
    try:
        assert_ok(check_bytes(iis, f"cam{seed}/img.raw", data, rules, hr))
    finally:
        iis.free_rules(hr)


# ---------------------------------------------------------------- 坏文件模糊测试

def _fuzz_inputs():
    rng = np.random.default_rng(7)
    out = [encode_bmp(image(rng, 20, 33, 8)), encode_bmp(image(rng, 9, 13, 8, 3), top_down=True)]
    for comp in (None, "lzw", "zlib", "packbits"):
        buf = io.BytesIO()
        tifffile.imwrite(buf, image(rng, 24, 30, 12).astype(np.uint16), compression=comp, rowsperstrip=5)
        out.append(buf.getvalue())
    buf = io.BytesIO()
    tifffile.imwrite(buf, image(rng, 33, 40, 8), tile=(16, 16), bigtiff=True)
    out.append(buf.getvalue())
    return out


def test_fuzz_headers_never_crash_and_always_restore(iis):
    rng = random.Random(99)
    bases = _fuzz_inputs()
    rules = iis.raw_rules([{"match": "*.raw", "width": 30, "height": 20, "pixel_format": "Mono12p"}])
    methods = {1: 0, 2: 0, 3: 0}
    try:
        for i in range(1500):
            data = bytearray(rng.choice(bases))
            for _ in range(rng.randint(1, 6)):
                pos = rng.randrange(min(len(data), 400)) if rng.random() < 0.85 else rng.randrange(len(data))
                data[pos] = rng.randrange(256)
            if rng.random() < 0.1:
                data = data[: rng.randrange(1, len(data))]      # 截断
            data = bytes(data)
            method, cont, detail, _ = iis.compress(data, "x.raw" if i % 10 == 0 else "x.bin", rules)
            methods[method] += 1
            if method != 3:
                st, msg, back = iis.check_container(cont, deep=True)
                assert st == 0 and back == data, f"第 {i} 次：{detail} / {msg}"
    finally:
        iis.free_rules(rules)
    assert methods[1] > 100 and methods[2] > 100          # 两条路径都被走到


def test_image_container_every_byte_corruption_detected(iis):
    rng = np.random.default_rng(3)
    data = encode_bmp(image(rng, 60, 70, 8))      # 太小的图容器开销大于收益，会保留原文件
    method, cont, _, _ = iis.compress(data)
    assert method == 1
    rr = random.Random(4)
    for pos in range(len(cont)):
        bad = bytearray(cont)
        bad[pos] ^= rr.randrange(1, 256)
        st, _, _ = iis.check_container(bytes(bad), deep=True)
        assert st != 0, f"第 {pos} 字节的损坏没有被发现"


def test_detect_json_fields(iis):
    rng = np.random.default_rng(5)
    d = iis.detect(encode_bmp(image(rng, 16, 16, 8)))
    assert d["class"] in ("bmp", "bayer") and d["width"] == 16 and d["bits_stored"] == 8 and d["processable"]
    assert iis.detect(b"\x89PNG\r\n\x1a\n" + bytes(30))["class"] == "unsupported"
    assert iis.detect(b"random bytes")["class"] == "unknown"
    with pytest.raises(native.NativeError):
        iis.raw_rules([{"match": "*.raw", "width": 10, "height": 10, "pixel_format": "Mono11"}])
    _ = json.dumps(d)
