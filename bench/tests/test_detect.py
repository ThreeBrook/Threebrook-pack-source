import struct

import numpy as np
import pytest

from bench.detect import detect_bytes, scan_dir
from bench.detect.evaluate import evaluate
from bench.detect.stats import bayer_detect, value_stats
from bench.formats.magic import looks_like_text
from bench.synth.bmpwrite import encode_bmp


def test_synthetic_accuracy_100(small):
    root, manifest = small
    recs = scan_dir(root, root / "raw_config.yaml", log=None)
    ev = evaluate(recs, manifest)
    assert ev["mismatches"] == []
    assert ev["accuracy"] == 1.0
    assert all(c["rank"] == 1 for c in ev["raw_size_candidates"])


def test_extension_ignored(small):
    root, _ = small
    for name in ("bmp/bmp8_gray_bu.bmp", "tiff/tif8_II_strips.tif"):
        data = (root / name).read_bytes()
        r = detect_bytes("x.jpg", data)
        assert r["class"] in ("bmp", "tiff_uncompressed")


def test_dark_image_is_not_text():
    # 暗场 8 位图像字节大多是可打印 ASCII，不能被当成文本
    img = np.random.default_rng(0).integers(40, 90, 64 * 64, dtype=np.uint8).tobytes()
    assert not looks_like_text(img)
    assert looks_like_text("2026-11-01 ST03 OK 合格\n".encode("gbk") * 20)


@pytest.mark.parametrize("data", [b"BM" + b"\xff" * 60, b"II*\0" + b"\0" * 40, b"MM\0*\xff\xff\xff\xff", b"\x00"])
def test_garbage_never_raises(data):
    r = detect_bytes("g.bin", data)
    assert r["class"] in ("unknown", "unsupported")


def test_truncated_bmp_is_damaged():
    img = np.zeros((64, 64), np.uint8)
    data = encode_bmp(img)[:2000]
    r = detect_bytes("t.bmp", data)
    assert r["class"] == "unknown" and r["damaged"]


def test_value_stats_shift():
    a = (np.arange(4096, dtype=np.uint16) << 4)
    assert value_stats([a], 16) == {"value_max": 4095 << 4, "shift": 4, "bits_effective": 12}
    assert value_stats([np.array([255], np.uint8)], 8)["shift"] == 0


def test_bayer_negative_on_gray_and_period2_texture():
    rng = np.random.default_rng(1)
    y, x = np.mgrid[0:256, 0:256]
    smooth = (100 + 50 * np.sin(x / 20) + rng.normal(0, 2, (256, 256))).clip(0, 255).astype(np.uint8)
    assert not bayer_detect(smooth, 255)["is_bayer"]
    # 局部的 2 像素周期纹理（如细光栅）只占少数图块，不应判为 Bayer
    tex = smooth.copy()
    tex[:64, :64] = np.where((x[:64, :64] + y[:64, :64]) % 2 == 0, 200, 60)
    assert not bayer_detect(tex, 255)["is_bayer"]


def test_bayer_positive():
    rng = np.random.default_rng(2)
    base = (120 + rng.normal(0, 3, (256, 256)))
    gain = np.ones((256, 256))
    gain[0::2, 0::2] = 0.55   # R
    gain[1::2, 1::2] = 0.45   # B
    b = bayer_detect((base * gain).clip(0, 255).astype(np.uint8), 255)
    assert b["is_bayer"] and b["candidates"] == ["RGGB", "BGGR"]


def test_size_candidates_without_config(small):
    """去掉配置后，按文件大小推出的候选中应包含真值且排在前 3（带行跨距的除外：大小反推不覆盖跨距）。
    宽度 W / 2W / W/2 几种解释的代价只差 3%~20%，所以不要求一定第 1，候选需用户确认。"""
    from bench.formats import raw as fr
    root, manifest = small
    for f in manifest["files"]:
        e = f["expect"]
        if not e or e["container"] != "raw" or e["row_padding"]:
            continue
        fmt = "Mono16" if e["bits_stored"] == 16 else {"BayerRG8": "Mono8"}.get(e["pixel_format"], e["pixel_format"])
        cands = fr.size_candidates((root / f["path"]).read_bytes())
        keys = [(c["pixel_format"], c["width"], c["height"]) for c in cands[:3]]
        assert (fmt, e["width"], e["height"]) in keys, f["path"]
    assert fr.size_candidates((root / "other/random.bin").read_bytes())[0]["cost"] > fr.IMAGE_COST_MAX
