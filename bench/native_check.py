"""内核格式处理器与阶段 0 Python 参考实现的逐项对照（tools/t3_parity.py 与测试共用）。

check_bytes() 对一个文件内容：
  1. 识别结果逐字段比对
  2. 内核压缩：Python 能拆分的，内核必须按图像压缩；Python 兜底的，内核也不得按图像压缩
  3. 按图像压缩的：每个像素平面的像素与 JPEG-LS 码流（与阶段 0 编码逐字节相同）、原样字节区间、附加字节、
     TIFF 复现编码器与 Python 拆分一致；配方符合 spec/recipe-v1.schema.json
  4. 内核只凭容器还原，逐字节等于原文件
"""
from __future__ import annotations

import json
import time
from pathlib import Path

import imagecodecs as ic
import numpy as np

from . import codecs as cx
from . import container as ct
from .detect import detect_bytes
from .layout import SPLITTABLE, LayoutError, split

ROOT = Path(__file__).resolve().parent.parent
DET_FIELDS = ["class", "container", "width", "height", "channels", "bits_stored", "bits_effective", "value_max",
              "shift", "byteorder", "pages", "compression", "predictor", "palette", "bayer_candidates",
              "bayer_guess", "processable", "damaged"]

_validator = None


def schema_errors(recipe: dict) -> list[str]:
    global _validator
    if _validator is None:
        import jsonschema
        schema = json.loads((ROOT / "spec/recipe-v1.schema.json").read_text(encoding="utf-8"))
        _validator = jsonschema.Draft202012Validator(schema)
    return [f"配方不符合 Schema：{e.message}" for e in _validator.iter_errors(recipe)][:3]


def norm(v):
    return None if v in ("", None, []) else v


def py_encoder_id(page: dict) -> str | None:
    """阶段 0 记录的复现编码器 → 内核的命名。"""
    if not page.get("compression"):
        return None
    if page["compression"] == "lzw":
        s = "".join("L" if c == "L" else "I" for c in page["encoders"])
        return "libtiff-lzw" if set(s) == {"L"} else "imcd-lzw" if set(s) == {"I"} else "lzw:" + s
    e = page["encoder"]
    if e.startswith("ic.zlib"):
        return f"zlib{e[-1]}"
    if e.startswith("py.zlib"):
        return f"zlib-ng{e[-1]}"
    if e.startswith("ic.deflate"):
        return f"libdeflate{e[-1]}"
    return {"ic.packbits.rows": "packbits-rows", "ic.packbits": "packbits"}[e]


def cpp_encoder_id(page: dict) -> str | None:
    e = page["encoder"]
    if e is None:
        return None
    if e["name"] in ("zlib", "libdeflate"):
        return f"{e['name']}{e['level']}"
    if e["name"] == "lzw":
        return "lzw:" + page["encoders"]
    return e["name"]


def compare_detection(py: dict, cd: dict) -> list[str]:
    py_unknown = py["class"] in ("unknown", "unsupported") or (py["class"] == "raw" and not py["processable"])
    if py_unknown:
        return [] if cd["class"] in ("unknown", "unsupported") else [f"类别 {cd['class']} ≠ Python {py['class']}（不处理）"]
    return [f"识别 {f}: {cd.get(f)!r} ≠ {py.get(f)!r}" for f in DET_FIELDS if norm(cd.get(f)) != norm(py.get(f))]


def compare_split(cont: bytes, s) -> list[str]:
    c = ct.open_bytes(cont)
    recipe = c.recipe()
    diffs = schema_errors(recipe)
    planes = recipe["planes"]
    if len(planes) != len(s.planes):
        return diffs + [f"平面数 {len(planes)} ≠ {len(s.planes)}"]
    for pm, p in zip(planes, s.planes):
        i = c.find(ct.PLANE, pm["id"])
        a = cx._prep(p)
        bits = max(2, min(p.bits, a.itemsize * 8))
        if pm["bits"] != bits:
            diffs.append(f"平面 {p.name} 位数 {pm['bits']} ≠ {bits}")
        dt = np.uint8 if pm["bits"] <= 8 else np.dtype("<u2")
        shape = (pm["height"], pm["width"]) + ((pm["components"],) if pm["components"] > 1 else ())
        got = np.frombuffer(c.decode(i), dt).reshape(shape)
        if got.shape != a.shape or not np.array_equal(got.astype(np.uint32), a.astype(np.uint32)):
            diffs.append(f"平面 {p.name} 像素不同")
        elif c.read(i)[1] != cx._jpegls(p):
            diffs.append(f"平面 {p.name} JPEG-LS 码流与阶段 0 不同")
    sk = c.find(ct.SKELETON, 0)
    ranges = c.ranges(sk) if sk is not None else []
    want = [(o, len(b)) for o, b in s.skeleton]
    if ranges != want:
        diffs.append(f"原样字节区间 {ranges[:4]} ≠ {want[:4]}")
    ex = {}
    for em in recipe["extras"]:
        name = em["role"] if em["role"] != "block_tail" else f"p{em['image']}.seg{em['block']}.tail"
        ex[name] = c.decode(c.find(ct.EXTRA, em["id"]))
    if ex != dict(s.extras):
        diffs.append(f"附加字节 {sorted(ex)} ≠ {sorted(s.extras)}")
    if s.fmt == "tiff":
        for pr_c, pr_p in zip(recipe["layout"]["pages"], s.recipe["pages"]):
            a, b = cpp_encoder_id(pr_c), py_encoder_id(pr_p)
            if a != b:
                diffs.append(f"第 {pr_p['index']} 页复现编码器 {a} ≠ {b}")
    return diffs


def check_bytes(iis, rel: str, data: bytes, rules: list | None = None, hrules=None, sidecar: str | None = None,
                timing: bool = False, path: Path | None = None) -> dict:
    """返回 {method, detail, diffs, ...}；timing 时另含分段耗时（Python 拆分 + 编码、内核各段、还原）。"""
    py = detect_bytes(rel, data, path, rules)  # 参考实现按文件路径查同名参数文件
    cd = iis.detect(data, rel, hrules, sidecar)
    out = {"path": rel, "class_py": py["class"], "class_cpp": cd["class"], "diffs": compare_detection(py, cd)}
    s, py_reason, py_t = None, None, 0.0
    if py["class"] in SPLITTABLE and py.get("processable"):
        t0 = time.perf_counter()
        try:
            s = split(data, py)
            if timing:
                for p in s.planes:
                    cx._jpegls(p)
                if s.skeleton or s.extras:
                    ic.zstd_encode(b"".join(b for _, b in s.skeleton) + b"".join(s.extras.values()), level=19)
        except LayoutError as e:
            s, py_reason = None, str(e)
        py_t = time.perf_counter() - t0
    method, cont, detail, times = iis.compress(data, rel, hrules, sidecar)
    out.update(method=method, orig=len(data), stored=len(cont) if method != 3 else len(data), detail=detail,
               times=times, py_split_encode_s=py_t)
    if s is not None and method != 1 and "不小于原文件" not in detail:  # 拆得开但压不小属于取舍，不算不一致
        out["diffs"].append(f"Python 能拆分，内核未按图像压缩：{detail}")
    if s is None and method == 1:
        out["diffs"].append(f"Python 兜底（{py_reason}），内核却按图像压缩")
    if method != 3:
        t0 = time.perf_counter()
        st, msg, back = iis.check_container(cont, deep=True)
        out["restore_s"] = time.perf_counter() - t0
        if st != 0 or back != data:
            out["diffs"].append(f"内核还原不一致：{msg}")
    if method == 1 and s is not None:
        out["diffs"] += compare_split(cont, s)
    out["image_split"] = s is not None
    return out
