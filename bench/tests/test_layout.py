import hashlib
import io

import cv2
import imagecodecs
import numpy as np
import pytest
import tifffile

from bench.detect import detect_bytes, scan_dir
from bench.layout import LayoutError, rebuild, split
from bench.layout import lzw_libtiff
from bench.layout import transforms as tf
from bench.layout.packing import pack
from bench.layout.verify import roundtrip


def sha(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def test_synthetic_roundtrip_100(small):
    """T3 验收：所有可拆分文件“只拆分再重组” SHA-256 一致；已压缩 TIFF 全部复现成功。"""
    root, _ = small
    recs = scan_dir(root, root / "raw_config.yaml", log=None)
    results = [roundtrip((root / r["path"]).read_bytes(), r) for r in recs]
    assert not [r for r in results if r["status"] == "mismatch"]
    fallback = [r["path"] for r in results if r["status"] == "fallback"]
    assert fallback == ["raw/mono8_noconfig.raw"]          # 未确认参数的 RAW 按设计不拆
    comp = [r for r in results if r["class"] == "tiff_compressed"]
    assert comp and all(r["status"] == "ok" for r in comp)


def test_tampered_plane_breaks_hash(small):
    root, _ = small
    data = (root / "bmp/bmp8_gray_bu_pad1.bmp").read_bytes()
    s = split(data, detect_bytes("x.bmp", data)).detach()
    s.planes[0].data[0, 0] ^= 1
    assert sha(rebuild(s)) != sha(data)


def test_libtiff_lzw_matches_opencv_on_noise():
    """高噪声 16 位图让 LZW 码表反复填满，覆盖 libtiff 的重置逻辑。"""
    img = np.random.default_rng(3).integers(0, 4096, (96, 300)).astype(np.uint16)
    ok, buf = cv2.imencode(".tif", img)
    data = buf.tobytes()
    with tifffile.TiffFile(io.BytesIO(data)) as t:
        p = t.pages[0]
        segs = [data[o:o + n] for o, n in zip(p.dataoffsets, p.databytecounts)]
    assert all(lzw_libtiff.encode(imagecodecs.lzw_decode(s)) == s for s in segs)
    rec = detect_bytes("n.tif", data)
    assert rec["class"] == "tiff_compressed"
    assert sha(rebuild(split(data, rec).detach())) == sha(data)


def test_raw_with_header_trailer_and_padding():
    rng = np.random.default_rng(4)
    img = (rng.integers(0, 4096, (40, 51)) << 4).astype(np.uint16)       # 12 位高位对齐
    stride = 51 * 2 + 6
    rows = np.zeros((40, stride), np.uint8)
    rows[:, :102] = img.astype("<u2").view(np.uint8).reshape(40, 102)
    rows[:, 102:] = 7                                                     # 非 0 行填充
    data = b"HDR" * 33 + b"!" + rows.tobytes() + b"TAIL123"
    rules = [{"match": "*.raw", "width": 51, "height": 40, "pixel_format": "Mono16", "stride": stride, "offset": 100}]
    rec = detect_bytes("a.raw", data, rules=rules)
    assert rec["class"] == "raw" and rec["shift"] == 4 and rec["trailer_bytes"] == 7
    s = split(data, rec)
    assert s.planes[0].bits == 12 and "row_padding" in s.extras
    assert sha(rebuild(s.detach())) == sha(data)


def test_packed_tail_nonzero_preserved():
    img = np.random.default_rng(5).integers(0, 4096, (7, 9)).astype(np.uint16)   # 63 像素，尾部半字节
    body = bytearray(pack("Mono12p", img))
    body[-1] |= 0xF0                                                          # 未用位写入垃圾
    data = bytes(body)
    rules = [{"match": "*", "width": 9, "height": 7, "pixel_format": "Mono12p"}]
    rec = detect_bytes("p.raw", data, rules=rules)
    assert sha(rebuild(split(data, rec).detach())) == sha(data)


def test_unsupported_not_split():
    ok, buf = cv2.imencode(".png", np.zeros((8, 8), np.uint8))
    with pytest.raises(LayoutError):
        split(buf.tobytes(), detect_bytes("x.png", buf.tobytes()))


@pytest.mark.parametrize("shape", [(6, 8), (7, 9), (1, 5), (5, 1)])
def test_bayer_split_merge(shape):
    a = np.arange(np.prod(shape), dtype=np.uint16).reshape(shape)
    assert np.array_equal(tf.bayer_merge(tf.bayer_split(a), shape), a)


def test_delta_wraps():
    a = np.array([[0, 65535, 1, 0]], np.uint16)
    assert np.array_equal(tf.delta_decode(tf.delta_encode(a, 1), 1), a)


def test_shift_requires_zero_low_bits():
    with pytest.raises(LayoutError):
        tf.shift_down(np.array([16, 17], np.uint16), 4)
