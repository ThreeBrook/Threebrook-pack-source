"""像素统计：有效位数、高位对齐移位、Bayer 2×2 周期检测。

这些统计只用于“识别”，不在可逆路径上；判定结果最终都要经往返哈希校验兜底。
"""
from __future__ import annotations

import numpy as np

BAYER_THRESHOLD = 0.04      # 两条对角线均值相对差的投票门限
BAYER_MIN_AGREE = 0.8       # 有效图块中同号比例
BAYER_GRID = 8


def value_stats(arrays, bits_stored: int) -> dict:
    """value_max、shift（所有值低位都是 0 的位数，仅对 >8 位容器判断）、bits_effective。"""
    vmax, acc = 0, 0
    for a in arrays:
        if a.size:
            vmax = max(vmax, int(a.max()))
            if bits_stored > 8:
                acc |= int(np.bitwise_or.reduce(a.ravel()))
    shift = min((acc & -acc).bit_length() - 1, bits_stored - 1) if acc else 0
    return {"value_max": vmax, "shift": shift, "bits_effective": (vmax >> shift).bit_length()}


def _phase_means(tile: np.ndarray) -> np.ndarray:
    """四个相位（00,01,10,11）的均值，整数求和后再除。"""
    return np.array([tile[dy::2, dx::2].sum(dtype=np.int64) / tile[dy::2, dx::2].size
                     for dy in (0, 1) for dx in (0, 1)])


def bayer_detect(img: np.ndarray, maxval: int) -> dict:
    """img：自上而下的单通道像素（已右移去掉对齐位）。

    原理：Bayer 马赛克中两个 G 相位在同一对角线上，原始响应普遍高于 R、B，
    因此“主对角线(00,11) 与 副对角线(01,10)”的均值差在全图各处方向一致。
    自然灰度图的这一差值 ≈ 0 且方向随机。按 8×8 图块投票，避免局部周期纹理误判。

    能可靠确定 G 所在对角线；R 与 B 谁在哪个相位，单靠统计无法可靠区分
    （取决于光源色温与传感器），故只给出两个候选与一个低置信猜测。
    """
    h, w = img.shape
    h2, w2 = h - h % 2, w - w % 2
    out = {"is_bayer": False, "confidence": 0.0, "g_diagonal": None, "candidates": [], "guess": None,
           "score": 0.0, "tiles_valid": 0, "agree": 0.0}
    if h2 < 8 or w2 < 8:
        return out
    x = img[:h2, :w2]
    th, tw = max(2, h2 // BAYER_GRID // 2 * 2), max(2, w2 // BAYER_GRID // 2 * 2)
    lo, hi = 0.02 * maxval, 0.90 * maxval
    votes, mags, sums = [], [], np.zeros(4)
    for y in range(0, h2 - th + 1, th):
        for xx in range(0, w2 - tw + 1, tw):
            m = _phase_means(x[y:y + th, xx:xx + tw])
            mean = m.mean()
            if mean < lo or m.max() > hi:      # 太暗（噪声主导）或接近饱和（相位被削平）
                continue
            d = ((m[0] + m[3]) - (m[1] + m[2])) / (2 * mean)
            sums += m
            mags.append(abs(d))
            votes.append(np.sign(d) if abs(d) > BAYER_THRESHOLD else 0)
    if len(votes) < 4:
        return out
    votes = np.array(votes)
    pos, neg = (votes > 0).mean(), (votes < 0).mean()
    agree = max(pos, neg)
    score = float(np.median(mags))
    out.update(tiles_valid=len(votes), agree=round(float(agree), 3), score=round(score, 4))
    if agree < BAYER_MIN_AGREE:
        return out
    main = pos > neg
    m00, m01, m10, m11 = sums
    if main:          # G 在 00/11，R/B 在 01/10
        cands = ["GRBG", "GBRG"]
        guess = "GRBG" if m01 >= m10 else "GBRG"
    else:             # G 在 01/10，R/B 在 00/11
        cands = ["RGGB", "BGGR"]
        guess = "RGGB" if m00 >= m11 else "BGGR"
    out.update(is_bayer=True, g_diagonal="main" if main else "anti", candidates=cands, guess=guess,
               confidence=round(float(agree * min(1.0, score / (2.5 * BAYER_THRESHOLD))), 3))
    return out
