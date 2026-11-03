import argparse
import csv
import json

import pytest

from bench import roi
from bench.report import build, roi_from_args
from bench.scan import run


def test_device_math():
    d = roi.device(daily_gb=200, disk_tb=10, retention_days=90, ratio=2.5, debug_daily_gb=500, reserve=0.1)
    assert d["keep_days_before"] == 45.0 and d["keep_days_after"] == 112.5
    assert d["debug_fill_days_before"] == 18.0 and d["debug_fill_days_after"] == 45.0
    assert d["need_tb_before"] == 20.0 and d["need_tb_after"] == 8.0 and d["saved_tb"] == 12.0
    assert d["disks_before"]["10TB"] == 2 and d["disks_after"]["10TB"] == 1
    assert not d["meets_retention_before"] and d["meets_retention_after"]
    with pytest.raises(ValueError):
        roi.device(daily_gb=0, disk_tb=10, retention_days=90, ratio=2)


def _ns(**kw):
    base = dict(daily_gb=None, daily_images=None, image_mb=None, debug_daily_gb=None, disk_tb=None,
                retention_days=None, reserve=0.1)
    return argparse.Namespace(**{**base, **kw})


def test_roi_args():
    assert roi_from_args(_ns())["example"] is True
    r = roi_from_args(_ns(daily_images=40000, image_mb=5, disk_tb=8, retention_days=60))
    assert r["example"] is False and r["params"]["daily_gb"] == 200 and r["params"]["debug_daily_gb"] == 200


def test_scan_and_report(small, tmp_path):
    root, manifest = small
    out = tmp_path / "scan"
    run(root, out, schemes=["zstd3", "jpegls", "htj2k"], quick=False, jobs=4, raw_config=root / "raw_config.yaml",
        speed_per_group=0, repeat=1, ntfs=False, frames="sequence/frame_*", background="sequence/background_*",
        ceiling="auto", cpuload=False, cpu_cores=4, cpu_load=0.5, log=lambda *_: None)
    data = build(out, roi_from_args(_ns(daily_gb=100, disk_tb=5, retention_days=30)))
    html = (out / "report.html").read_text(encoding="utf-8")
    for sec in ("1 样本概览", "2 各方案压缩比", "3 与 Windows NTFS", "4 Bayer", "5 复现式还原", "6 天花板",
                "7 CPU 影响", "8 失败与兜底", "9 ROI"):
        assert sec in html
    assert "http://" not in html and "https://" not in html          # 离线：无外部资源
    rows = list(csv.DictReader(open(out / "details.csv", encoding="utf-8-sig")))
    assert len(rows) == len(list(p for p in root.rglob("*") if p.is_file()))
    s = json.loads((out / "summary.json").read_text(encoding="utf-8"))
    assert s["recommended"]["scheme"] == "jpegls" and s["roi"]["example"] is False
    assert data["ceiling"] and data["layout"]["mismatch"] == 0


def test_scan_refuses_output_inside_samples(small):
    root, _ = small
    with pytest.raises(SystemExit):
        run(root, root / "out", schemes=["zstd3"], quick=False, jobs=1, raw_config=None, speed_per_group=0, repeat=1,
            ntfs=False, frames="x", background="y", ceiling="off", cpuload=False, cpu_cores=1, cpu_load=0.5,
            log=lambda *_: None)
    assert not (root / "out").exists()
