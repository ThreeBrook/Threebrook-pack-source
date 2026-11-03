"""TIFF 像素块编解码：内核（经 iis-devtool）与阶段 0 参考实现逐字节对照。未构建 iis-devtool 时跳过。

复现式还原的前提是“同样的输入，编码输出与写图程序逐字节一致”，所以这里对每个编码器都比对整串字节。
"""
import subprocess
import sys
from pathlib import Path

import imagecodecs as ic
import numpy as np
import pytest

from bench.layout import lzw_libtiff, reproduce

ROOT = Path(__file__).resolve().parents[2]
DEVTOOL = next((p for p in [ROOT / "build/win-x64/src/tools/Release/iis-devtool.exe",
                            ROOT / "build/linux-x64/src/tools/iis-devtool"] if p.is_file()), None)
pytestmark = pytest.mark.skipif(DEVTOOL is None, reason="iis-devtool 未构建")


def run(tmp_path, name: str, data: bytes, arg=None) -> bytes | None:
    src, dst = tmp_path / "in.bin", tmp_path / "out.bin"
    src.write_bytes(data)
    if dst.exists():
        dst.unlink()
    cmd = [str(DEVTOOL), "codec", name, str(src), str(dst)] + ([str(arg)] if arg is not None else [])
    r = subprocess.run(cmd, capture_output=True)
    return dst.read_bytes() if r.returncode == 0 else None


def inputs():
    rng = np.random.default_rng(1)
    y, x = np.mgrid[0:96, 0:257]
    out = {
        "empty": b"", "one": b"\x07", "two": b"\x07\x07", "three": b"abc", "zeros": bytes(5000),
        "noise": rng.integers(0, 256, 60000, np.uint8).tobytes(),          # 码表满、压缩率检查
        "lownoise": rng.integers(0, 4, 50000, np.uint8).tobytes(),
        "gradient": ((x + y) % 256).astype(np.uint8).tobytes(),
        "rows16": ((x * 37 + y * 11) % 4096).astype("<u2").tobytes(),
        "runs": b"".join(bytes([i % 7]) * (i % 5 + 1) for i in range(4000)),
        "pairs": b"aabbccdd" * 300 + b"xyz" + b"\x00\x00\x01\x01\x01" * 200,
    }
    # 合成样本中的真实条带（解压后的未压缩字节）
    for p in sorted((ROOT / "bench/out/synth/tiff").glob("*.tif"))[:4]:
        import tifffile
        with tifffile.TiffFile(p) as t:
            pg = t.pages[0]
            a = pg.asarray()
            out[f"synth:{p.name}"] = np.ascontiguousarray(a[: min(64, a.shape[0])]).tobytes()
    return out


INPUTS = inputs()


@pytest.mark.parametrize("key", list(INPUTS))
def test_lzw_encoders(tmp_path, key):
    d = INPUTS[key]
    assert run(tmp_path, "lzw-imcd", d) == ic.lzw_encode(d)
    if len(d) <= 70000:                       # Python 版 libtiff LZW 很慢，只测较小的输入
        lt = lzw_libtiff.encode(d)
        assert run(tmp_path, "lzw-libtiff", d) == lt
        if d:                                 # 空输入时 libtiff 只写 EOI，imagecodecs 同样拒绝解码
            assert run(tmp_path, "lzw-decode", lt) == d
    assert run(tmp_path, "lzw-decode", ic.lzw_encode(d)) == d


@pytest.mark.parametrize("key", list(INPUTS))
def test_packbits(tmp_path, key):
    d = INPUTS[key]
    assert run(tmp_path, "packbits", d) == ic.packbits_encode(d)
    assert run(tmp_path, "packbits-rows", d, 257) == reproduce._packbits_rows(d, 257)
    assert run(tmp_path, "packbits-decode", ic.packbits_encode(d)) == d


@pytest.mark.parametrize("key", ["zeros", "noise", "lownoise", "gradient", "rows16", "runs"] +
                         [k for k in INPUTS if k.startswith("synth:")])
def test_deflate(tmp_path, key):
    d = INPUTS[key]
    for lv in (1, 6, 9):
        z = run(tmp_path, "zlib", d, lv)
        assert z == ic.zlib_encode(d, level=lv), f"zlib {lv}"
        assert run(tmp_path, "libdeflate", d, lv) == ic.deflate_encode(d, level=lv), f"libdeflate {lv}"
        assert run(tmp_path, "zlib-decode", z) == d


def test_decoders_reject_garbage(tmp_path):
    assert run(tmp_path, "lzw-decode", b"\x12\x34\x56") is None        # 不以 CLEAR 开头
    assert run(tmp_path, "zlib-decode", b"\x78\x9c\x00\x01") is None
