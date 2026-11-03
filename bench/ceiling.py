"""T5 天花板测试（定义见 bench/FINDINGS.md 第 2 节）。

  G1  标准版面 + 单帧编码：不移位、不拆 Bayer，位深按容器位数（8/16）；取 JPEG-LS / HTJ2K / JPEG XL 中最小
  G2  领域版面 + 单帧编码：移位、按实际有效位数、Bayer 四平面；取最小
  G3a 背景参考帧：残差 = 当前帧 − 整像素对齐后的背景参考（空载背景整数均值），参考帧体积按帧数分摊
  G3b 帧间：以前一帧为参考，块级整数运动补偿后求残差，运动矢量计入体积
  G3c Bayer：四平面拆分后以 G 平面整数预测 R、B，编码残差

可逆路径全部整数运算；有符号残差加偏移映射为无符号（位深 +1），在 JPEG-LS / JPEG 2000（0 级、2 级小波）中逐帧取最小。
每个文件的每一档都做“解码 → 逆预测 → 按原版面重组 → SHA-256”校验。
判读时 G3 与同样“取最小”的 G2 比较（保守）；另列出与 JPEG-LS 版 G2 的对比。
"""
from __future__ import annotations

import hashlib
from pathlib import Path

import imagecodecs as ic
import numpy as np

from . import codecs as cx
from .layout import rebuild, split
from .layout.model import Plane
from .layout.transforms import BAYER_PHASES

G12_CODECS = ("jpegls", "htj2k", "jxl7")


def _sha(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def _container(s, stream_lens: list[int], blob_z: bytes | None, extra: float = 0.0) -> float:
    fake = [b"\0" * n for n in stream_lens]
    return cx.container_size(s, fake, blob_z)["total"] + extra


def _blob_z(s) -> bytes | None:
    b = cx.nonpixel_blob(s)
    return ic.zstd_encode(b, level=19) if b else None


def _rebuild_with(s, arrays: dict) -> bytes:
    d = s.detach()
    d.planes = [Plane(p.name, arrays.get(p.name, p.data).reshape(p.data.shape).astype(p.data.dtype), p.bits)
                for p in d.planes]
    return rebuild(d)


# ---------------------------------------------------------------- 残差编码

def _j2k(levels: int):
    def enc(u: np.ndarray, bits: int) -> bytes:
        return ic.jpeg2k_encode(u, level=0, reversible=True, resolutions=levels + 1, bitspersample=bits, numthreads=1)
    return enc


# 残差候选编码器（都是标准码流）。残差接近白噪声时，JPEG-LS 的空间预测反而放大误差，
# 不做小波分解的 JPEG 2000（EBCOT 上下文熵编码）更接近熵下限；纹理多时 JPEG-LS / 小波更好。逐帧取最小。
RESIDUAL_CODECS = {
    "jpegls": (lambda u, bits: ic.jpegls_encode(u, level=0, bitspersample=bits), ic.jpegls_decode),
    "j2k_l0": (_j2k(0), ic.jpeg2k_decode),
    "j2k_l2": (_j2k(2), ic.jpeg2k_decode),
}


def encode_residual(r: np.ndarray, bits_src: int) -> tuple[bytes, np.ndarray, str]:
    """有符号残差 r ∈ [-(2^b-1), 2^b-1] 加偏移 2^b-1 映射为无符号（位深 b+1），
    在候选编码器中取最小。返回 (码流, 解码出的残差, 编码器 id)。"""
    off = (1 << bits_src) - 1
    u = r.astype(np.int32) + off
    if u.min() < 0 or u.max() > 2 * off:
        raise ValueError("残差超出范围")
    u = u.astype(np.uint16)
    best = None
    for cid, (enc, _) in RESIDUAL_CODECS.items():
        b = enc(u, bits_src + 1)
        if best is None or len(b) < len(best[0]):
            best = (b, cid)
    stream, cid = best
    back = np.asarray(RESIDUAL_CODECS[cid][1](stream)).astype(np.int32).reshape(r.shape) - off
    return stream, back, cid


def shifted(a: np.ndarray, dy: int, dx: int, pad: int) -> np.ndarray:
    """返回 b[y,x] = a[y+dy, x+dx]，越界按边缘复制。整数、确定性。"""
    p = np.pad(a, pad, mode="edge")
    h, w = a.shape
    return p[pad + dy: pad + dy + h, pad + dx: pad + dx + w]


# ---------------------------------------------------------------- 块级模式选择

def med_error(x: np.ndarray) -> np.ndarray:
    """JPEG-LS / LOCO-I 的 MED 预测误差绝对值（首行首列用左 / 上邻），用作帧内编码代价的估计。"""
    x = x.astype(np.int32)
    a = np.zeros_like(x)
    b = np.zeros_like(x)
    c = np.zeros_like(x)
    a[:, 1:] = x[:, :-1]
    b[1:, :] = x[:-1, :]
    c[1:, 1:] = x[:-1, :-1]
    a[:, 0] = b[:, 0]
    b[0, :] = a[0, :]
    c[0, :] = a[0, :]
    c[:, 0] = b[:, 0]
    hi, lo = np.maximum(a, b), np.minimum(a, b)
    med = np.where(c >= hi, lo, np.where(c <= lo, hi, a + b - c))
    return np.abs(x - med)


def _block_sum(a: np.ndarray, block: int) -> np.ndarray:
    h, w = a.shape
    nby, nbx = -(-h // block), -(-w // block)
    p = np.pad(a, ((0, nby * block - h), (0, nbx * block - w)))
    return p.reshape(nby, block, nbx, block).sum(axis=(1, 3), dtype=np.int64)


def _expand(m: np.ndarray, block: int, shape) -> np.ndarray:
    return np.repeat(np.repeat(m, block, axis=0), block, axis=1)[: shape[0], : shape[1]]


def mode_select(cur: np.ndarray, inter: np.ndarray, block: int) -> tuple[np.ndarray, bytes, float]:
    """逐块在“参考预测”与“帧内（块均值）”之间选择：代价估计为 Σ|cur−inter| 对 Σ|MED 误差|。
    返回 (预测图, 附加信息（模式图 + 帧内块均值，zstd）, 帧内块比例)。全部整数。"""
    c = cur.astype(np.int32)
    inter_cost = _block_sum(np.abs(c - inter), block)
    intra_cost = _block_sum(med_error(c), block)
    intra = intra_cost < inter_cost
    cnt = _block_sum(np.ones_like(c), block)
    means = ((_block_sum(c, block) + cnt // 2) // cnt).astype(np.uint16)
    means[~intra] = 0
    pred = np.where(_expand(intra, block, c.shape), _expand(means.astype(np.int32), block, c.shape), inter)
    side = ic.zstd_encode(intra.astype(np.uint8).tobytes() + means.tobytes(), level=19)
    back = ic.zstd_decode(side)                                   # 附加信息往返
    n = intra.size
    m2 = np.frombuffer(back[:n], np.uint8).reshape(intra.shape).astype(bool)
    mu2 = np.frombuffer(back[n:], np.uint16).reshape(means.shape).astype(np.int32)
    pred2 = np.where(_expand(m2, block, c.shape), _expand(mu2, block, c.shape), inter)
    if not np.array_equal(pred2, pred):
        raise ValueError("模式附加信息往返不一致")
    return pred, side, float(intra.mean()), _expand(intra, block, c.shape), intra


def code_predicted(cur: np.ndarray, inter: np.ndarray, block: int, bits: int) -> dict:
    """逐块模式选择后，两种编码方式取小：
    single  —— 一幅残差图（帧内块用块均值作预测），在残差候选编码器中取小；
    regions —— 分区编码：参考块的残差、帧内块的原像素拆成两幅图，另一区域填 0，
               残差图按残差候选编码器取小（白噪声适合不做预测的 JPEG 2000），原像素图用 JPEG-LS。
    返回 {"lens": [...各码流与附加信息字节数], "recon": 还原图, "variant", "codec", "intra"}。"""
    c = cur.astype(np.int32)
    pred, side, frac, mask, intra = mode_select(cur, inter, block)
    st, back, cid = encode_residual(c - pred, bits)
    best = {"lens": [len(st), len(side)], "recon": back + pred, "variant": "single", "codec": cid, "intra": frac}
    if 0 < frac < 1:
        a_img = np.where(mask, 0, c - inter)
        sa, back_a, cid_a = encode_residual(a_img, bits)
        b_img = np.where(mask, c, 0).astype(np.uint16 if bits > 8 else np.uint8)
        sb = ic.jpegls_encode(b_img, level=0, bitspersample=bits)
        back_b = np.asarray(ic.jpegls_decode(sb)).astype(np.int32).reshape(c.shape)
        side_m = ic.zstd_encode(intra.astype(np.uint8).tobytes(), level=19)
        lens = [len(sa), len(sb), len(side_m)]
        if sum(lens) < sum(best["lens"]):
            best = {"lens": lens, "recon": np.where(mask, back_b, back_a + inter), "variant": "regions",
                    "codec": f"{cid_a}+jpegls", "intra": frac}
    return best


# ---------------------------------------------------------------- G1 / G2

def g12(path: str, rec: dict, codecs=G12_CODECS) -> dict:
    data = Path(path).read_bytes()
    out = {"path": rec["path"], "orig": len(data)}
    variants = {"G1": split(data, {**rec, "shift": 0, "class": {"bmp": "bmp", "tiff": "tiff_uncompressed",
                                                                  "raw": "raw"}[rec["container"]]}),
                "G2": split(data, rec)}
    for g, s in variants.items():
        bz = _blob_z(s)
        for cid in codecs:
            codec = cx.PIXEL_CODECS[cid]
            planes = s.planes
            if g == "G1":           # 位深按容器位数，不用实际有效位数
                planes = [Plane(p.name, p.data, p.data.itemsize * 8) for p in planes]
            streams = [codec.encode(p) for p in planes]
            dec = {p.name: np.asarray(codec.decode(b)) for p, b in zip(planes, streams)}
            ok = _sha(_rebuild_with(s, dec)) == rec["sha256"]
            out[f"{g}.{cid}"] = _container(s, [len(b) for b in streams], bz) if ok else None
    return out


# ---------------------------------------------------------------- 序列帧工具

def gray_split(data: bytes, rec: dict):
    s = split(data, rec)
    if len(s.planes) != 1 or s.planes[0].data.ndim != 2:
        raise ValueError("G3a/G3b 只支持单个灰度平面的帧")
    return s


def background_ref(bg_paths: list[str], bg_recs: list[dict]) -> tuple[np.ndarray, int]:
    """空载背景的整数均值（四舍五入），返回 (参考帧, 位深)。"""
    acc, n, bits = None, 0, 8
    for p, r in zip(bg_paths, bg_recs):
        s = gray_split(Path(p).read_bytes(), r)
        a = s.planes[0].data.astype(np.int64)
        bits = s.planes[0].data.itemsize * 8
        acc = a if acc is None else acc + a
        n += 1
    ref = ((acc + n // 2) // n).astype(np.uint16 if bits > 8 else np.uint8)
    return ref, bits


def align(cur: np.ndarray, ref: np.ndarray, radius: int, bits: int = 8) -> tuple[int, int]:
    """整数全局对齐（相机微振）：搜索截断 SAD 最小的 (dy, dx)，平局取位移小者（确定性）。
    差值封顶在 2^(bits-4)（约 4 倍噪声）：产品等前景区域无论怎么平移贡献都是常数，对齐只由背景决定。
    隔行隔列抽样以加速。"""
    c = cur[::2, ::2].astype(np.int32)
    cap = 1 << max(1, bits - 4)
    best, arg = None, (0, 0)
    for dy, dx in sorted(((y, x) for y in range(-radius, radius + 1) for x in range(-radius, radius + 1)),
                         key=lambda t: (abs(t[0]) + abs(t[1]), t)):
        d = np.abs(c - shifted(ref, dy, dx, radius)[::2, ::2].astype(np.int32))
        sad = int(np.minimum(d, cap).sum())
        if best is None or sad < best:
            best, arg = sad, (dy, dx)
    return arg


def g3a(path: str, rec: dict, ref: np.ndarray, bits: int, ref_share: float, radius: int = 6,
        block: int = 16) -> dict:
    data = Path(path).read_bytes()
    s = gray_split(data, rec)
    cur = s.planes[0].data
    name = s.planes[0].name
    bz = _blob_z(s)
    dy, dx = align(cur, ref, radius, bits)
    inter = shifted(ref, dy, dx, radius).astype(np.int32)
    out = {"path": rec["path"], "orig": len(data), "align": f"{dy},{dx}"}
    # 纯预测：整幅都用参考帧
    st, back, _ = encode_residual(cur.astype(np.int32) - inter, bits)
    ok = _sha(_rebuild_with(s, {name: back + inter})) == rec["sha256"]
    out["G3a_pure"] = _container(s, [len(st), 2], bz, ref_share) if ok else None
    # 块级模式选择
    cp = code_predicted(cur, inter, block, bits)
    ok = _sha(_rebuild_with(s, {name: cp["recon"]})) == rec["sha256"]
    out.update(G3a=_container(s, cp["lens"] + [2], bz, ref_share) if ok else None,
               G3a_intra_blocks=round(cp["intra"], 3), G3a_codec=f"{cp['variant']}:{cp['codec']}")
    return out


def motion_compensate(cur: np.ndarray, prev: np.ndarray, block: int, radius: int):
    """块级整数运动补偿（全搜索 SAD，平局取位移小者）。返回 (预测图, 运动矢量 int8 (nby,nbx,2))。"""
    h, w = cur.shape
    nby, nbx = -(-h // block), -(-w // block)
    hp, wp = nby * block, nbx * block
    c = np.pad(cur, ((0, hp - h), (0, wp - w)), mode="edge").astype(np.int32)
    pp = np.pad(prev, ((0, hp - h), (0, wp - w)), mode="edge")
    offsets = sorted(((y, x) for y in range(-radius, radius + 1) for x in range(-radius, radius + 1)),
                     key=lambda t: (abs(t[0]) + abs(t[1]), t))
    best = np.full((nby, nbx), np.iinfo(np.int64).max, np.int64)
    mv = np.zeros((nby, nbx, 2), np.int8)
    for dy, dx in offsets:
        sh = shifted(pp, dy, dx, radius).astype(np.int32)
        sad = np.abs(c - sh).reshape(nby, block, nbx, block).sum(axis=(1, 3), dtype=np.int64)
        better = sad < best
        best[better] = sad[better]
        mv[better] = (dy, dx)
    pred = np.empty((hp, wp), np.int32)
    for dy, dx in offsets:
        m = (mv[..., 0] == dy) & (mv[..., 1] == dx)
        if not m.any():
            continue
        full = np.repeat(np.repeat(m, block, axis=0), block, axis=1)
        pred[full] = shifted(pp, dy, dx, radius).astype(np.int32)[full]
    return pred[:h, :w], mv


def g3b(path: str, rec: dict, prev_path: str, prev_rec: dict, bits: int, block: int = 16, radius: int = 4) -> dict:
    data = Path(path).read_bytes()
    s = gray_split(data, rec)
    cur = s.planes[0].data
    name = s.planes[0].name
    bz = _blob_z(s)
    prev = gray_split(Path(prev_path).read_bytes(), prev_rec).planes[0].data
    inter, mv = motion_compensate(cur, prev, block, radius)
    mv_z = ic.zstd_encode(mv.tobytes(), level=19)
    # 解码端：用前一帧（已无损还原）+ 运动矢量重建预测，再加残差
    if not np.array_equal(np.frombuffer(ic.zstd_decode(mv_z), np.int8).reshape(mv.shape), mv):
        raise ValueError("运动矢量往返不一致")
    out = {"path": rec["path"], "orig": len(data), "mv_bytes": len(mv_z),
           "moving_blocks": f"{int((mv != 0).any(axis=2).sum())}/{mv.shape[0] * mv.shape[1]}"}
    st, back, _ = encode_residual(cur.astype(np.int32) - inter, bits)
    ok = _sha(_rebuild_with(s, {name: back + inter})) == rec["sha256"]
    out["G3b_pure"] = _container(s, [len(st), len(mv_z)], bz) if ok else None
    cp = code_predicted(cur, inter, block, bits)
    ok = _sha(_rebuild_with(s, {name: cp["recon"]})) == rec["sha256"]
    out.update(G3b=_container(s, cp["lens"] + [len(mv_z)], bz) if ok else None,
               G3b_intra_blocks=round(cp["intra"], 3), G3b_codec=f"{cp['variant']}:{cp['codec']}")
    return out


# ---------------------------------------------------------------- G3c

def g_average(planes: list[np.ndarray], g_diag: str) -> tuple[np.ndarray, list[int], list[int]]:
    """返回 (G 均值预测图, G 相位下标, R/B 相位下标)。两个 G 平面边缘复制到 R/B 平面尺寸后取整数均值。"""
    gi, xi = ([0, 3], [1, 2]) if g_diag == "main" else ([1, 2], [0, 3])
    h = max(planes[i].shape[0] for i in xi)
    w = max(planes[i].shape[1] for i in xi)

    def fit(a):
        return np.pad(a, ((0, max(0, h - a.shape[0])), (0, max(0, w - a.shape[1]))), mode="edge")[:h, :w].astype(np.int32)
    ga = (fit(planes[gi[0]]) + fit(planes[gi[1]]) + 1) >> 1
    return ga, gi, xi


def g3c(path: str, rec: dict, g_diag: str, block: int = 16) -> dict:
    """G 平面照常编码；R、B 平面编码 X − round(k·Gavg)（k 为 8 位定点整数比例，每平面 2 字节参数）。
    另给出逐块模式选择版本（G 预测 / 帧内块均值）。"""
    data = Path(path).read_bytes()
    s = split(data, rec)
    if len(s.planes) != 4:
        raise ValueError("G3c 需要 Bayer 四平面")
    arrs = [p.data for p in s.planes]
    bits = max(p.bits for p in s.planes)
    ga, gi, xi = g_average(arrs, g_diag)
    bz = _blob_z(s)
    base_streams, dec = [], {}
    for i in gi:
        b = cx.PIXEL_CODECS["jpegls"].encode(s.planes[i])
        base_streams.append(len(b))
        dec[s.planes[i].name] = np.asarray(ic.jpegls_decode(b))
    out = {"path": rec["path"], "orig": len(data), "g_diagonal": g_diag}
    for variant in ("pure", "mode"):
        streams, d2 = list(base_streams), dict(dec)
        for i in xi:
            x = arrs[i].astype(np.int32)
            g = ga[: x.shape[0], : x.shape[1]]
            k = min(max(int(round(256 * x.sum() / max(1, g.sum()))), 0), 65535)   # 定点比例，整数
            inter = np.clip((g * k + 128) >> 8, 0, (1 << bits) - 1)
            if variant == "mode":
                cp = code_predicted(x, inter, block, bits)
                streams += cp["lens"] + [2]
                d2[s.planes[i].name] = cp["recon"]
                continue
            st, back, _ = encode_residual(x - inter, bits)
            streams += [len(st), 2]
            d2[s.planes[i].name] = back + inter
        ok = _sha(_rebuild_with(s, d2)) == rec["sha256"]
        out["G3c_pure" if variant == "pure" else "G3c"] = _container(s, streams, bz) if ok else None
    return out


# ---------------------------------------------------------------- 判读

def gain(new: float, base: float) -> float:
    return 1 - new / base


def judge(name: str, g: float, vs: str) -> str:
    """name 对 vs 的体积变化 g（正 = 更小）。G2 对 G1 用“版面整理”措辞，G3 对 G2 用“预测”措辞。"""
    layout = name == "G2"
    if g < 0:
        verdict = "比对照更大，无收益" + ("" if layout else "，首发不做")
    elif g < 0.05:
        verdict = "收益不明显（成本低，仍建议保留）" if layout else "无明显收益，首发不做"
    elif g < 0.15:
        verdict = "有收益，保留" if layout else "有收益，列为阶段 1 可选项，需更多客户样本确认"
    else:
        verdict = "收益显著，必须保留" if layout else "收益显著，建议进入阶段 1 设计，同时评估代价"
    sign = "" if abs(g) < 0.0005 else ("-" if g > 0 else "+")
    return f"{name} 对 {vs}：体积 {sign}{abs(g):.1%} → {verdict}"


COSTS = {
    "G3a": "代价：读取依赖背景参考帧（参考帧损坏影响整组）；需维护背景更新策略（换型、光照漂移）",
    "G3b": "代价：读取第 N 帧需先还原前面的帧（随机读取变慢，需设关键帧间隔）；任一帧损坏影响后续；写入需缓存前一帧",
    "G3c": "代价：R/B 依赖 G 平面，同一文件内无额外依赖，代价很小",
}
