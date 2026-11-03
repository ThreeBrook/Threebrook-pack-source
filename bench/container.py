"""IIS 容器 v1 的 Python 参考实现（spec/container-v1.md）。

独立于 C++ 内核实现，用于：与内核逐字节对照（同输入同段顺序，两边写出的容器必须完全相同）、
互相读取对方写的容器、以及将来的开源解码器原型。只依赖标准库与 imagecodecs（zstd / JPEG-LS 解码）。
CRC32C 为纯 Python 查表实现，速度约每秒几 MB，只适合测试与小文件。
"""
from __future__ import annotations

import hashlib
import json
import struct
from dataclasses import dataclass, field

MAGIC = b"IISCONT\0"
SEG_MAGIC, IDX_MAGIC, END_MAGIC = b"ISEG", b"IIDX", b"IEND"
MAJOR, MINOR = 1, 0
HEADER_SIZE, SEG_HEAD, ENTRY_SIZE, INDEX_HEAD, TRAILER = 72, 40, 40, 16, 32
KNOWN_INCOMPAT = 0
MAX_SEGMENTS, MAX_ENTRY_SIZE, MAX_HEADER_SIZE = 65536, 4096, 4096
MAX_META, MAX_JSON, MAX_RAW = 16 << 20, 16 << 20, 8 << 30

RECIPE, SKELETON, PLANE, EXTRA, META = 1, 2, 3, 4, 5
NONE, ZSTD, JPEGLS, HTJ2K, JPEGXL = 0, 1, 2, 3, 4
CODEC_NAMES = {NONE: "none", ZSTD: "zstd", JPEGLS: "jpegls", HTJ2K: "htj2k", JPEGXL: "jpegxl"}
CRITICAL = 1
KNOWN_TYPES = {RECIPE, SKELETON, PLANE, EXTRA, META}


class ContainerError(Exception):
    """kind：corrupt（损坏）/ unsupported（不支持）/ invalid（写入参数错误）"""

    def __init__(self, kind: str, msg: str):
        super().__init__(msg)
        self.kind = kind


def _corrupt(msg: str):
    raise ContainerError("corrupt", msg)


# ---------------- CRC32C ----------------

def _make_table():
    t = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ (0x82F63B78 if c & 1 else 0)
        t.append(c)
    return t


_T = _make_table()


def crc32c(data: bytes, crc: int = 0) -> int:
    c = crc ^ 0xFFFFFFFF
    t = _T
    for b in data:
        c = (c >> 8) ^ t[(c ^ b) & 0xFF]
    return c ^ 0xFFFFFFFF


# ---------------- LEB128 区间表 ----------------

def _leb(v: int) -> bytes:
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        out.append(b | (0x80 if v else 0))
        if not v:
            return bytes(out)


def _leb_get(buf: bytes, pos: int) -> tuple[int, int]:
    v = shift = 0
    while pos < len(buf):
        b = buf[pos]
        pos += 1
        if shift == 63 and b & 0x7E:
            _corrupt("区间表数值溢出")
        v |= (b & 0x7F) << shift
        if not b & 0x80:
            if b == 0 and shift > 0:
                _corrupt("区间表不是最短编码")
            return v, pos
        if shift >= 63:
            _corrupt("区间表数值溢出")
        shift += 7
    _corrupt("区间表编码损坏")


def encode_ranges(ranges: list[tuple[int, int]], orig_size: int) -> tuple[bytes, int]:
    if not ranges:
        raise ContainerError("invalid", "区间表为空")
    out, end, total = bytearray(_leb(len(ranges))), 0, 0
    for off, length in ranges:
        if length <= 0 or off < end or off + length > orig_size:
            raise ContainerError("invalid", "区间无效（为空、未升序、重叠或越界）")
        out += _leb(off - end) + _leb(length)
        end, total = off + length, total + length
    return bytes(out), total


def decode_ranges(meta: bytes, orig_size: int) -> tuple[list[tuple[int, int]], int]:
    n, pos = _leb_get(meta, 0)
    if n == 0 or n > len(meta):
        _corrupt("区间表计数无效")
    out, cur, total = [], 0, 0
    for _ in range(n):
        gap, pos = _leb_get(meta, pos)
        length, pos = _leb_get(meta, pos)
        if length == 0 or cur + gap + length > orig_size:
            _corrupt("区间为空或越界")
        out.append((cur + gap, length))
        cur += gap + length
        total += length
    if pos != len(meta):
        _corrupt("区间表后有多余字节")
    return out, total


# ---------------- 写入 ----------------

def recipe_bytes(obj: dict) -> bytes:
    """配方的推荐写法：紧凑、键名排序、UTF-8（与内核 nlohmann::json 输出一致）。"""
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def bytes_recipe(writer: dict) -> dict:
    return {"iis_recipe": 1, "format": "bytes", "writer": writer, "planes": [], "extras": [],
            "layout": {}, "predictor": None}


@dataclass
class Entry:
    offset: int
    type: int
    codec: int
    flags: int
    id: int
    meta_len: int
    data_crc: int
    raw_len: int
    data_len: int


class Writer:
    def __init__(self, orig_size: int, orig_sha256: bytes):
        self.buf = bytearray()
        self.orig_size = orig_size
        self.entries: list[Entry] = []
        self.ranges: list[tuple[int, int]] = []
        h = bytearray(MAGIC + struct.pack("<HHIIIQ", MAJOR, MINOR, HEADER_SIZE, 0, 0, orig_size) + orig_sha256 + b"\0" * 4)
        h += struct.pack("<I", crc32c(bytes(h)))
        self.buf += h

    def _put(self, type_: int, codec: int, id_: int, raw_len: int, meta: bytes, data: bytes):
        if any(e.type == type_ and e.id == id_ for e in self.entries):
            raise ContainerError("invalid", "段类型与 id 重复")
        flags = CRITICAL if type_ != META else 0
        dcrc = crc32c(data, crc32c(meta))
        e = Entry(len(self.buf), type_, codec, flags, id_, len(meta), dcrc, raw_len, len(data))
        head = SEG_MAGIC + struct.pack("<HHHHIQQI", type_, codec, flags, id_, len(meta), raw_len, len(data), dcrc)
        self.buf += head + struct.pack("<I", crc32c(head)) + meta + data
        self.entries.append(e)

    def add(self, type_: int, codec: int, id_: int, raw_len: int, data: bytes):
        if type_ not in KNOWN_TYPES or type_ == SKELETON:
            raise ContainerError("invalid", "段类型无效")
        allowed = {NONE, ZSTD, JPEGLS} if type_ == PLANE else {NONE, ZSTD}
        if codec not in allowed or (codec == NONE and raw_len != len(data)):
            raise ContainerError("invalid", "编码或长度不符")
        if type_ in (RECIPE, META) and id_ != 0:
            raise ContainerError("invalid", "该类型段的 id 必须为 0")
        self._put(type_, codec, id_, raw_len, b"", data)

    def add_skeleton(self, id_: int, codec: int, ranges: list[tuple[int, int]], data: bytes):
        meta, total = encode_ranges(ranges, self.orig_size)
        if codec not in (NONE, ZSTD) or (codec == NONE and total != len(data)):
            raise ContainerError("invalid", "编码或长度不符")
        allr = sorted(self.ranges + list(ranges))
        if any(allr[i][0] < allr[i - 1][0] + allr[i - 1][1] for i in range(1, len(allr))):
            raise ContainerError("invalid", "与已写入的原样字节段区间重叠")
        self._put(SKELETON, codec, id_, total, meta, data)
        self.ranges = allr

    def finish(self) -> bytes:
        if sum(e.type == RECIPE for e in self.entries) != 1:
            raise ContainerError("invalid", "容器必须恰好有一个配方段")
        index_offset = len(self.buf)
        idx = bytearray(IDX_MAGIC + struct.pack("<III", len(self.entries), ENTRY_SIZE, 0))
        for e in self.entries:
            idx += struct.pack("<QHHHHIIQQ", e.offset, e.type, e.codec, e.flags, e.id, e.meta_len, e.data_crc,
                               e.raw_len, e.data_len)
        idx += struct.pack("<I", crc32c(bytes(idx)))
        t = struct.pack("<QQQ", index_offset, len(idx), index_offset + len(idx) + TRAILER)
        self.buf += idx + t + struct.pack("<I", crc32c(t)) + END_MAGIC
        return bytes(self.buf)


def pack_bytes(data: bytes, zstd_level: int, writer: dict) -> bytes:
    """整文件兜底容器；writer 为配方中的写入方信息（与内核对照时传相同内容）。"""
    import imagecodecs as ic
    w = Writer(len(data), hashlib.sha256(data).digest())
    r = recipe_bytes(bytes_recipe(writer))
    w.add(RECIPE, NONE, 0, len(r), r)
    if data:
        if zstd_level > 0:
            w.add_skeleton(0, ZSTD, [(0, len(data))], ic.zstd_encode(data, level=zstd_level))
        else:
            w.add_skeleton(0, NONE, [(0, len(data))], data)
    return w.finish()


# ---------------- 读取 ----------------

@dataclass
class Container:
    major: int
    minor: int
    incompat: int
    compat: int
    orig_size: int
    orig_sha256: bytes
    entries: list[Entry]
    data: bytes
    why_not: str = ""
    _cache: dict = field(default_factory=dict)

    @property
    def restorable(self) -> bool:
        return not self.why_not

    def find(self, type_: int, id_: int = 0) -> int | None:
        return next((i for i, e in enumerate(self.entries) if e.type == type_ and e.id == id_), None)

    def read(self, i: int) -> tuple[bytes, bytes]:
        e = self.entries[i]
        d = self.data
        head = d[e.offset:e.offset + SEG_HEAD]
        if head[:4] != SEG_MAGIC:
            _corrupt(f"第 {i} 段段头魔数不符")
        if struct.unpack_from("<I", head, 36)[0] != crc32c(head[:36]):
            _corrupt(f"第 {i} 段段头校验失败")
        if struct.unpack_from("<HHHHIQQI", head, 4) != (e.type, e.codec, e.flags, e.id, e.meta_len, e.raw_len,
                                                        e.data_len, e.data_crc):
            _corrupt(f"第 {i} 段段头与段表不一致")
        p = e.offset + SEG_HEAD
        meta, data = d[p:p + e.meta_len], d[p + e.meta_len:p + e.meta_len + e.data_len]
        if crc32c(data, crc32c(meta)) != e.data_crc:
            _corrupt(f"第 {i} 段数据校验失败")
        return meta, data

    def decode(self, i: int) -> bytes:
        import imagecodecs as ic
        e = self.entries[i]
        _, data = self.read(i)
        if e.codec == NONE:
            out = data
        elif e.codec == ZSTD:
            out = ic.zstd_decode(data)
        elif e.codec == JPEGLS:
            a = ic.jpegls_decode(data)
            out = a.astype(a.dtype.newbyteorder("<")).tobytes()
        else:
            raise ContainerError("unsupported", f"不能解码的编码 {e.codec}")
        if len(out) != e.raw_len:
            _corrupt(f"第 {i} 段解码长度与记录不符")
        return out

    def recipe(self) -> dict:
        try:
            r = json.loads(self.decode(self.find(RECIPE)).decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as ex:
            _corrupt(f"配方不是合法 JSON：{ex}")
        if not isinstance(r, dict) or not isinstance(r.get("iis_recipe"), int):
            _corrupt("配方缺少 iis_recipe 版本号")
        if r["iis_recipe"] != 1:
            raise ContainerError("unsupported", f"配方版本 {r['iis_recipe']} 不受支持")
        for k in ("format", "writer", "planes", "extras", "layout", "predictor"):
            if k not in r:
                _corrupt(f"配方缺少字段 {k}")
        if r["predictor"] is not None:
            raise ContainerError("unsupported", "配方使用了领域预测层")
        # planes[] / extras[] 与 PLANE / EXTRA 段一一对应
        for key, t in (("planes", PLANE), ("extras", EXTRA)):
            ids = [p.get("id") for p in r[key]]
            if len(set(ids)) != len(ids) or set(ids) != {e.id for e in self.entries if e.type == t}:
                _corrupt(f"配方 {key} 与段不对应")
            if t == PLANE:
                for p in r[key]:
                    if p.get("codec") != CODEC_NAMES.get(self.entries[self.find(PLANE, p["id"])].codec):
                        _corrupt(f"配方 planes 的 id {p['id']} 编码与段不符")
        return r

    def ranges(self, i: int) -> list[tuple[int, int]]:
        meta, _ = self.read(i)
        r, total = decode_ranges(meta, self.orig_size)
        if total != self.entries[i].raw_len:
            _corrupt("区间总长与记录不符")
        return r

    def restore(self) -> bytes:
        if not self.restorable:
            raise ContainerError("unsupported", self.why_not)
        self.recipe()
        if any(e.type in (PLANE, EXTRA) for e in self.entries):
            raise ContainerError("unsupported", "该容器含像素平面，需要格式处理器")
        out = bytearray(self.orig_size)
        allr = []
        for i, e in enumerate(self.entries):
            if e.type != SKELETON:
                continue
            rs, raw, pos = self.ranges(i), self.decode(i), 0
            for off, length in rs:
                out[off:off + length] = raw[pos:pos + length]
                pos += length
            allr += rs
        allr.sort()
        if any(allr[k][0] < allr[k - 1][0] + allr[k - 1][1] for k in range(1, len(allr))) or \
                sum(x[1] for x in allr) != self.orig_size:
            _corrupt("原样字节段没有恰好覆盖整个原文件")
        if hashlib.sha256(out).digest() != self.orig_sha256:
            _corrupt("还原结果的 SHA-256 与原文件不符")
        return bytes(out)


def open_bytes(d: bytes) -> Container:
    size = len(d)
    if size < HEADER_SIZE + INDEX_HEAD + 4 + TRAILER:
        _corrupt("文件过短")
    if d[:8] != MAGIC:
        _corrupt("不是 IIS 容器（魔数不符）")
    major, minor, hsize, incompat, compat, orig_size = struct.unpack_from("<HHIIIQ", d, 8)
    if hsize < HEADER_SIZE or hsize > MAX_HEADER_SIZE or hsize > size:
        _corrupt("文件头长度无效")
    if struct.unpack_from("<I", d, hsize - 4)[0] != crc32c(d[:hsize - 4]):
        _corrupt("文件头校验失败")
    if major == 0 or major > MAJOR:
        raise ContainerError("unsupported", f"容器主版本 {major} 不受支持")
    if incompat & ~KNOWN_INCOMPAT:
        raise ContainerError("unsupported", f"不支持的不兼容特性位 {incompat}")
    sha = d[32:64]
    t = d[size - TRAILER:]
    if t[28:] != END_MAGIC:
        _corrupt("文件尾缺失或损坏")
    if struct.unpack_from("<I", t, 24)[0] != crc32c(t[:24]):
        _corrupt("文件尾校验失败")
    index_offset, index_size, declared = struct.unpack_from("<QQQ", t, 0)
    if declared != size:
        _corrupt("容器长度不符")
    if index_offset < hsize or index_offset + index_size + TRAILER != size or index_size < INDEX_HEAD + 4:
        _corrupt("段表位置无效")
    idx = d[index_offset:index_offset + index_size]
    if idx[:4] != IDX_MAGIC or struct.unpack_from("<I", idx, len(idx) - 4)[0] != crc32c(idx[:-4]):
        _corrupt("段表损坏")
    count, esize = struct.unpack_from("<II", idx, 4)
    if not ENTRY_SIZE <= esize <= MAX_ENTRY_SIZE or count > MAX_SEGMENTS or index_size != INDEX_HEAD + esize * count + 4:
        _corrupt("段表头无效")
    entries, pos, seen, why_not = [], hsize, set(), ""
    for k in range(count):
        e = Entry(*struct.unpack_from("<QHHHHIIQQ", idx, INDEX_HEAD + esize * k))
        if e.offset != pos or e.meta_len > MAX_META or e.raw_len > MAX_RAW:
            _corrupt(f"第 {k} 段位置或长度无效")
        pos += SEG_HEAD + e.meta_len + e.data_len
        if pos > index_offset:
            _corrupt(f"第 {k} 段越过段表起点")
        if (e.type, e.id) in seen:
            _corrupt(f"第 {k} 段类型与 id 重复")
        seen.add((e.type, e.id))
        if e.type in KNOWN_TYPES:
            if (e.flags & CRITICAL) != (0 if e.type == META else CRITICAL):
                _corrupt(f"第 {k} 段关键标志不符")
            if e.type != SKELETON and e.meta_len:
                _corrupt(f"第 {k} 段不应带 meta")
            if e.type == SKELETON and e.meta_len == 0:
                _corrupt(f"第 {k} 段缺少区间表")
            if e.type in (RECIPE, META) and (e.id != 0 or e.raw_len > MAX_JSON):
                _corrupt(f"第 {k} 段 id 或长度无效")
            if e.codec == NONE and e.raw_len != e.data_len:
                _corrupt(f"第 {k} 段不编码但长度不一致")
            allowed = {NONE, ZSTD, JPEGLS, HTJ2K, JPEGXL} if e.type == PLANE else {NONE, ZSTD}
            if e.codec <= JPEGXL and e.codec not in allowed:
                _corrupt(f"第 {k} 段编码与类型不符")
            if e.codec not in (NONE, ZSTD, JPEGLS) and not why_not:
                why_not = f"第 {k} 段使用不能解码的编码 {e.codec}"
        elif e.flags & CRITICAL and not why_not:
            why_not = f"第 {k} 段是不认识的关键段（类型 {e.type}）"
        entries.append(e)
    if pos != index_offset:
        _corrupt("最后一段的终点与段表起点不符")
    if sum(e.type == RECIPE for e in entries) != 1 or sum(e.type == META for e in entries) > 1:
        _corrupt("配方段或描述信息段数量不符")
    return Container(major, minor, incompat, compat, orig_size, sha, entries, d, why_not)


def verify(c: Container, deep: bool = False):
    """完整校验：每段 CRC、区间表、配方外层；deep 时再还原并比对 SHA-256。"""
    allr = []
    for i, e in enumerate(c.entries):
        if e.type == SKELETON:
            allr += c.ranges(i)
        else:
            c.read(i)
    allr.sort()
    if any(allr[k][0] < allr[k - 1][0] + allr[k - 1][1] for k in range(1, len(allr))):
        _corrupt("原样字节段之间区间重叠")
    c.recipe()
    if deep:
        c.restore()
