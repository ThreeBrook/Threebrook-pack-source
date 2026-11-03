"""容器 v1：Python 参考实现自身往返；与内核库逐字节对照、互相读取；逐字节损坏两边都能发现；配方 JSON Schema。"""
import hashlib
import json
import random
from pathlib import Path

import imagecodecs as ic
import jsonschema
import numpy as np
import pytest

from bench import container as ct
from bench import native

ROOT = Path(__file__).resolve().parents[2]
SCHEMA = json.loads((ROOT / "spec/recipe-v1.schema.json").read_text(encoding="utf-8"))
needs_native = pytest.mark.skipif(native.find_lib() is None, reason="内核库未构建")


@pytest.fixture(scope="module")
def iis():
    return native.Iis()


def sample(n: int, seed: int) -> bytes:
    rng = random.Random(seed)
    b = bytearray(rng.getrandbits(8) for _ in range(n))
    b[: n // 3] = bytes(i // 64 % 256 for i in range(n // 3))
    return bytes(b)


def writer_info(iis) -> dict:
    return json.loads(iis.container_recipe(iis.pack_bytes(b"x", 0)))["writer"]


def rich_python(writer: dict) -> tuple[bytes, np.ndarray, bytes]:
    """Python 写一个含配方、两段原样字节、JPEG-LS 平面、zstd 附加字节、描述信息的容器。"""
    orig = sample(5000, 7)
    px = (np.arange(16 * 32, dtype=np.uint16).reshape(16, 32) * 7 % 4096).astype(np.uint16)
    ls = ic.jpegls_encode(px, level=0, bitspersample=12)
    recipe = ct.recipe_bytes({
        "iis_recipe": 1, "format": "raw", "writer": writer,
        "planes": [{"id": 3, "image": 0, "role": "gray", "bayer_phase": None, "width": 32, "height": 16,
                    "components": 1, "bits": 12, "shift": 0, "codec": "jpegls"}],
        "extras": [{"id": 0, "image": 0, "role": "packed_tail"}],
        "layout": {"pixel_format": "Mono12", "width": 32, "height": 16, "stride": None, "body_offset": 54,
                   "body_len": 1024, "padding_zero": True},
        "predictor": None})
    w = ct.Writer(len(orig), hashlib.sha256(orig).digest())
    w.add(ct.RECIPE, ct.NONE, 0, len(recipe), recipe)
    w.add_skeleton(0, ct.NONE, [(0, 54), (100, 20)], orig[:54] + orig[100:120])
    w.add_skeleton(1, ct.ZSTD, [(4000, 1000)], ic.zstd_encode(orig[4000:], level=3))
    w.add(ct.PLANE, ct.JPEGLS, 3, px.nbytes, ls)
    w.add(ct.EXTRA, ct.ZSTD, 0, 300, ic.zstd_encode(b"p" * 300, level=3))
    meta = json.dumps({"source_name": "相机1_0001.raw", "verdict": "NG"}, ensure_ascii=False).encode()
    w.add(ct.META, ct.NONE, 0, len(meta), meta)
    return w.finish(), px, recipe


def test_crc32c_vectors():
    assert ct.crc32c(b"123456789") == 0xE3069283
    assert ct.crc32c(bytes(32)) == 0x8A9136AA
    assert ct.crc32c(b"6789", ct.crc32c(b"12345")) == 0xE3069283


@pytest.mark.parametrize("n", [0, 1, 1000, 70000])
def test_python_pack_roundtrip(n):
    data = sample(n, n)
    for level in (0, 3):
        c = ct.open_bytes(ct.pack_bytes(data, level, {"name": "py", "version": "0", "libs": {}}))
        ct.verify(c, deep=True)
        assert c.restore() == data and c.orig_size == n


@needs_native
@pytest.mark.parametrize("n", [0, 1, 1000, 70000])
@pytest.mark.parametrize("level", [0, 3, 19])
def test_pack_bytes_identical_to_native(iis, n, level):
    data = sample(n, n + level)
    nat = iis.pack_bytes(data, level)
    py = ct.pack_bytes(data, level, writer_info(iis))
    assert py == nat
    st, msg, back = iis.check_container(py)
    assert st == 0 and back == data, msg


@needs_native
def test_rich_container_cross_read(iis):
    f, px, recipe = rich_python(writer_info(iis))
    c = ct.open_bytes(f)
    ct.verify(c)
    assert c.decode(c.find(ct.PLANE, 3)) == px.tobytes()
    assert c.ranges(c.find(ct.SKELETON, 0)) == [(0, 54), (100, 20)]
    with pytest.raises(ct.ContainerError) as e:
        c.restore()
    assert e.value.kind == "unsupported"
    # 内核读 Python 写的容器：结构校验通过；这个人造容器的 RAW 像素区与原样字节区间重叠，
    # 内核格式处理器重组时发现并报告损坏，不交出错误的结果
    st, msg, _ = iis.check_container(f, deep=False)
    assert st == 0, msg
    st, msg, _ = iis.check_container(f, deep=True)
    assert st == -3 and "重叠" in msg, msg
    assert iis.container_recipe(f) == recipe
    # 内核按同样的段顺序写，结果逐字节相同
    orig = sample(5000, 7)
    segs = []
    for i, e in enumerate(c.entries):
        meta, data = c.read(i)
        if e.type == ct.SKELETON:
            segs.append(("skeleton", e.id, e.codec, c.ranges(i), data))
        else:
            segs.append(("add", e.type, e.codec, e.id, e.raw_len, data))
    assert iis.write_container(len(orig), hashlib.sha256(orig).digest(), segs) == f


@needs_native
def test_every_byte_corruption_both_readers(iis):
    data = sample(400, 9)
    f = iis.pack_bytes(data, 3)
    rng = random.Random(10)
    for pos in range(len(f)):
        bad = bytearray(f)
        bad[pos] ^= rng.randrange(1, 256)
        bad = bytes(bad)
        st, _, _ = iis.check_container(bad)
        assert st != 0, f"内核没发现第 {pos} 字节的损坏"
        with pytest.raises(ct.ContainerError):
            ct.verify(ct.open_bytes(bad), deep=True)
    for n in range(len(f)):
        with pytest.raises(ct.ContainerError):
            ct.open_bytes(f[:n])


def test_python_rejects_future_and_reserved():
    data = sample(100, 3)
    f = bytearray(ct.pack_bytes(data, 0, {"name": "py", "version": "0", "libs": {}}))

    def patch_header(b: bytearray, off: int, val: bytes) -> bytes:
        b = bytearray(b)
        b[off:off + len(val)] = val
        b[68:72] = ct.crc32c(bytes(b[:68])).to_bytes(4, "little")
        return bytes(b)

    for off, val, kind in [(8, b"\x02\x00", "unsupported"), (16, b"\x01\x00\x00\x00", "unsupported")]:
        with pytest.raises(ct.ContainerError) as e:
            ct.open_bytes(patch_header(f, off, val))
        assert e.value.kind == kind
    assert ct.open_bytes(patch_header(f, 10, b"\x07\x00")).restore() == data   # 更高次版本照常读
    assert ct.open_bytes(patch_header(f, 20, b"\x80\x00\x00\x00")).restore() == data  # 兼容特性位忽略


# ---------------- 配方 JSON Schema ----------------

def test_schema_is_valid_draft_2020_12():
    jsonschema.Draft202012Validator.check_schema(SCHEMA)


@needs_native
def test_native_bytes_recipe_matches_schema(iis):
    jsonschema.validate(json.loads(iis.container_recipe(iis.pack_bytes(b"abc", 3))), SCHEMA)


GOOD = [
    {"iis_recipe": 1, "format": "bmp", "writer": {"name": "libiis", "version": "0.1.0", "libs": {}},
     "planes": [{"id": k, "image": 0, "role": "gray", "bayer_phase": [k // 2, k % 2], "width": 640, "height": 512,
                 "components": 1, "bits": 8, "shift": 0, "codec": "jpegls"} for k in range(4)],
     "extras": [], "layout": {"width": 1280, "height": 1024, "channels": 1, "top_down": False, "stride": 1280,
                              "pixel_offset": 1078, "padding_zero": True, "alpha_const": None},
     "predictor": None},
    {"iis_recipe": 1, "format": "tiff", "writer": {"name": "libiis", "version": "0.1.0", "libs": {"zlib": "1.3.1"}},
     "planes": [{"id": 0, "image": 0, "role": "gray", "bayer_phase": None, "width": 1280, "height": 1024,
                 "components": 1, "bits": 12, "shift": 4, "codec": "jpegls"}],
     "extras": [], "layout": {"byteorder": "MM", "bigtiff": False, "pages": [
         {"index": 0, "width": 1280, "height": 1024, "samples": 1, "bits": 16, "planar": 1, "tiled": False,
          "tile": None, "rowsperstrip": 3, "predictor": 2, "compression": "lzw", "offsets": [8, 5446],
          "bytecounts": [5438, 5414], "encoder": {"name": "libtiff-lzw"}, "encoders": None}]},
     "predictor": None},
    {"iis_recipe": 1, "format": "raw", "writer": {"name": "libiis", "version": "0.1.0", "libs": {}},
     "planes": [{"id": 0, "image": 0, "role": "gray", "bayer_phase": None, "width": 1278, "height": 1024,
                 "components": 1, "bits": 10, "shift": 0, "codec": "jpegls"}],
     "extras": [{"id": 0, "image": 0, "role": "packed_tail"}],
     "layout": {"pixel_format": "Mono10p", "width": 1278, "height": 1024, "stride": None, "body_offset": 0,
                "body_len": 1635840, "padding_zero": True}, "predictor": None},
]


@pytest.mark.parametrize("recipe", GOOD, ids=["bmp-bayer", "tiff-lzw", "raw-mono10p"])
def test_schema_accepts_examples(recipe):
    jsonschema.validate(recipe, SCHEMA)


@pytest.mark.parametrize("mutate", [
    lambda r: r.update(predictor={"ref": 1}),                       # 预测层 v1 不允许
    lambda r: r.update(iis_recipe=2),
    lambda r: r["planes"][0].update(bits=17),
    lambda r: r["planes"][0].update(components=2),
    lambda r: r["planes"][0].pop("shift"),
    lambda r: r["layout"].update(channels=2),
    lambda r: r.update(format="bytes"),                             # bytes 的 layout 必须为空、无平面
    lambda r: r["planes"][0].update(bayer_phase=[0, 2]),
])
def test_schema_rejects_bad(mutate):
    r = json.loads(json.dumps(GOOD[0]))
    mutate(r)
    with pytest.raises(jsonschema.ValidationError):
        jsonschema.validate(r, SCHEMA)
