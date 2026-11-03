import json

import pytest

from bench.synth.generate import generate


@pytest.fixture(scope="session")
def small(tmp_path_factory):
    """小尺寸合成数据集（整个测试会话共用）。"""
    out = tmp_path_factory.mktemp("synth")
    generate(out, "small", 7, seq_frames=6, log=lambda *_: None)
    return out, json.loads((out / "manifest.json").read_text(encoding="utf-8"))
