import dataclasses

import numpy as np
import pytest

from bench import codecs as cx
from bench import compress as cp
from bench.detect import scan_dir

FAST = ["zstd3", "png", "jpegls", "htj2k"]


@pytest.fixture(scope="module")
def evaluated(small):
    root, _ = small
    recs = scan_dir(root, root / "raw_config.yaml", log=None)
    rows = []
    for r in recs:
        rows += cp.evaluate_file(str(root / r["path"]), r, FAST)
    return recs, rows


def test_all_processable_verified(evaluated):
    recs, rows = evaluated
    processable = {r["path"] for r in recs if r["processable"]}
    for row in rows:
        if row["path"] in processable and row["scheme"] in cx.PIXEL_CODECS:
            assert row["status"] in ("ok", "stored_raw"), (row["path"], row["scheme"], row["reason"])
    assert not [r for r in rows if r["status"] == "mismatch"]


def test_size_accounting_is_complete(evaluated):
    _, rows = evaluated
    for r in rows:
        if r["status"] == "ok" and r["variant"] != "file":
            # 总体积 = 像素码流 + 非像素 zstd + 配方 + 容器头/段头，且大于像素码流
            assert r["stored"] > r["pixel"] + r["nonpixel"] + r["recipe"]


def test_bayer_has_both_variants(evaluated):
    _, rows = evaluated
    bayer = [r for r in rows if r["class"] == "bayer" and r["scheme"] == "jpegls"]
    assert {r["variant"] for r in bayer} == {"default", "bayer_direct"}


def test_not_image_uses_fallback(evaluated):
    _, rows = evaluated
    for r in rows:
        if r["class"] in ("unknown", "unsupported") and r["scheme"] in cx.PIXEL_CODECS:
            assert r["status"] == "not_image" and r["stored"] <= r["orig"]
            assert "已核对" in r["reason"] or "原样" in r["reason"]           # 兜底 zstd 也做了往返核对


def test_broken_codec_falls_back(small, monkeypatch):
    """解码结果被篡改时必须检测到并兜底，不能把错误数据计入压缩比。"""
    root, _ = small
    recs = scan_dir(root, root / "raw_config.yaml", log=None)
    rec = next(r for r in recs if r["path"] == "bmp/bmp8_gray_bu.bmp")
    good = cx.PIXEL_CODECS["jpegls"]

    def bad_decode(b):
        a = np.array(good.decode(b))
        a.flat[0] ^= 1
        return a
    monkeypatch.setitem(cx.PIXEL_CODECS, "jpegls", dataclasses.replace(good, decode=bad_decode))
    rows = cp.evaluate_file(str(root / rec["path"]), rec, ["jpegls"])
    assert rows[0]["status"] == "fallback" and "SHA-256" in rows[0]["reason"]
    assert rows[0]["stored"] <= rows[0]["orig"]


def test_summary_ratio(evaluated):
    _, rows = evaluated
    s = cp.summarize(rows, FAST)
    for sc in FAST:
        o = s["overall"][sc]
        assert o["ratio"] == pytest.approx(o["orig"] / o["stored"], rel=1e-3)
    assert s["bayer"]["jpegls"]["saving"] > 0


def test_speed_pinning_never_touches_main_process():
    """测速绑核只发生在专用子进程里；主进程亲和性不变（否则后续阶段的子进程会全部挤在一个核上）。"""
    from concurrent.futures import ProcessPoolExecutor
    psutil = pytest.importorskip("psutil")
    before = psutil.Process().cpu_affinity()
    with ProcessPoolExecutor(max_workers=1) as ex:
        note = ex.submit(cp.pin_self).result()
    assert "绑定逻辑核" in note
    assert psutil.Process().cpu_affinity() == before
