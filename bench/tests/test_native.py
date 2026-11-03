"""内核库（iis.dll / libiis.so）与阶段 0 imagecodecs 的逐字节对照。未构建内核库时跳过。

全量合成样本对照见 tools/native_parity.py；这里用随机平面覆盖各位深 / 分量组合。
"""
import hashlib

import imagecodecs as ic
import numpy as np
import pytest

from bench import native

pytestmark = pytest.mark.skipif(native.find_lib() is None, reason="内核库未构建")


@pytest.fixture(scope="module")
def iis():
    return native.Iis()


@pytest.mark.parametrize("bits,comps", [(8, 1), (8, 3), (8, 4), (2, 1), (10, 1), (12, 1), (12, 3), (16, 1)])
def test_jpegls_streams_identical(iis, bits, comps):
    rng = np.random.default_rng(bits * 10 + comps)
    shape = (57, 83) + ((comps,) if comps > 1 else ())
    y, x = np.mgrid[0:57, 0:83]
    base = (x * 7 + y * 5) % (1 << bits)
    a = (base[..., None] if comps > 1 else base) + rng.integers(0, 3, shape)
    a = np.minimum(a, (1 << bits) - 1).astype(np.uint8 if bits <= 8 else np.uint16)
    py = ic.jpegls_encode(a, level=0, bitspersample=bits)
    nat = iis.jpegls_encode(a, bits)
    assert nat == py
    assert np.array_equal(iis.jpegls_decode(py), a)


def test_zstd_sha_crc(iis):
    data = np.random.default_rng(1).integers(0, 16, 200_000, np.uint8).tobytes()
    assert iis.zstd_compress(data, 19) == ic.zstd_encode(data, level=19)
    assert iis.zstd_decompress(ic.zstd_encode(data, level=3), len(data)) == data
    assert iis.sha256(data) == hashlib.sha256(data).digest()
    assert iis.crc32c(b"123456789") == 0xE3069283
    with pytest.raises(native.NativeError) as e:
        iis.zstd_decompress(ic.zstd_encode(data, level=3), len(data) - 1)
    assert e.value.status == -5
