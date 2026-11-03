import math

import numpy as np
import pytest

from bench import bgmodel, bgsweep


@pytest.mark.parametrize("sigma,tol", [(1.0, 0.1), (2.0, 0.03), (4.0, 0.03)])
def test_gaussian_entropy_large_sigma(sigma, tol):
    # 连续近似 log2(σ)+2.047 在 σ 较大时才精确；σ=1 时离散熵略高
    assert bgmodel.gaussian_entropy_bits(sigma) == pytest.approx(math.log2(sigma) + 2.047, abs=tol)


def test_gaussian_entropy_small_sigma():
    assert bgmodel.gaussian_entropy_bits(0.0) == 0.0
    assert bgmodel.gaussian_entropy_bits(0.2) < 0.2


def test_potential_detects_static_texture():
    """同样的时间噪声下，纹理丰富的静态背景应估出更大的背景参考收益。"""
    rng = np.random.default_rng(0)
    smooth = np.full((96, 128), 80.0)
    textured = 80 + 25 * (rng.random((96, 128)) > 0.5)

    def frames(base):
        return [np.clip(np.rint(base + rng.normal(0, 1.0, base.shape)), 0, 255).astype(np.uint8) for _ in range(6)]
    a = bgmodel.potential(frames(smooth), 8)
    b = bgmodel.potential(frames(textured), 8)
    assert 0.7 < a["temporal_sigma"] < 1.3 and 0.7 < b["temporal_sigma"] < 1.3
    assert b["static_detail"] > 5 * max(a["static_detail"], 0.5)
    assert b["bg_saving"] > a["bg_saving"] + 0.2


def test_sweep_config_small():
    """小尺寸跑一个配置：往返校验在 run_config 内部断言；低噪声 + 纹理背景应明显获益。"""
    r = bgsweep.run_config({"sigma": 0.3, "texture": 0.15, "fpn": 1.0, "product": 0.5}, h=96, w=128, n_frames=2)
    assert r["measured_saving"] > 0.2
    assert abs(r["estimated_saving"] - r["measured_saving"]) < 0.15
