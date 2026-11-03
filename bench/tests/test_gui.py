"""GUI 后端（Api）测试：不开窗口，直接调用页面会调用的接口。"""
import json
import time
from pathlib import Path

import pytest

from bench.gui import app as gui


@pytest.fixture
def api(tmp_path, monkeypatch):
    monkeypatch.setattr(gui, "data_dir", lambda: tmp_path / "gui-data")
    return gui.Api()


def wait_done(api, timeout=240):
    t0 = time.time()
    while api.poll()["status"] == "running":
        assert time.time() - t0 < timeout, "测试超时"
        time.sleep(0.3)
    return api.poll()


def test_full_flow(api, small):
    root, _ = small
    info = api.get_info()
    assert info["settings"]["mode"] == "quick" and "gui-data" in info["output_root"]
    f = api.inspect(str(root))
    assert f["ok"] and f["files"] > 50 and f["raw_config"] and f["frames"] >= 2 and not f["inside_output"]
    assert f["disk_bytes"] >= f["bytes"]                                  # 占用空间按簇补齐，不会小于实际大小

    r = api.start({"path": str(root), "mode": "quick", "ntfs": False, "ceiling": True, "cpuload": False})
    assert r["ok"]
    assert not api.start({"path": str(root)})["ok"]                       # 同时只能跑一个
    s = wait_done(api)
    assert s["status"] == "done", s["error"]
    assert s["overall"] >= 99 and all(p["status"] in ("done", "skip") for p in s["phases"])
    assert s["result"]["mismatch"] == 0 and s["result"]["ratio"] > 1.5
    assert s["live"]["jpegls"] == pytest.approx(s["result"]["ratio"], rel=1e-3)
    # 体积对比：原始大小、压缩后大小与倍数一致
    res = s["result"]
    assert s["total_bytes"] == res["bytes"] == res["orig"] > 0
    assert res["orig"] / res["stored"] == pytest.approx(res["ratio"], rel=1e-3)
    o, st = s["live_bytes"]["jpegls"]
    assert o == res["orig"] and st == res["stored"]
    assert all(x["stored"] > 0 for x in res["schemes"])
    pa, po = res["parts"]["coded"], res["parts"]["other"]               # 总量 = 图像压缩部分 + 其余（原样 / zstd）
    assert pa["files"] + po["files"] == res["files"] and pa["files"] == res["ok"]
    assert pa["orig"] + po["orig"] == res["orig"] and pa["stored"] + po["stored"] == res["stored"]
    assert Path(s["report"]).is_file()
    assert any(x["tag"] == "需确认参数" for x in s["failures"])

    h = api.history()
    assert len(h) == 1 and h[0]["brief"] and h[0]["status"] == "done"
    roi = api.roi(h[0]["path"], {"daily_gb": 100, "disk_tb": 5, "retention_days": 30, "reserve": 0.1})
    assert roi["ok"] and roi["keep_days_after"] > roi["keep_days_before"]
    assert api.roi_apply(h[0]["path"], {"daily_gb": 100, "disk_tb": 5, "retention_days": 30, "reserve": 0.1})["ok"]
    summ = json.loads((Path(h[0]["path"]) / "summary.json").read_text(encoding="utf-8"))
    assert summ["roi"]["example"] is False and summ["roi"]["inputs"]["daily_gb"] == 100
    assert api.get_info()["settings"]["roi_set"] is True

    assert not api.delete_run(str(root))["ok"]                            # 越界：不能删样本
    assert api.delete_run(h[0]["path"])["ok"] and api.history() == []


def test_stop(api, small):
    root, _ = small
    api.start({"path": str(root), "mode": "full", "ntfs": False, "ceiling": False, "cpuload": False})
    time.sleep(2.0)
    api.stop()
    s = wait_done(api, timeout=60)
    assert s["status"] == "cancelled"
    h = api.history()
    assert h and h[0]["status"] == "cancelled" and h[0]["brief"] is None


def test_refuse_output_inside_sample(api, small):
    root, _ = small
    api.save_settings({"output_root": str(root / "results")})
    r = api.start({"path": str(root)})
    assert not r["ok"] and "只读" in r["error"]
    assert not (root / "results").exists()


def test_summarize_failures():
    recs = [{"path": "a/x.jpg", "class": "unsupported"}, {"path": "a/y.png", "class": "unsupported"},
            {"path": "b/t.bmp", "class": "unknown", "damaged": True, "error": "截断"},
            {"path": "r/m.raw", "class": "raw", "processable": False,
             "raw_candidates": [{"pixel_format": "Mono8", "width": 64, "height": 48}]},
            {"path": "ok.bmp", "class": "bmp", "processable": True}]
    out = gui.summarize_failures(recs, [{"path": "z.tif", "scheme": "jpegls", "reason": "往返 SHA-256 不一致"}])
    tags = [o["tag"] for o in out]
    assert tags.count("需确认参数") == 1 and tags.count("已损坏") == 1 and tags.count("还原不一致") == 1
    assert any("等 2 个" in o["name"] for o in out)
