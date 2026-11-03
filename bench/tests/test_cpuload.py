import platform

import pytest

from bench import cpuload as cl
from bench.detect import detect_file
from bench.formats import raw as fraw

pytestmark = pytest.mark.skipif(platform.system() != "Windows", reason="T6 仅在 Windows 上执行")


def test_available():
    ok, note = cl.available()
    assert ok and note == "Windows"


def test_inspect_once(small):
    root, _ = small
    img, tmpl = cl.load_image(str(root / "sequence/frame_0000.bmp"))
    assert isinstance(cl.inspect_once(img, tmpl), int)
    assert cl.calibrate(str(root / "sequence/frame_0000.bmp"), n=3) > 0


def test_short_phase_with_compressor(small):
    """短时间跑一组：检测有记录、后台压缩有吞吐且校验全部通过。"""
    import psutil
    root, _ = small
    rules = fraw.load_config(root / "raw_config.yaml")
    files = [(str(root / p), detect_file(root / p, root, rules))
             for p in ("bmp/bmp8_gray_bu.bmp", "tiff/tif16_II.tif", "raw/mono12p_odd.raw")]
    cpus = sorted(psutil.Process().cpu_affinity())[-4:]
    r = cl.run_phase(str(root / "sequence/frame_0000.bmp"), files, cpus, 2, 0.01, cpus[-1:], "below_normal", 0.5, 1.5)
    assert r["n"] > 0 and r["p99_ms"] >= r["p50_ms"]
    assert r["compress_files"] > 0 and r["verify_fail"] == 0
