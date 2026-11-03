"""工控机本地盘场景的 ROI 代入（定义见 bench/FINDINGS.md 第 3 节）。

不修改 tools/roi_model.py（那里是网关一体机的 5 年 ROI）；这里是任务书 T7 要求的 device 逻辑：
  现有硬盘可保留天数 X → Y、调试期爆盘周期 A → B、满足招标保留天数需配硬盘与少配 N TB。
压缩比 r 必须是实测、校验通过、按原文件字节加权的总体压缩比。
"""
from __future__ import annotations

import math

DEFAULT_DISK_SIZES = (4, 8, 10, 16)


def device(daily_gb: float, disk_tb: float, retention_days: float, ratio: float,
           debug_daily_gb: float | None = None, reserve: float = 0.10,
           disk_sizes: tuple = DEFAULT_DISK_SIZES) -> dict:
    if daily_gb <= 0 or disk_tb <= 0 or ratio <= 0 or not 0 <= reserve < 1:
        raise ValueError("参数必须为正，预留比例在 [0, 1) 内")
    debug = debug_daily_gb or daily_gb
    usable_gb = disk_tb * 1000 * (1 - reserve)
    x = usable_gb / daily_gb
    a = usable_gb / debug
    need_raw_tb = daily_gb * retention_days / 1000 / (1 - reserve)
    need_new_tb = need_raw_tb / ratio

    def disks(tb):
        return {f"{s}TB": math.ceil(tb / s) for s in disk_sizes}

    y = x * ratio
    return {
        "inputs": {"daily_gb": daily_gb, "debug_daily_gb": debug, "disk_tb": disk_tb,
                   "retention_days": retention_days, "ratio": round(ratio, 4), "reserve": reserve},
        "keep_days_before": round(x, 1), "keep_days_after": round(y, 1),
        "debug_fill_days_before": round(a, 1), "debug_fill_days_after": round(a * ratio, 1),
        "need_tb_before": round(need_raw_tb, 2), "need_tb_after": round(need_new_tb, 2),
        "saved_tb": round(need_raw_tb - need_new_tb, 2),
        "disks_before": disks(need_raw_tb), "disks_after": disks(need_new_tb),
        "meets_retention_before": x >= retention_days, "meets_retention_after": y >= retention_days,
        "shortfall_days_after": round(max(0.0, retention_days - y), 1),
    }


def describe(d: dict) -> list[str]:
    i = d["inputs"]
    lines = [
        f"现有 {i['disk_tb']:g} TB 硬盘（预留 {i['reserve']:.0%}）可保留天数：从 {d['keep_days_before']:g} 天延长到 "
        f"{d['keep_days_after']:g} 天（压缩比 {i['ratio']:.2f}）",
        f"调试期（每天 {i['debug_daily_gb']:g} GB）爆盘周期：从 {d['debug_fill_days_before']:g} 天到 "
        f"{d['debug_fill_days_after']:g} 天",
        f"满足招标保留 {i['retention_days']:g} 天需配硬盘：从 {d['need_tb_before']:g} TB 降到 {d['need_tb_after']:g} TB，"
        f"少配 {d['saved_tb']:g} TB",
    ]
    if d["meets_retention_after"] and not d["meets_retention_before"]:
        lines.append("现有硬盘：压缩前不满足招标保留天数，压缩后满足")
    elif d["meets_retention_after"]:
        lines.append("现有硬盘：压缩前后都满足招标保留天数")
    else:
        lines.append(f"现有硬盘：压缩后仍差 {d['shortfall_days_after']:g} 天，需要扩容")
    return lines
