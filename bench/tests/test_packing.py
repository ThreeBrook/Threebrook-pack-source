import numpy as np
import pytest

from bench.layout import packing


@pytest.mark.parametrize("fmt", list(packing.PACKED_FORMATS))
@pytest.mark.parametrize("n", [1, 2, 3, 4, 5, 7, 1000, 1001])
def test_roundtrip(fmt, n):
    bits = packing.PACKED_FORMATS[fmt][0]
    v = np.random.default_rng(n).integers(0, 1 << bits, n).astype(np.uint16)
    data = packing.pack(fmt, v)
    assert len(data) == packing.packed_size(fmt, n)
    assert np.array_equal(packing.unpack(fmt, data, n), v)
    # 重新打包必须逐字节一致
    assert packing.pack(fmt, packing.unpack(fmt, data, n)) == data


def test_known_layouts():
    # Mono12p：b0=p0[7:0], b1=p0[11:8]|p1[3:0]<<4, b2=p1[11:4]
    assert packing.pack("Mono12p", np.array([0xABC, 0x123])) == bytes([0xBC, 0x3A, 0x12])
    # Mono12Packed：b0=p0[11:4], b1=p0[3:0]|p1[3:0]<<4, b2=p1[11:4]
    assert packing.pack("Mono12Packed", np.array([0xABC, 0x123])) == bytes([0xAB, 0x3C, 0x12])
    # Mono10p：LSB 优先连续位流
    assert packing.pack("Mono10p", np.array([0x3FF, 0, 0, 0])) == bytes([0xFF, 0x03, 0, 0, 0])


def test_overflow_rejected():
    with pytest.raises(ValueError):
        packing.pack("Mono10p", np.array([1024]))
