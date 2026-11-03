"""阶段 1 内核库（iis.dll / libiis.so）的 ctypes 绑定，用于与阶段 0 Python 参考实现做对照。

不是对外 SDK：只覆盖对照测试需要的接口。库路径取环境变量 IIS_LIB，否则找默认构建目录。
"""
from __future__ import annotations

import ctypes
import json
import os
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_LIBS = [
    ROOT / "build/win-x64/src/core/Release/iis.dll",
    ROOT / "build/linux-x64/src/core/libiis.so",
]


class _Buf(ctypes.Structure):
    _fields_ = [("data", ctypes.c_void_p), ("size", ctypes.c_size_t)]


class _Info(ctypes.Structure):
    _fields_ = [("width", ctypes.c_uint32), ("height", ctypes.c_uint32),
                ("components", ctypes.c_uint32), ("bits_per_sample", ctypes.c_uint32)]


class _Range(ctypes.Structure):
    _fields_ = [("offset", ctypes.c_uint64), ("length", ctypes.c_uint64)]


ABI_VERSION = 3  # 与 iis.h 的 IIS_ABI_VERSION 一致


class _CompressOptions(ctypes.Structure):
    _fields_ = [("fallback_zstd_level", ctypes.c_int), ("raw_rules", ctypes.c_void_p), ("rel_path_utf8", ctypes.c_char_p),
                ("raw_sidecar_json", ctypes.c_char_p), ("min_saving", ctypes.c_double), ("raw_auto", ctypes.c_int)]


class _CompressResult(ctypes.Structure):
    _fields_ = [("method", ctypes.c_int), ("orig_size", ctypes.c_uint64), ("stored_size", ctypes.c_uint64),
                ("detail", ctypes.c_char * 512), ("seconds_analyze", ctypes.c_double),
                ("seconds_encode", ctypes.c_double), ("seconds_verify", ctypes.c_double)]


class NativeError(RuntimeError):
    def __init__(self, status: int, message: str):
        super().__init__(f"{status}: {message}")
        self.status = status


def find_lib() -> Path | None:
    env = os.environ.get("IIS_LIB")
    if env:
        return Path(env)
    return next((p for p in DEFAULT_LIBS if p.is_file()), None)


class Iis:
    def __init__(self, path: Path | str | None = None):
        path = Path(path) if path else find_lib()
        if not path or not path.is_file():
            raise FileNotFoundError("找不到 iis 动态库；先构建 build/win-x64 或设置 IIS_LIB")
        self.path = path
        self.lib = lib = ctypes.CDLL(str(path))
        if lib.iis_abi_version() != ABI_VERSION:  # 结构体布局与本绑定不一致时拒绝使用
            raise RuntimeError(f"iis 动态库 ABI 版本 {lib.iis_abi_version()} 与绑定（{ABI_VERSION}）不一致：{path}")
        lib.iis_build_info.restype = ctypes.c_char_p
        lib.iis_last_error.restype = ctypes.c_char_p
        lib.iis_crc32c.restype = ctypes.c_uint32
        lib.iis_crc32c.argtypes = [ctypes.c_uint32, ctypes.c_char_p, ctypes.c_size_t]
        lib.iis_zstd_decompress.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_uint64, ctypes.POINTER(_Buf)]
        lib.iis_zstd_compress.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_int, ctypes.POINTER(_Buf)]
        vp = ctypes.c_void_p
        lib.iis_writer_free.argtypes = [vp]
        lib.iis_writer_finish.argtypes = [vp, ctypes.c_int, ctypes.POINTER(_Buf)]
        lib.iis_writer_add.argtypes = [vp, ctypes.c_uint16, ctypes.c_uint16, ctypes.c_uint16, ctypes.c_uint64,
                                       ctypes.c_char_p, ctypes.c_size_t]
        lib.iis_writer_add_skeleton.argtypes = [vp, ctypes.c_uint16, ctypes.c_uint16, ctypes.POINTER(_Range),
                                                ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
        lib.iis_reader_open_memory.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(vp)]
        lib.iis_reader_close.argtypes = [vp]
        lib.iis_reader_verify.argtypes = [vp, ctypes.c_int]
        lib.iis_reader_restore.argtypes = [vp, ctypes.POINTER(_Buf)]
        lib.iis_reader_recipe.argtypes = [vp, ctypes.POINTER(_Buf)]
        lib.iis_raw_rules_parse.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(vp)]
        lib.iis_raw_rules_free.argtypes = [vp]
        lib.iis_detect.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(_CompressOptions), ctypes.POINTER(_Buf)]
        lib.iis_compress.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(_CompressOptions),
                                     ctypes.POINTER(_Buf), ctypes.POINTER(_CompressResult)]

    @property
    def build_info(self) -> str:
        return self.lib.iis_build_info().decode("utf-8")

    def _take(self, st: int, buf: _Buf) -> bytes:
        if st != 0:
            raise NativeError(st, self.lib.iis_last_error().decode("utf-8"))
        try:
            return ctypes.string_at(buf.data, buf.size)
        finally:
            self.lib.iis_buffer_free(ctypes.byref(buf))

    def jpegls_encode(self, a: np.ndarray, bits: int) -> bytes:
        a = np.ascontiguousarray(a)
        if a.dtype == np.uint16 and sys.byteorder != "little":
            raise NotImplementedError
        comps = a.shape[2] if a.ndim == 3 else 1
        info = _Info(a.shape[1], a.shape[0], comps, bits)
        buf = _Buf()
        st = self.lib.iis_jpegls_encode(a.ctypes.data_as(ctypes.c_void_p), ctypes.c_size_t(a.nbytes),
                                        ctypes.byref(info), ctypes.byref(buf))
        return self._take(st, buf)

    def jpegls_decode(self, data: bytes) -> np.ndarray:
        info, buf = _Info(), _Buf()
        st = self.lib.iis_jpegls_decode(data, ctypes.c_size_t(len(data)), ctypes.byref(info), ctypes.byref(buf))
        raw = self._take(st, buf)
        dt = np.uint8 if info.bits_per_sample <= 8 else np.uint16
        shape = (info.height, info.width) + ((info.components,) if info.components > 1 else ())
        return np.frombuffer(raw, dt).reshape(shape)

    def zstd_compress(self, data: bytes, level: int) -> bytes:
        buf = _Buf()
        return self._take(self.lib.iis_zstd_compress(data, len(data), level, ctypes.byref(buf)), buf)

    def zstd_decompress(self, data: bytes, expected: int | None = None) -> bytes:
        buf = _Buf()
        exp = 2**64 - 1 if expected is None else expected
        return self._take(self.lib.iis_zstd_decompress(data, len(data), exp, ctypes.byref(buf)), buf)

    def sha256(self, data: bytes) -> bytes:
        out = (ctypes.c_uint8 * 32)()
        self.lib.iis_sha256(data, ctypes.c_size_t(len(data)), out)
        return bytes(out)

    def crc32c(self, data: bytes, crc: int = 0) -> int:
        return self.lib.iis_crc32c(crc, data, len(data))

    # ---- 容器 ----

    def pack_bytes(self, data: bytes, zstd_level: int) -> bytes:
        buf = _Buf()
        return self._take(self.lib.iis_pack_bytes(data, ctypes.c_size_t(len(data)), zstd_level, ctypes.byref(buf)), buf)

    def write_container(self, orig_size: int, orig_sha256: bytes, segments: list[tuple]) -> bytes:
        """segments：("add", type, codec, id, raw_len, data) 或 ("skeleton", id, codec, [(off, len)], data)。"""
        w = ctypes.c_void_p()
        st = self.lib.iis_writer_create_memory(ctypes.c_uint64(orig_size), orig_sha256, ctypes.byref(w))
        if st:
            raise NativeError(st, self.lib.iis_last_error().decode("utf-8"))
        try:
            for s in segments:
                if s[0] == "add":
                    _, t, codec, id_, raw_len, data = s
                    st = self.lib.iis_writer_add(w, ctypes.c_uint16(t), ctypes.c_uint16(codec), ctypes.c_uint16(id_),
                                                 ctypes.c_uint64(raw_len), data, ctypes.c_size_t(len(data)))
                else:
                    _, id_, codec, ranges, data = s
                    arr = (_Range * len(ranges))(*[_Range(o, n) for o, n in ranges])
                    st = self.lib.iis_writer_add_skeleton(w, ctypes.c_uint16(id_), ctypes.c_uint16(codec), arr,
                                                          ctypes.c_size_t(len(ranges)), data, ctypes.c_size_t(len(data)))
                if st:
                    raise NativeError(st, self.lib.iis_last_error().decode("utf-8"))
            buf = _Buf()
            return self._take(self.lib.iis_writer_finish(w, 0, ctypes.byref(buf)), buf)
        finally:
            self.lib.iis_writer_free(w)

    def check_container(self, data: bytes, deep: bool = True) -> tuple[int, str, bytes | None]:
        """打开 + 校验（+ deep 时还原）。返回 (状态码, 错误说明, 还原结果或 None)。"""
        r = ctypes.c_void_p()
        st = self.lib.iis_reader_open_memory(data, ctypes.c_size_t(len(data)), ctypes.byref(r))
        if st:
            return st, self.lib.iis_last_error().decode("utf-8"), None
        try:
            st = self.lib.iis_reader_verify(r, 0)
            if st or not deep:
                return st, self.lib.iis_last_error().decode("utf-8") if st else "", None
            buf = _Buf()
            st = self.lib.iis_reader_restore(r, ctypes.byref(buf))
            if st:
                return st, self.lib.iis_last_error().decode("utf-8"), None
            return 0, "", self._take(0, buf)
        finally:
            self.lib.iis_reader_close(r)

    # ---- 识别与压缩（T3）----

    def raw_rules(self, rules: list[dict]) -> ctypes.c_void_p:
        text = json.dumps({"raw": rules}, ensure_ascii=False).encode("utf-8")
        h = ctypes.c_void_p()
        st = self.lib.iis_raw_rules_parse(text, ctypes.c_size_t(len(text)), ctypes.byref(h))
        if st:
            raise NativeError(st, self.lib.iis_last_error().decode("utf-8"))
        return h

    def free_rules(self, h):
        self.lib.iis_raw_rules_free(h)

    def detect(self, data: bytes, rel: str = "", rules=None, sidecar: str | None = None) -> dict:
        opt = _CompressOptions(19, rules, rel.encode("utf-8"), sidecar.encode("utf-8") if sidecar else None)
        buf = _Buf()
        st = self.lib.iis_detect(data, ctypes.c_size_t(len(data)), ctypes.byref(opt), ctypes.byref(buf))
        return json.loads(self._take(st, buf).decode("utf-8"))

    def compress(self, data: bytes, rel: str = "", rules=None, sidecar: str | None = None,
                 zstd_level: int = 19) -> tuple[int, bytes, str, dict]:
        """返回 (method, 容器字节, 说明, 分段耗时)。method：1 图像、2 整文件 zstd、3 保留原文件。"""
        opt = _CompressOptions(zstd_level, rules, rel.encode("utf-8"), sidecar.encode("utf-8") if sidecar else None)
        res, buf = _CompressResult(), _Buf()
        st = self.lib.iis_compress(data, ctypes.c_size_t(len(data)), ctypes.byref(opt), ctypes.byref(buf),
                                   ctypes.byref(res))
        out = self._take(st, buf)
        times = {"analyze": res.seconds_analyze, "encode": res.seconds_encode, "verify": res.seconds_verify}
        return res.method, out, res.detail.decode("utf-8", "replace"), times

    def container_recipe(self, data: bytes) -> bytes:
        r = ctypes.c_void_p()
        st = self.lib.iis_reader_open_memory(data, ctypes.c_size_t(len(data)), ctypes.byref(r))
        if st:
            raise NativeError(st, self.lib.iis_last_error().decode("utf-8"))
        try:
            buf = _Buf()
            return self._take(self.lib.iis_reader_recipe(r, ctypes.byref(buf)), buf)
        finally:
            self.lib.iis_reader_close(r)
