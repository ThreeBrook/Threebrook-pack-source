"""T4 压缩对比：逐文件、逐方案编码 → 解码 → 重组 → SHA-256 校验，体积按“算全”口径。

分两段：
  ① 压缩比 + 往返校验：全部文件，多进程并行（每进程单线程；压缩比是确定性的，不受并行影响）；
  ② 速度：按格式组抽样，单核绑定、预热后多次取中位数（任务书第 4 节第 7 条）。
"""
from __future__ import annotations

import hashlib
import statistics
import time
from pathlib import Path

import imagecodecs as ic
import numpy as np

from . import codecs as cx
from .layout import SPLITTABLE, LayoutError, rebuild, split
from .layout.model import Plane

BASE_CLASS = {"bmp": "bmp", "tiff": "tiff_uncompressed", "raw": "raw"}


def group_of(rec: dict) -> str:
    c, cont = rec["class"], (rec.get("container") or "").upper()
    if c == "bayer":
        return f"Bayer（{cont}）"
    if c == "tiff_compressed":
        return f"TIFF-{(rec.get('compression') or '').upper()}"
    if c in ("bmp", "tiff_uncompressed", "raw"):
        ch = rec.get("channels") or 1
        bits = rec.get("bits_stored")
        if c == "raw":
            return f"RAW {rec.get('pixel_format')}"
        return f"{cont} {bits}位" + (f"×{ch}" if ch > 1 else "")
    return "其他（不处理）"


def _sha(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def _splits(data: bytes, rec: dict) -> dict:
    """返回 {变体: Split}。Bayer 文件同时给出“拆四平面”（默认）与“直接压”两个变体。"""
    if rec["class"] not in SPLITTABLE or not rec.get("processable"):
        return {}
    out = {"default": split(data, rec)}
    if rec["class"] == "bayer":
        out["bayer_direct"] = split(data, {**rec, "class": BASE_CLASS[rec["container"]]})
    return out


def encode_split(s, codec: cx.Codec):
    streams = [codec.encode(p) for p in s.planes]
    blob = cx.nonpixel_blob(s)
    blob_z = ic.zstd_encode(blob, level=19) if blob else None
    return streams, blob_z


def decode_rebuild(s, codec: cx.Codec, streams, blob_z) -> bytes:
    if blob_z is not None:
        ic.zstd_decode(blob_z)                    # 计入解码时间；内容与骨架相同
    d = s.detach()
    planes = []
    for p, b in zip(d.planes, streams):
        a = np.asarray(codec.decode(b))
        if a.size != p.data.size:
            raise LayoutError(f"平面 {p.name} 解码尺寸 {a.shape} ≠ {p.data.shape}")
        planes.append(Plane(p.name, a.reshape(p.data.shape), p.bits))
    d.planes = planes
    return rebuild(d)


def fallback_size(data: bytes) -> tuple[int, str]:
    """兜底存储：整文件 zstd，解压回来逐字节比对（写入即自检）；压大了或核对不过就原样存。"""
    z = ic.zstd_encode(data, level=19)
    size = cx.zstd_file_size(len(z))
    if size < len(data) and ic.zstd_decode(z) == data:
        return size, "zstd（已核对）"
    return len(data), "原样"


def evaluate_file(path: str, rec: dict, schemes: list[str], min_gain: float = 0.0) -> list[dict]:
    """一个文件在各方案下的结果行。任何异常只影响本文件本方案（记录后兜底）。"""
    data = Path(path).read_bytes()
    base = {"path": rec["path"], "class": rec["class"], "group": group_of(rec),
            "dir": rec["path"].split("/")[0] if "/" in rec["path"] else ".", "orig": len(data)}
    rows = []
    fb = None

    def get_fb():
        nonlocal fb
        if fb is None:
            fb = fallback_size(data)
        return fb

    for sc in schemes:
        if sc in cx.FILE_CODECS:
            t0 = time.perf_counter()
            z = ic.zstd_encode(data, level=cx.FILE_CODECS[sc])
            t1 = time.perf_counter()
            ok = _sha(ic.zstd_decode(z)) == rec["sha256"]
            t2 = time.perf_counter()
            size = cx.zstd_file_size(len(z))
            status = "ok" if ok else "mismatch"
            if size >= len(data):
                size, status = len(data), "stored_raw"
            rows.append({**base, "scheme": sc, "variant": "file", "stored": size, "status": status,
                         "reason": None, "pixel": None, "nonpixel": None, "recipe": None,
                         "enc_s": round(t1 - t0, 5), "dec_s": round(t2 - t1, 5)})
    try:
        splits = _splits(data, rec)
        split_err = None
    except LayoutError as e:
        splits, split_err = {}, str(e)
    except Exception as e:  # 不中断
        splits, split_err = {}, f"拆分异常 {type(e).__name__}: {e}"

    for sc in schemes:
        if sc not in cx.PIXEL_CODECS:
            continue
        codec = cx.PIXEL_CODECS[sc]
        if not splits:
            size, how = get_fb()
            if split_err:
                reason = split_err
            elif rec["class"] in SPLITTABLE:
                reason = "RAW 参数未确认（无配置，只给出候选参数）" if rec["container"] == "raw" else "未确认，不做图像编码"
            else:
                reason = f"类别 {rec['class']} 不做图像编码"
            rows.append({**base, "scheme": sc, "variant": "default", "stored": size,
                         "status": "not_image" if rec["class"] not in SPLITTABLE else "fallback",
                         "reason": f"{reason}；按{how}存储", "pixel": None, "nonpixel": None, "recipe": None,
                         "enc_s": None, "dec_s": None})
            continue
        for var, s in splits.items():
            row = {**base, "scheme": sc, "variant": var, "reason": None}
            try:
                t0 = time.perf_counter()
                streams, blob_z = encode_split(s, codec)
                t1 = time.perf_counter()
                rebuilt = decode_rebuild(s, codec, streams, blob_z)
                t2 = time.perf_counter()
                cs = cx.container_size(s, streams, blob_z)
                row.update(pixel=cs["pixel"], nonpixel=cs["nonpixel"], recipe=cs["recipe"],
                           enc_s=round(t1 - t0, 5), dec_s=round(t2 - t1, 5))
                if _sha(rebuilt) != rec["sha256"]:
                    size, how = get_fb()
                    row.update(stored=size, status="fallback", reason=f"往返 SHA-256 不一致；按{how}存储")
                elif cs["total"] >= len(data) * (1 - min_gain):
                    row.update(stored=len(data), status="stored_raw", reason="压缩后不小于原文件（或收益低于阈值），原样存储")
                else:
                    row.update(stored=cs["total"], status="ok")
            except Exception as e:
                size, how = get_fb()
                row.update(stored=size, status="fallback", reason=f"{type(e).__name__}: {e}；按{how}存储",
                           pixel=None, nonpixel=None, recipe=None, enc_s=None, dec_s=None)
            rows.append(row)
    return rows


# ---------------------------------------------------------------- 单核测速

def time_file(path: str, rec: dict, sc: str, repeat: int, slow_s: float = 10.0) -> dict:
    """预热 1 次 + repeat 次取中位数。单次超过 slow_s 秒的方案只测 1 次（结果中注明）。
    计时范围：编码 = 拆分 + 像素编码 + 非像素 zstd；解码 = 像素解码 + 非像素解压 + 重组。"""
    data = Path(path).read_bytes()
    if sc in cx.FILE_CODECS:
        lv = cx.FILE_CODECS[sc]
        enc = lambda: ic.zstd_encode(data, level=lv)  # noqa: E731
        z = enc()
        dec = lambda: ic.zstd_decode(z)  # noqa: E731
    else:
        codec = cx.PIXEL_CODECS[sc]
        holder = {}

        def enc():
            s = split(data, rec)
            holder["s"], (holder["st"], holder["bz"]) = s, encode_split(s, codec)

        def dec():
            return decode_rebuild(holder["s"], codec, holder["st"], holder["bz"])

    t0 = time.perf_counter()
    enc()
    first_enc = time.perf_counter() - t0
    t0 = time.perf_counter()
    out = dec()
    first_dec = time.perf_counter() - t0
    if _sha(out) != rec["sha256"]:
        return {"ok": False}
    n = 1 if max(first_enc, first_dec) > slow_s else repeat
    if n == 1 and max(first_enc, first_dec) > slow_s:
        te, td = [first_enc], [first_dec]
        note = f"单次 >{slow_s:g}s，只测 1 次（含冷启动）"
    else:
        te, td = [], []
        for _ in range(n):
            t0 = time.perf_counter()
            enc()
            te.append(time.perf_counter() - t0)
            t0 = time.perf_counter()
            dec()
            td.append(time.perf_counter() - t0)
        note = f"预热 1 次 + {n} 次取中位数"
    return {"ok": True, "enc_s": statistics.median(te), "dec_s": statistics.median(td), "note": note}


def pin_self() -> str:
    """把“当前进程”绑定到一个逻辑核并提高优先级。只在专用的测速子进程里调用，主进程亲和性从不改动。
    Windows 上逻辑核超过 64 个时分处理器组，跨组的核会被拒绝（WinError 87），所以依次尝试候选核。"""
    try:
        import psutil
        p = psutil.Process()
        cands = p.cpu_affinity()
        # 先采样各核占用，优先选最空闲的核（机器上有其他负载时，固定选某个核会被干扰）
        busy = psutil.cpu_percent(interval=0.3, percpu=True)
        cands = sorted(cands, key=lambda c: (busy[c] if c < len(busy) else 100, c))
    except Exception as e:
        return f"未能绑定单核（{e}）"
    for core in cands:
        try:
            p.cpu_affinity([core])
        except OSError:
            continue
        try:
            p.nice(psutil.HIGH_PRIORITY_CLASS if hasattr(psutil, "HIGH_PRIORITY_CLASS") else -5)
        except Exception:
            pass
        return f"绑定逻辑核 {core}"
    return "未能绑定单核（所有候选核都被拒绝）"


def cpu_name() -> str:
    import platform
    try:
        if platform.system() == "Windows":
            import winreg
            k = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DESCRIPTION\System\CentralProcessor\0")
            return winreg.QueryValueEx(k, "ProcessorNameString")[0].strip()
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except Exception:
        pass
    return platform.processor() or "未知"


# ---------------------------------------------------------------- 汇总

def _agg(rows: list[dict]) -> dict:
    orig = sum(r["orig"] for r in rows)
    stored = sum(r["stored"] for r in rows)
    st: dict[str, int] = {}
    for r in rows:
        st[r["status"]] = st.get(r["status"], 0) + 1
    return {"files": len(rows), "orig": orig, "stored": stored,
            "ratio": round(orig / stored, 4) if stored else None, "status": st}


def summarize(rows: list[dict], schemes: list[str]) -> dict:
    main = [r for r in rows if r["variant"] in ("default", "file")]
    out: dict = {"overall": {}, "image_only": {}, "by_group": {}, "by_dir": {}, "bayer": {}, "parts": {}}
    for sc in schemes:
        rs = [r for r in main if r["scheme"] == sc]
        out["overall"][sc] = _agg(rs)
        # 总量的构成：按该方案做了图像压缩（校验通过）的文件 vs 其余（原样 / zstd 兜底）
        out["parts"][sc] = {"coded": _agg([r for r in rs if r["status"] == "ok"]),
                            "other": _agg([r for r in rs if r["status"] != "ok"])}
        out["image_only"][sc] = _agg([r for r in rs if r["class"] in SPLITTABLE and r["status"] != "not_image"])
        for key, field in (("by_group", "group"), ("by_dir", "dir")):
            for g in sorted({r[field] for r in rs}):
                out[key].setdefault(g, {})[sc] = _agg([r for r in rs if r[field] == g])
        bsplit = {r["path"]: r for r in rows
                  if r["scheme"] == sc and r["class"] == "bayer" and r["variant"] == "default"}
        bdirect = {r["path"]: r for r in rows if r["scheme"] == sc and r["variant"] == "bayer_direct"}
        common = sorted(set(bsplit) & set(bdirect))
        if common:
            s1 = sum(bsplit[p]["stored"] for p in common)
            s0 = sum(bdirect[p]["stored"] for p in common)
            orig = sum(bsplit[p]["orig"] for p in common)
            out["bayer"][sc] = {"files": len(common), "ratio_direct": round(orig / s0, 4),
                                "ratio_split": round(orig / s1, 4), "saving": round(1 - s1 / s0, 4)}
    out["failures"] = [{"path": r["path"], "scheme": r["scheme"], "variant": r["variant"], "status": r["status"],
                        "reason": r["reason"]} for r in rows if r["status"] in ("fallback", "mismatch")]
    return out


def speed_sample(recs: list[dict], per_group: int) -> list[dict]:
    seen: dict[str, int] = {}
    out = []
    for r in recs:
        if r["class"] not in SPLITTABLE or not r.get("processable"):
            continue
        g = group_of(r)
        if seen.get(g, 0) < per_group:
            seen[g] = seen.get(g, 0) + 1
            out.append(r)
    return out


# 复现式还原用纯 Python 的 libtiff 兼容 LZW 编码器，速度不代表产品，单列、不计入“总体（不含）”速度
PY_SLOW_GROUPS = ("TIFF-LZW",)


def speed_summary(speed: list[dict], schemes: list[str]) -> dict:
    def mbps(xs):
        b = sum(x["orig"] for x in xs)
        te = max(sum(x["enc_s"] for x in xs), 1e-9)      # 小文件极快的操作耗时可能是微秒级，防除零
        td = max(sum(x["dec_s"] for x in xs), 1e-9)
        return {"enc_MBps": round(b / te / 1e6, 2), "dec_MBps": round(b / td / 1e6, 2), "files": len(xs)}
    out = {}
    for sc in schemes:
        xs = [x for x in speed if x["scheme"] == sc]
        if not xs:
            continue
        out[sc] = mbps(xs)
        fast = [x for x in xs if x["group"] not in PY_SLOW_GROUPS]
        out[sc]["excl_py_lzw"] = mbps(fast) if fast else None
        out[sc]["by_group"] = {g: mbps([x for x in xs if x["group"] == g]) for g in sorted({x["group"] for x in xs})}
    return out
