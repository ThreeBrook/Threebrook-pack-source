import numpy as np
import pytest

from bench import ceiling as ce
from bench.detect import detect_file
from bench.formats import raw as fraw


@pytest.mark.parametrize("bits", [8, 12])
def test_residual_extremes_roundtrip(bits):
    m = (1 << bits) - 1
    r = np.array([[-m, m, 0, -1], [1, -m, m, 7]], np.int32)
    _, back, cid = ce.encode_residual(r, bits)
    assert np.array_equal(back, r) and cid in ce.RESIDUAL_CODECS


def test_shifted_edge():
    a = np.arange(12, dtype=np.int32).reshape(3, 4)
    b = ce.shifted(a, 1, -1, 2)
    assert b[0, 1] == a[1, 0] and b[0, 0] == a[1, 0] and b[2, 3] == a[2, 2]


def test_motion_compensation_recovers_shift():
    rng = np.random.default_rng(0)
    prev = rng.integers(0, 256, (64, 80)).astype(np.uint8)
    cur = ce.shifted(prev, 2, -1, 4).astype(np.uint8)        # cur[y,x] = prev[y+2, x-1]
    pred, mv = ce.motion_compensate(cur, prev, 16, 4)
    inner = (slice(16, 48), slice(16, 64))
    assert np.array_equal(pred[inner], cur[inner].astype(np.int32))
    assert (mv[1:3, 1:4] == (2, -1)).all()


def test_mode_select_roundtrip():
    rng = np.random.default_rng(1)
    cur = rng.integers(0, 256, (40, 50)).astype(np.uint8)
    inter = np.zeros((40, 50), np.int32)
    inter[:20] = cur[:20]                                     # 上半部分参考完美
    pred, side, frac, mask, _ = ce.mode_select(cur, inter, 8)
    assert 0 < frac < 1 and side
    assert np.array_equal(pred[:16], cur[:16].astype(np.int32))


def test_tiers_verify_on_synthetic(small):
    root, _ = small
    rules = fraw.load_config(root / "raw_config.yaml")
    frames = sorted(root.glob("sequence/frame_*"))[:3]
    bgs = sorted(root.glob("sequence/background_*"))
    frec = [detect_file(p, root, rules) for p in frames]
    ref, bits = ce.background_ref([str(p) for p in bgs], [detect_file(p, root, rules) for p in bgs])
    r12 = ce.g12(str(frames[0]), frec[0], ("jpegls",))
    assert r12["G1.jpegls"] and r12["G2.jpegls"]
    ra = ce.g3a(str(frames[1]), frec[1], ref, bits, 100.0)
    assert ra["G3a"] and ra["G3a_pure"]
    rb = ce.g3b(str(frames[1]), frec[1], str(frames[0]), frec[0], bits)
    assert rb["G3b"] and rb["G3b_pure"]
    bp = root / "bayer/bayer_RGGB_bmp8.bmp"
    rc = ce.g3c(str(bp), detect_file(bp, root, rules), "anti")
    assert rc["G3c"] and rc["G3c_pure"]


def test_judge_wording():
    assert "必须保留" in ce.judge("G2", 0.2, "G1")
    assert "首发不做" in ce.judge("G3a", 0.01, "G2")
    assert "无收益" in ce.judge("G3b", -0.05, "G2")
    assert "阶段 1 设计" in ce.judge("G3c", 0.2, "G2")


def test_align_ignores_foreground():
    """背景平移 (1,-2)，中央有一大块不在参考里的前景：对齐应只由背景决定。"""
    rng = np.random.default_rng(2)
    ref = (rng.integers(0, 40, (96, 96)) + np.add.outer(np.arange(96), np.arange(96))).astype(np.uint8)
    cur = ce.shifted(ref, 1, -2, 6).copy()
    cur[20:76, 20:76] = 230
    assert ce.align(cur, ref, 6) == (1, -2)


def test_code_predicted_roundtrip():
    rng = np.random.default_rng(3)
    cur = rng.integers(0, 256, (48, 64)).astype(np.uint8)
    inter = np.zeros((48, 64), np.int32)
    inter[:24] = cur[:24].astype(np.int32) + rng.integers(-2, 3, (24, 64))
    r = ce.code_predicted(cur, inter, 8, 8)
    assert np.array_equal(r["recon"], cur.astype(np.int32)) and r["variant"] in ("single", "regions")
