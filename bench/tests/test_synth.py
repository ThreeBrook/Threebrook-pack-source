import json

from bench.synth.generate import generate


def test_all_images_read_back(small):
    _, m = small
    for f in m["files"]:
        e = f["expect"]
        if e and e["class"] not in ("unsupported", "unknown"):
            assert f["readback"] == "ok", f["path"]


def test_deterministic(small, tmp_path):
    _, m = small
    generate(tmp_path / "again", "small", 7, seq_frames=6, log=lambda *_: None)
    m2 = json.loads((tmp_path / "again" / "manifest.json").read_text(encoding="utf-8"))
    assert [f["sha256"] for f in m["files"]] == [f["sha256"] for f in m2["files"]]


def test_coverage(small):
    root, m = small
    exp = [f["expect"] for f in m["files"] if f["expect"]]
    bmp = [e for e in exp if e["container"] == "bmp" and e["class"] == "bmp"]
    assert {e["channels"] for e in bmp} >= {1, 3, 4}
    assert {e["row_order"] for e in bmp} == {"bottom_up", "top_down"}
    assert any(e["row_padding"] for e in bmp) and any(e["palette"] == "color" for e in bmp)
    tif = [e for e in exp if e["container"] == "tiff"]
    assert {e["byteorder"] for e in tif} >= {"II", "MM"}
    assert {e["layout"] for e in tif} >= {"strips", "tiles"}
    assert any(e["pages"] > 1 for e in tif) and any(e["shift"] == 4 for e in tif)
    assert {e["compression"] for e in tif} >= {None, "lzw", "zip", "packbits"}
    raw = {e["pixel_format"] for e in exp if e["class"] == "raw"}
    assert raw >= {"Mono8", "Mono16", "Mono12", "Mono10p", "Mono12p", "Mono12Packed"}
    bayer = {(e["bayer"], e["container"]) for e in exp if e["class"] == "bayer"}
    assert bayer == {(p, c) for p in ("RGGB", "BGGR", "GRBG", "GBRG") for c in ("raw", "bmp", "tiff")}
    assert any(e["width"] % 2 and e["height"] % 2 for e in exp if e["class"] == "bayer")
    for cat in ("bmp", "tiff", "raw", "bayer", "sequence", "other"):
        assert (root / cat / "README.md").is_file()
    assert (root / "raw_config.yaml").is_file()
