"""T7 报告：汇总各阶段结果 → summary.json（机器可读）+ details.csv（逐文件）+ report.html（单文件、离线、中文）。

输入是 `python -m bench scan` 的输出目录：
  detect/detect.json  layout/layout_summary.json  compress/{summary.json,details.csv,speed.csv,ntfs.csv}
  ceiling/ceiling_summary.json（可选）  cpuload/cpu_impact.json（可选）  scan_meta.json
报告中的所有数字都来自这些实测文件；缺失的阶段明确写“未运行”及原因。
"""
from __future__ import annotations

import csv
import html
import json
import time
from collections import Counter, defaultdict
from pathlib import Path

from . import roi as roi_mod
from .compress import PY_SLOW_GROUPS

CLASS_LABELS = {"bmp": "BMP", "tiff_uncompressed": "TIFF（未压缩）", "tiff_compressed": "TIFF（LZW/ZIP/PackBits）",
                "raw": "RAW", "bayer": "Bayer（伪装/RAW）", "unsupported": "不处理（JPEG/PNG 等）", "unknown": "未知 / 非图像"}
WRITER_LABELS = {"opencv": "OpenCV", "halcon": "Halcon", "visionpro": "VisionPro", "camera_sdk": "相机 SDK",
                 "tifffile": "tifffile", "unknown": "未知"}
STATUS_LABELS = {"ok": "通过", "stored_raw": "原样存储", "fallback": "兜底", "not_image": "非图像兜底",
                 "mismatch": "校验不一致"}
IMAGE_CODECS = ("png", "jpegls", "htj2k", "jxl3", "jxl7", "jxl9")
ENCODER_LABELS = {"lzw(ic|libtiff)": "LZW（imagecodecs，不一致的条带用 libtiff 兼容编码器）",
                  "ic.packbits.rows": "PackBits（逐行）", "ic.packbits": "PackBits（整块）"}
for _lv in range(1, 10):
    ENCODER_LABELS[f"ic.zlib{_lv}"] = f"Deflate：经典 zlib {_lv} 级"
    ENCODER_LABELS[f"ic.deflate{_lv}"] = f"Deflate：libdeflate {_lv} 级"
    ENCODER_LABELS[f"py.zlib{_lv}"] = f"Deflate：Python zlib（zlib-ng）{_lv} 级"


def _load(p: Path):
    return json.loads(p.read_text(encoding="utf-8")) if p.is_file() else None


def _csv(p: Path) -> list[dict]:
    if not p.is_file():
        return []
    with open(p, encoding="utf-8-sig") as f:
        return list(csv.DictReader(f))


def _mb(b) -> str:
    """与 Windows 资源管理器同一口径：1 MB = 1024×1024 字节（文件实际大小，不含磁盘簇补齐）。"""
    return f"{b / 1024 ** 2:,.2f}"


def _e(x) -> str:
    return html.escape(str(x))


# ---------------------------------------------------------------- 汇总数据

def collect(out: Path, roi_args: dict | None, scheme: str = "jpegls") -> dict:
    from .compress import group_of
    detect = _load(out / "detect/detect.json") or []
    layout = _load(out / "layout/layout_summary.json")
    comp = _load(out / "compress/summary.json")
    ceil = _load(out / "ceiling/ceiling_summary.json")
    bgp = _load(out / "bgpotential/bg_potential.json")
    cpu = _load(out / "cpuload/cpu_impact.json")
    meta = _load(out / "scan_meta.json") or {}
    if comp is None:
        raise SystemExit(f"{out} 下没有压缩对比结果（compress/summary.json），请先运行 python -m bench scan")

    classes = defaultdict(lambda: [0, 0])
    groups = defaultdict(lambda: [0, 0])
    writers = Counter()
    writer_ev = {}
    for r in detect:
        classes[r["class"]][0] += 1
        classes[r["class"]][1] += r["size"]
        g = group_of(r)
        groups[g][0] += 1
        groups[g][1] += r["size"]
        if r["class"] not in ("unknown", "unsupported"):
            writers[r["writer"]] += 1
            writer_ev.setdefault(r["writer"], r.get("writer_evidence"))
    if scheme not in comp["overall"]:
        scheme = next(s for s in ("jpegls", "htj2k", "jxl3") if s in comp["overall"])
    ratio = comp["overall"][scheme]["ratio"]
    roi = None
    if roi_args:
        d = roi_mod.device(ratio=ratio, **roi_args["params"])
        roi = {"example": roi_args["example"], "scheme": scheme, **d, "text": roi_mod.describe(d)}

    notes = []
    if any(r.get("note", "").startswith("单次") for r in _csv(out / "compress/speed.csv")):
        notes.append("部分慢方案（单次 >10 s）只测 1 次，见 compress/speed.csv 的 note 列")
    if cpu and not cpu.get("reliable", True):
        notes.append("CPU 影响测试期间机器不安静，结果仅供参考，需在空闲机器或客户工控机上复测")
    return {
        "generated": time.strftime("%Y-%m-%d %H:%M:%S"), "machine": comp["machine"],
        "sample": {"dir": comp["sample_dir"], "files": comp["files"], "bytes": comp["bytes"],
                   "classes": {k: {"files": v[0], "bytes": v[1]} for k, v in sorted(classes.items(), key=lambda t: -t[1][1])},
                   "groups": {k: {"files": v[0], "bytes": v[1]} for k, v in sorted(groups.items(), key=lambda t: -t[1][1])},
                   "writers": dict(writers.most_common()), "writer_evidence": writer_ev},
        "recommended": {"scheme": scheme, "label": comp["labels"][scheme], "ratio": ratio,
                        "image_only_ratio": comp["image_only"][scheme]["ratio"]},
        "compress": {k: comp.get(k) for k in ("schemes", "labels", "overall", "image_only", "parts", "by_group", "by_dir", "bayer",
                                         "speed", "speed_protocol", "ratio_protocol", "lib_versions", "ntfs",
                                         "ntfs_note")},
        "layout": layout, "ceiling": ceil, "bg_potential": bgp, "cpuload": cpu, "failures": comp["failures"], "roi": roi,
        "meta": meta, "notes": notes,
    }


def write_details(out: Path, data: dict):
    detect = _load(out / "detect/detect.json") or []
    lay = {r["path"]: r for r in _csv(out / "layout/layout.csv")}
    ntfs = {r["path"]: int(r["ntfs"]) for r in _csv(out / "compress/ntfs.csv")}
    comp = defaultdict(dict)
    for r in _csv(out / "compress/details.csv"):
        comp[r["path"]][(r["scheme"], r["variant"])] = r
    schemes = data["compress"]["schemes"]
    from .compress import group_of
    rows = []
    for d in detect:
        row = {"path": d["path"], "dir": d["path"].split("/")[0] if "/" in d["path"] else ".", "class": d["class"],
               "group": group_of(d), "container": d["container"], "width": d["width"], "height": d["height"],
               "channels": d["channels"], "bits_stored": d["bits_stored"], "bits_effective": d["bits_effective"],
               "shift": d["shift"], "writer": d["writer"], "size": d["size"], "sha256": d["sha256"],
               "layout_status": lay.get(d["path"], {}).get("status")}
        for sc in schemes:
            c = comp[d["path"]].get((sc, "file")) or comp[d["path"]].get((sc, "default"))
            if c:
                st = int(float(c["stored"]))
                row[f"{sc}_bytes"] = st
                row[f"{sc}_ratio"] = round(d["size"] / st, 4) if st else None
                row[f"{sc}_status"] = c["status"]
            bd = comp[d["path"]].get((sc, "bayer_direct"))
            if bd:
                row[f"{sc}_bayer_direct_bytes"] = int(float(bd["stored"]))
        if d["path"] in ntfs:
            row["ntfs_bytes"] = ntfs[d["path"]]
            row["ntfs_ratio"] = round(d["size"] / ntfs[d["path"]], 4) if ntfs[d["path"]] else None
        row["notes"] = "；".join(d["notes"] + ([d["error"]] if d["error"] else []))
        rows.append(row)
    keys = list(dict.fromkeys(k for r in rows for k in r))
    with open(out / "details.csv", "w", newline="", encoding="utf-8-sig") as f:
        w = csv.DictWriter(f, keys)
        w.writeheader()
        w.writerows(rows)


# ---------------------------------------------------------------- 图表（内联 SVG，无外部依赖）

def _nice(vmax: float) -> tuple[float, float]:
    """取 1/2/2.5/5×10^k 的整齐步长，刻度不超过 7 个。返回 (步长, 轴上限)。"""
    import math
    vmax = max(vmax, 1e-9)
    mag = 10 ** math.floor(math.log10(vmax / 6))
    for m in (1, 2, 2.5, 5, 10):
        step = m * mag
        if vmax / step <= 6:
            return step, math.ceil(vmax / step) * step
    return 10 * mag, math.ceil(vmax / (10 * mag)) * 10 * mag


def _tick(v: float) -> str:
    return f"{v:g}"


def hbar(items: list[tuple[str, float, str]], unit: str = "", fmt: str = "{:.2f}", series: dict | None = None,
         ref: float | None = None, ref_label: str = "") -> str:
    """水平条形图。items：(标签, 数值, 系列键)；series：{系列键: (CSS 变量, 图例名)}。"""
    if not items:
        return ""
    series = series or {"a": ("--series-1", "")}
    lw, bw, row, gap = 190, 400, 26, 6
    step, vmax = _nice(max(v for _, v, _ in items) * 1.08)
    top = 20 if ref is not None else 6
    h = top + len(items) * row + 24
    out = [f'<svg class="chart" viewBox="0 0 {lw + bw + 90} {h}" role="img">']
    n = round(vmax / step)
    for i in range(n + 1):
        x = lw + bw * i / n
        out.append(f'<line x1="{x:.1f}" y1="{top - 2}" x2="{x:.1f}" y2="{h - 22}" class="grid"/>'
                   f'<text x="{x:.1f}" y="{h - 8}" class="tick" text-anchor="middle">{_tick(step * i)}</text>')
    if ref is not None and ref <= vmax:
        x = lw + bw * ref / vmax
        out.append(f'<line x1="{x:.1f}" y1="{top - 4}" x2="{x:.1f}" y2="{h - 22}" class="refline"/>'
                   f'<text x="{x:.1f}" y="12" class="tick" text-anchor="middle">{_e(ref_label)}</text>')
    for i, (label, v, key) in enumerate(items):
        y = top + i * row
        w = max(2.0, bw * v / vmax)
        var = series[key][0]
        out.append(f'<text x="{lw - 8}" y="{y + row / 2:.1f}" class="lab" text-anchor="end" dominant-baseline="middle">'
                   f'{_e(label)}</text>'
                   f'<g class="bar"><rect x="{lw}" y="{y + gap / 2:.1f}" width="{w:.1f}" height="{row - gap}" rx="4" '
                   f'style="fill:var({var})"><title>{_e(label)}：{fmt.format(v)}{unit}</title></rect></g>'
                   f'<text x="{lw + w + 6:.1f}" y="{y + row / 2:.1f}" class="val" dominant-baseline="middle">'
                   f'{fmt.format(v)}{unit}</text>')
    out.append("</svg>")
    legend = ""
    if len(series) > 1:
        legend = '<div class="legend">' + "".join(
            f'<span><i style="background:var({v})"></i>{_e(n)}</span>' for v, n in series.values()) + "</div>"
    return legend + "".join(out)


# ---------------------------------------------------------------- HTML

CSS = """
:root{--surface:#fcfcfb;--panel:#ffffff;--text:#0b0b0b;--text2:#52514e;--muted:#8a8984;--line:#e4e3df;
--series-1:#2a78d6;--series-2:#eb6834;--series-3:#1baf7a;--good:#0ca30c;--warn:#b07800;--bad:#d03b3b;--hl:#eef4fc}
@media (prefers-color-scheme: dark){:root:not([data-theme="light"]){--surface:#1a1a19;--panel:#222220;--text:#f0efec;
--text2:#c3c2b7;--muted:#8f8e88;--line:#383835;--series-1:#3987e5;--series-2:#d95926;--series-3:#199e70;--hl:#1f2a38}}
:root[data-theme="dark"]{--surface:#1a1a19;--panel:#222220;--text:#f0efec;--text2:#c3c2b7;--muted:#8f8e88;--line:#383835;
--series-1:#3987e5;--series-2:#d95926;--series-3:#199e70;--hl:#1f2a38}
*{box-sizing:border-box}body{margin:0;background:var(--surface);color:var(--text);
font:15px/1.6 "Microsoft YaHei","PingFang SC","Noto Sans CJK SC",system-ui,sans-serif}
main{max-width:1080px;margin:0 auto;padding:24px 16px 64px}h1{font-size:26px;margin:0 0 4px}
h2{font-size:20px;margin:40px 0 12px;padding-top:12px;border-top:1px solid var(--line)}h3{font-size:16px;margin:20px 0 8px}
.sub{color:var(--text2);margin:0 0 16px}.muted{color:var(--muted)}.small{font-size:13px}
.tiles{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:12px;margin:16px 0}
.tile{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:14px 16px}
.tile b{display:block;font-size:28px;line-height:1.2}.tile span{color:var(--text2);font-size:13px}
.tablewrap{overflow-x:auto;margin:8px 0 16px}table{border-collapse:collapse;width:100%;font-size:13.5px;background:var(--panel)}
th,td{border-bottom:1px solid var(--line);padding:6px 10px;text-align:right;white-space:nowrap}
th:first-child,td:first-child,th.l{text-align:left}th{color:var(--text2);font-weight:600;background:var(--surface)}
td.best{font-weight:700;background:var(--hl)}td.l{text-align:left;white-space:normal}
.chart{width:100%;max-width:720px;height:auto;display:block;margin:4px 0 8px}
.chart .grid{stroke:var(--line);stroke-width:1}.chart .refline{stroke:var(--muted);stroke-dasharray:4 3}
.chart text{fill:var(--text2);font-size:12px}.chart .lab{fill:var(--text);font-size:12.5px}.chart .val{fill:var(--text);font-size:12px}
.chart .bar rect:hover{opacity:.8}.legend{display:flex;gap:16px;font-size:13px;color:var(--text2);margin:4px 0}
.legend i{display:inline-block;width:12px;height:12px;border-radius:3px;margin-right:6px;vertical-align:-1px}
.callout{border-left:4px solid var(--series-1);background:var(--panel);padding:10px 14px;margin:12px 0;border-radius:4px}
.warn{border-left-color:var(--warn)}.ok{color:var(--good)}.bad{color:var(--bad)}
ul.concl{padding-left:20px}ul.concl li{margin:4px 0}code{font-size:13px}
@media print{h2{break-before:auto}.tablewrap{overflow:visible}}
"""


def _table(head: list[str], rows: list[list], best_cols: dict | None = None, left: tuple = ()) -> str:
    th = "".join(f"<th{' class="l"' if j in left else ''}>{_e(h)}</th>" for j, h in enumerate(head))
    body = []
    for i, r in enumerate(rows):
        tds = []
        for j, c in enumerate(r):
            cls = ["best"] if best_cols and best_cols.get(i) == j else []
            cls += ["l"] if j in left else []
            tds.append(f"<td{' class="' + ' '.join(cls) + '"' if cls else ''}>{c}</td>")
        body.append("<tr>" + "".join(tds) + "</tr>")
    return f'<div class="tablewrap"><table><thead><tr>{th}</tr></thead><tbody>{"".join(body)}</tbody></table></div>'


def render(data: dict) -> str:
    c = data["compress"]
    sc_all = c["schemes"]
    lab = c["labels"]
    rec = data["recommended"]
    m = data["machine"]
    s = data["sample"]
    parts = []
    add = parts.append

    add(f"<h1>工业图像无损压缩评估报告</h1><p class='sub'>样本目录 <code>{_e(s['dir'])}</code> · "
        f"{s['files']} 个文件 · {_mb(s['bytes'])} MB · 生成于 {_e(data['generated'])}<br>"
        f"测试机：{_e(m['cpu'])}（{m.get('logical_cpus', '?')} 逻辑核），{_e(m.get('os', ''))}，Python {_e(m.get('python', ''))}</p>")

    # ---------- 结论摘要
    zr = c["overall"].get("zstd19", {}).get("ratio")
    nt = (c["ntfs"] or {}).get("ratio")
    st = c["overall"][rec["scheme"]]["status"]
    tiles = [(f"{rec['ratio']:.2f}×", f"{_e(rec['label'])} 总体压缩比（全部文件，含兜底）")]
    if zr:
        tiles.append((f"{zr:.2f}×", "zstd -19 整文件（通用压缩对照）"))
    if nt:
        tiles.append((f"{nt:.2f}×", "Windows NTFS 压缩（对照）"))
    n_mis = st.get("mismatch", 0)
    n_fb = st.get("fallback", 0) + st.get("not_image", 0)
    tiles.append((f"{n_mis} 例", f"校验不一致（{st.get('ok', 0)} 个图像文件逐字节还原一致；"
                                 f"{n_fb} 个非图像或未确认文件按兜底计入）"))
    add("<div class='tiles'>" + "".join(f"<div class='tile'><b>{v}</b><span>{t}</span></div>" for v, t in tiles) + "</div>")
    summary_lines = []
    part = (c.get("parts") or {}).get(rec["scheme"])
    if part:
        a, o = part["coded"], part["other"]
        summary_lines.append(f"总量 {_mb(a['orig'] + o['orig'])} MB → {_mb(a['stored'] + o['stored'])} MB：其中 {a['files']} 个文件做了图像压缩"
                             f"（{_mb(a['orig'])} → {_mb(a['stored'])} MB），其余 {o['files']} 个原样或整文件 zstd 保存"
                             f"（{_mb(o['orig'])} → {_mb(o['stored'])} MB）")
    if zr:
        summary_lines.append(f"{_e(rec['label'])} 比通用压缩 zstd -19 多省 {rec['ratio'] / zr - 1:.0%} 的容量"
                             + (f"，是 NTFS 压缩的 {(1 - 1 / rec['ratio']) / max(1e-9, 1 - 1 / nt):.1f} 倍节省量" if nt and nt > 1 else ""))
    if data["roi"]:
        summary_lines += data["roi"]["text"][:2]
    if data["ceiling"]:
        ds = ""
        for x in data["ceiling"]["conclusions"]:
            if x.startswith("【"):
                ds = "时序序列" if "序列" in x else "Bayer 文件"
            elif " 对 " in x and "→" in x:
                summary_lines.append(f"天花板（{ds}）：{x.strip()}")
    add("<div class='callout'><ul class='concl'>" + "".join(f"<li>{_e(x)}</li>" for x in summary_lines) + "</ul></div>")
    if data["notes"]:
        add("<div class='callout warn'><b>注意</b><ul class='concl'>" + "".join(f"<li>{_e(x)}</li>" for x in data["notes"])
            + "<li>本报告的压缩比只说明在这批样本上的实测结果；不同产线、相机、噪声水平差异很大，对外承诺须以客户样本实测为准。</li></ul></div>")

    # ---------- 1 样本概览
    add("<h2>1 样本概览</h2>")
    rows = [[_e(CLASS_LABELS.get(k, k)), v["files"], _mb(v["bytes"]), f"{v['bytes'] / s['bytes']:.1%}"]
            for k, v in s["classes"].items()]
    add("<h3>格式分布（按内容识别，不看扩展名）</h3>" + _table(["类别", "文件数", "MB", "占比"], rows))
    rows = [[_e(k), v["files"], _mb(v["bytes"])] for k, v in s["groups"].items()]
    add("<h3>格式细分</h3>" + _table(["格式组", "文件数", "MB"], rows))
    rows = [[_e(WRITER_LABELS.get(k, k)), v, f"<span class='small muted'>{_e(s['writer_evidence'].get(k) or '')}</span>"]
            for k, v in s["writers"].items()]
    add("<h3>推测写图程序</h3>" + _table(["写图程序", "文件数", "依据（示例）"], rows, left=(2,))
        + "<p class='small muted'>Halcon / VisionPro / 相机 SDK 目前只按 Software 等文本标签推测，真实指纹需客户样本校准。</p>")

    # ---------- 2 压缩比与速度
    add("<h2>2 各方案压缩比与速度</h2>")
    items = [(lab[k], c["overall"][k]["ratio"], "img" if k in IMAGE_CODECS else "gen") for k in sc_all]
    if c["ntfs"]:
        items.append(("NTFS 压缩", c["ntfs"]["ratio"], "gen"))
    items.sort(key=lambda t: -t[1])
    add("<h3>总体压缩比（原文件总字节 / 存储总字节）</h3>"
        + hbar(items, "×", series={"img": ("--series-1", "标准图像编码"), "gen": ("--series-2", "通用 / 系统压缩对照")},
               ref=2.0, ref_label="2× 门槛"))
    rows = []
    for k in sc_all:
        o, im, sp = c["overall"][k], c["image_only"][k], c["speed"].get(k, {})
        spx = sp.get("excl_py_lzw") or sp
        stc = o["status"]
        rows.append([_e(lab[k]), f"{o['ratio']:.2f}", f"{im['ratio']:.2f}" if im["ratio"] else "—",
                     stc.get("ok", 0), stc.get("fallback", 0) + stc.get("mismatch", 0) + stc.get("not_image", 0),
                     stc.get("stored_raw", 0), f"{spx.get('enc_MBps', float('nan')):.1f}", f"{spx.get('dec_MBps', float('nan')):.1f}"])
    best = min(range(len(rows)), key=lambda i: -float(rows[i][1]))
    add(_table(["方案", "总体", "仅图像文件", "校验通过", "兜底（含非图像）", "原样存储", "编码 MB/s", "解码 MB/s"],
               rows, {best: 1}))
    add(f"<p class='small muted'>{_e(c['ratio_protocol'])}。速度：{_e(c['speed_protocol'])}；"
        f"不含 {'/'.join(PY_SLOW_GROUPS)} 组（其复现式还原在评估工具中用纯 Python 实现，不代表产品）。</p>")

    groups = list(c["by_group"])
    show = [k for k in sc_all]
    rows, bests = [], {}
    for i, g in enumerate(groups):
        vals = [c["by_group"][g][k]["ratio"] for k in show]
        r = [_e(g), c["by_group"][g][show[0]]["files"]] + [f"{v:.2f}" for v in vals]
        if c["ntfs"]:
            r.append(f"{c['ntfs']['by_group'].get(g, float('nan')):.2f}")
        bests[i] = 2 + max(range(len(vals)), key=lambda j: vals[j])
        rows.append(r)
    add("<h3>分格式压缩比</h3>" + _table(["格式组", "文件数"] + [lab[k] for k in show] + (["NTFS"] if c["ntfs"] else []),
                                     rows, bests))
    if len(c["by_dir"]) > 1:
        rows = [[_e("（根目录）" if d == "." else d), c["by_dir"][d][show[0]]["files"]]
                + [f"{c['by_dir'][d][k]['ratio']:.2f}" for k in show]
                for d in c["by_dir"]]
        add("<h3>分目录 / 工位压缩比</h3>" + _table(["目录", "文件数"] + [lab[k] for k in show], rows))
    sp_rows = []
    for k in show:
        bg = c["speed"].get(k, {}).get("by_group", {})
        for g, v in bg.items():
            sp_rows.append((k, g, v))
    if sp_rows:
        gl = sorted({g for _, g, _ in sp_rows})
        rows = [[_e(g)] + [f"{c['speed'].get(k, {}).get('by_group', {}).get(g, {}).get('enc_MBps', float('nan')):.1f} / "
                           f"{c['speed'].get(k, {}).get('by_group', {}).get(g, {}).get('dec_MBps', float('nan')):.1f}"
                           for k in show] for g in gl]
        add("<h3>分格式单核速度（编码 / 解码 MB/s）</h3>" + _table(["格式组"] + [lab[k] for k in show], rows))

    # ---------- 3 NTFS
    add("<h2>3 与 Windows NTFS 压缩对比</h2>")
    if c["ntfs"]:
        nt = c["ntfs"]["ratio"]
        add(f"<p>NTFS 透明压缩（LZNT1）总体 <b>{nt:.2f}×</b>，{_e(rec['label'])} <b>{rec['ratio']:.2f}×</b>。"
            f"节省的容量：NTFS {1 - 1 / nt:.0%}，{_e(rec['label'])} {1 - 1 / rec['ratio']:.0%}。"
            f"LZNT1 只有 LZ77 匹配、没有熵编码，对带噪声的图像几乎无效。</p>"
            f"<p class='small muted'>{_e(c['ntfs_note'])}。分格式对比见第 2 节表格最后一列。</p>")
    else:
        add(f"<p class='muted'>未运行：{_e(c.get('ntfs_note') or '')}</p>")

    # ---------- 4 Bayer
    add("<h2>4 Bayer 拆分收益</h2>")
    if c["bayer"]:
        items = []
        for k, b in c["bayer"].items():
            items += [(f"{lab[k]} 直接压", b["ratio_direct"], "d"), (f"{lab[k]} 拆四平面", b["ratio_split"], "s")]
        add(hbar(items, "×", series={"d": ("--series-2", "直接按灰度压"), "s": ("--series-1", "拆 R/Gr/Gb/B 四平面")}))
        rows = [[_e(lab[k]), b["files"], f"{b['ratio_direct']:.2f}", f"{b['ratio_split']:.2f}", f"-{b['saving']:.1%}"]
                for k, b in c["bayer"].items()]
        add(_table(["方案", "文件数", "直接压", "拆四平面", "体积变化"], rows))
    else:
        add("<p class='muted'>样本中没有识别为 Bayer 的文件。</p>")

    # ---------- 5 复现式还原
    add("<h2>5 复现式还原（已压缩 TIFF）</h2>")
    lay = data["layout"]
    if lay and lay["reproduce"]["total"]:
        rp = lay["reproduce"]
        add(f"<p>已压缩 TIFF {rp['total']} 个，复现成功 <b>{rp['ok']}</b> 个（{rp['ok'] / rp['total']:.0%}）。"
            f"成功的文件只存像素码流 + 编码参数，读取时重新编码出与原文件逐字节一致的块；失败的整文件 zstd 兜底。</p>")
        rows = [[_e(ENCODER_LABELS.get(k, k)), v] for k, v in rp["by_encoder"].items()]
        add(_table(["复现编码器", "文件数"], rows))
        if rp.get("libtiff_lzw_segments"):
            add(f"<p class='small muted'>其中 {rp['libtiff_lzw_segments']} 个 LZW 条带必须用 libtiff 兼容编码器才能复现"
                f"（imagecodecs 的 LZW 在码表填满后与 libtiff 不同）。</p>")
        if rp["failed"]:
            add(_table(["失败文件", "原因"], [[_e(f["path"]), _e(f["reason"])] for f in rp["failed"]], left=(1,)))
    elif lay:
        add("<p class='muted'>样本中没有已压缩的 TIFF。</p>")
    if lay:
        st2 = lay["status"]
        add(f"<p class='small muted'>版面拆分再重组（不压缩）自检：一致 {st2.get('ok', 0)}，不一致 {st2.get('mismatch', 0)}，"
            f"兜底 {st2.get('fallback', 0)}，非图像 {st2.get('not_image', 0)}。</p>")

    # ---------- 6 天花板
    add("<h2>6 天花板测试（领域预测还能多省多少）</h2>")
    ce = data["ceiling"]
    if ce:
        rows = []
        for name, label in (("sequence", "时序序列"), ("bayer", "Bayer 文件")):
            t = ce.get(name)
            if not t:
                continue
            for g in ("G1", "G2", "G3a", "G3b", "G3c"):
                if g in t:
                    extra = f"（{t[g]['codec']}）" if "codec" in t[g] else ""
                    rows.append([_e(label), g, f"{t[g]['ratio']:.2f}{extra}", t[g].get("failed", 0)])
        add(_table(["数据", "档位", "压缩比", "校验失败"], rows))
        add("<ul class='concl'>" + "".join(f"<li>{_e(x.strip())}</li>" for x in ce["conclusions"]) + "</ul>")
        add("<p class='small muted'>G1 标准版面单帧；G2 加移位 / 有效位深 / Bayer 拆分；G3a 背景参考帧残差；G3b 帧间块运动补偿；"
            "G3c 用 G 平面预测 R/B。判读：相对 G2 体积减少 &lt;5% 无明显收益，5~15% 有收益，≥15% 显著（定义见 bench/FINDINGS.md 第 2 节）。</p>")
    else:
        add(f"<p class='muted'>未运行：{_e(data['meta'].get('ceiling_skipped', '未提供序列'))}</p>")

    bgp = data.get("bg_potential")
    if bgp:
        add("<h3>背景压缩潜力评估（固定相机、背景基本不变时）</h3>")
        rows = [["时间噪声 σ", f"{bgp['temporal_sigma']} DN"], ["静态细节（纹理 + 固定模式噪声）", f"{bgp['static_detail']} DN"],
                ["列条纹 / 热像素", f"{bgp['column_fpn']} DN / {bgp['hot_pixels']} 个"],
                ["背景帧内编码（JPEG-LS）", f"{bgp['intra_bpp']} bit/像素"],
                ["背景参考后残差编码", f"{bgp['residual_bpp']} bit/像素"], ["理论下限（只剩时间噪声）", f"{bgp['floor_bpp']} bit/像素"],
                ["背景区可省（实际 / 上限）", f"{bgp['bg_saving']:.1%} / {bgp['bg_saving_ceiling']:.1%}"]]
        if "bg_fraction" in bgp:
            rows += [["背景占画面", f"{bgp['bg_fraction']:.0%}"],
                     ["全帧估计可省（估计 / 上限）", f"{bgp['frame_saving_est']:.1%} / {bgp['frame_saving_ceiling']:.1%}"]]
        add(_table(["指标", "数值"], rows) + f"<p><b>结论：</b>{_e(bgp['verdict'])}</p>"
            + "<p class='small muted'>相对 JPEG-LS 帧内编码；无损前提下背景参考去不掉时间噪声，收益主要来自背景纹理与固定模式噪声。"
              "估计值未计模式图与分区边界开销，合成数据扫描中平均比实测乐观约 2.5 个百分点。</p>")

    # ---------- 7 CPU 影响
    add("<h2>7 CPU 影响（后台压缩对检测的影响）</h2>")
    cpu = data["cpuload"]
    if cpu:
        p = cpu["params"]
        add(f"<p>模拟工控机：{len(p['cpus'])} 个逻辑核，{p['inspect_procs']} 个检测进程，目标负载 {p['target_load']:.0%}，"
            f"单次检测 {p.get('job_ms_concurrent', p.get('job_ms', 0)):.1f} ms，周期 {p['period_ms']:.1f} ms；"
            f"后台压缩路径：{_e(p['compress_path'])}。</p>")
        if not cpu.get("reliable", True):
            add(f"<div class='callout warn'>测试前外部负载 {cpu.get('ambient_busy_pct')}%，各次基线最大差异 "
                f"{cpu['baseline_drift_pct']}%：机器不安静，以下结果仅供参考。</div>")
        rows = [[_e(r["config"]), f"{r['mean_delta_pct']:+.1f}%", f"{r['p95_delta_pct']:+.1f}%", f"{r['p99_delta_pct']:+.1f}%",
                 r["over_period"], r.get("over_period_baseline", "—"), f"{r['compress_MBps']:.1f}"]
                for r in cpu["rows"] if r["priority"] != "-"]
        add(_table(["配置（Δ 相对前后两次基线）", "均值", "P95", "P99", "超周期", "基线超周期", "压缩 MB/s"], rows))
    else:
        add(f"<p class='muted'>未运行：{_e(data['meta'].get('cpuload_skipped', ''))}</p>")

    # ---------- 8 失败与兜底
    add("<h2>8 失败与兜底明细</h2>")
    fails = data["failures"]
    if fails:
        agg = Counter((f["path"], f["status"], f["reason"]) for f in fails)
        rows = [[_e(pth), _e(STATUS_LABELS.get(stt, stt)), n, _e(rsn)]
                for (pth, stt, rsn), n in list(agg.items())[:200]]
        add(f"<p>共 {len(fails)} 条（按文件与原因合并，“方案数”为受影响的方案个数）。"
            f"往返校验不一致的会被检出并按兜底计入，不会计入压缩比。</p>"
            + _table(["文件", "处理", "方案数", "原因"], rows, left=(3,)))
    else:
        add("<p class='ok'>无。全部可处理文件在全部方案下逐字节校验通过。</p>")
    nonimg = [(k, v) for k, v in s["classes"].items() if k in ("unknown", "unsupported")]
    if nonimg:
        add("<p class='small muted'>非图像 / 不处理的文件（" + "，".join(f"{CLASS_LABELS[k]} {v['files']} 个" for k, v in nonimg)
            + "）按 zstd 或原样存储，已计入“总体”压缩比。</p>")

    # ---------- 9 ROI
    add("<h2>9 ROI 代入（工控机本地硬盘）</h2>")
    r = data["roi"]
    if r:
        i = r["inputs"]
        if r["example"]:
            add("<div class='callout warn'>未提供实际参数，以下为<b>示例参数</b>。用 <code>python -m bench report &lt;输出目录&gt; "
                "--daily-gb … --disk-tb … --retention-days …</code> 代入客户数据后重新生成。</div>")
        add(f"<p>输入：每天出图 {i['daily_gb']:g} GB（调试期 {i['debug_daily_gb']:g} GB），图像盘 {i['disk_tb']:g} TB"
            f"（预留 {i['reserve']:.0%}），招标保留 {i['retention_days']:g} 天；压缩比取实测 {_e(data['recommended']['label'])} "
            f"总体 {i['ratio']:.2f}。</p>")
        add("<ul class='concl'>" + "".join(f"<li>{_e(x)}</li>" for x in r["text"]) + "</ul>")
        rows = [[k, r["disks_before"][k], r["disks_after"][k]] for k in r["disks_before"]]
        add(_table(["盘规格", "压缩前需块数", "压缩后需块数"], rows))
    else:
        add("<p class='muted'>未提供 ROI 参数。</p>")

    # ---------- 附录
    add("<h2>附录：测试口径与环境</h2><ul class='concl small'>")
    add(f"<li>压缩比口径：{_e(c['ratio_protocol'])}。</li>")
    add(f"<li>速度口径：{_e(c['speed_protocol'])}。</li>")
    add("<li>每个文件的每个方案都做“编码 → 解码 → 按原版面重组 → SHA-256 与原文件比对”，只有一致才计入压缩结果；"
        "整文件 zstd 兜底同样解压比对。</li>")
    add("<li>大小单位与 Windows 资源管理器一致：1 MB = 1024×1024 字节，统计的是文件实际大小，不含磁盘按簇分配的“占用空间”；"
        "ROI 的 GB / TB 按硬盘厂商的十进制口径。</li>")
    add("<li>编码库版本：" + "，".join(f"{_e(k)} {_e(v)}" for k, v in c["lib_versions"].items()) + "。</li>")
    ph = data["meta"].get("phases", {})
    if ph:
        add("<li>各阶段耗时：" + "，".join(f"{_e(k)} {v:g}s" for k, v in ph.items()) + "。</li>")
    add("<li>逐文件明细见同目录 details.csv；机器可读汇总见 summary.json；各阶段原始结果见子目录。</li></ul>")

    return (f"<!doctype html><html lang='zh-CN'><head><meta charset='utf-8'>"
            f"<meta name='viewport' content='width=device-width,initial-scale=1'><title>图像压缩评估报告</title>"
            f"<style>{CSS}</style></head><body><main>{''.join(parts)}</main></body></html>")


def build(out: Path, roi_args: dict | None, scheme: str = "jpegls") -> dict:
    data = collect(out, roi_args, scheme)
    (out / "summary.json").write_text(json.dumps(data, ensure_ascii=False, indent=1), encoding="utf-8")
    write_details(out, data)
    (out / "report.html").write_text(render(data), encoding="utf-8")
    return data


EXAMPLE_ROI = {"daily_gb": 200.0, "debug_daily_gb": 600.0, "disk_tb": 10.0, "retention_days": 90.0, "reserve": 0.10}


def roi_from_args(a) -> dict:
    vals = {"daily_gb": a.daily_gb, "debug_daily_gb": a.debug_daily_gb, "disk_tb": a.disk_tb,
            "retention_days": a.retention_days}
    if a.daily_gb is None and a.daily_images and a.image_mb:
        vals["daily_gb"] = a.daily_images * a.image_mb / 1000
    given = vals["daily_gb"] is not None and a.disk_tb is not None and a.retention_days is not None
    params = dict(EXAMPLE_ROI) if not given else {**vals, "debug_daily_gb": vals["debug_daily_gb"] or vals["daily_gb"]}
    params["reserve"] = a.reserve
    return {"example": not given, "params": params}
