/* 图像压缩评估 GUI 前端。接口见 bench/gui/app.py 的 Api；
   用浏览器直接打开并加 ?mock=idle|running|done 时使用内置演示数据（用于预览与截图）。 */
"use strict";

const ICONS = {
  play: '<path d="M7 4l12 8-12 8z"/>',
  list: '<rect x="4" y="3" width="16" height="18" rx="2"/><path d="M8 8h8M8 12h8M8 16h5"/>',
  chart: '<path d="M4 19V5M4 19h16M8 15l4-4 3 3 5-6"/>',
  tune: '<path d="M4 7h10M18 7h2M4 17h4M12 17h8"/><circle cx="16" cy="7" r="2"/><circle cx="10" cy="17" r="2"/>',
  folder: '<path d="M3 7a2 2 0 012-2h4l2 2h8a2 2 0 012 2v8a2 2 0 01-2 2H5a2 2 0 01-2-2z"/>',
  report: '<rect x="4" y="3" width="16" height="18" rx="2"/><path d="M8 8h8M8 12h8M8 16h5"/>',
  stop: '<rect x="6" y="6" width="12" height="12" rx="2" fill="currentColor" stroke="none"/>',
  info: '<circle cx="12" cy="12" r="9"/><path d="M12 11v5M12 7.5v.5"/>',
  shield: '<path d="M12 3l7 3v5c0 4.5-3 8-7 10-4-2-7-5.5-7-10V6z"/><path d="M9 12l2 2 4-4"/>',
  done: '<circle cx="12" cy="12" r="9"/><path d="M8 12l3 3 5-6"/>',
  run: '<circle cx="12" cy="12" r="9" stroke-dasharray="4 3"/>',
  wait: '<circle cx="12" cy="12" r="9"/>',
  skip: '<circle cx="12" cy="12" r="9"/><path d="M8 12h8"/>',
  alert: '<circle cx="12" cy="12" r="9"/><path d="M12 7v6M12 16.5v.5"/>',
  check: '<path d="M5 12l5 5 9-10"/>',
  warn: '<path d="M12 4l9 16H3z"/><path d="M12 10v4M12 17v.5"/>',
  trash: '<path d="M4 7h16M10 11v6M14 11v6M6 7l1 13h10l1-13M9 7V4h6v3"/>',
  restart: '<path d="M4 12a8 8 0 108-8M4 4v5h5"/>',
  arrow: '<path d="M5 12h14M13 6l6 6-6 6"/>',
};
const svg = (n, sw = 1.8) => `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="${sw}" stroke-linecap="round" stroke-linejoin="round">${ICONS[n] || ""}</svg>`;
const $ = (s) => document.querySelector(s);
const $$ = (s) => [...document.querySelectorAll(s)];
const esc = (s) => String(s ?? "").replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
const LABELS = { zstd3: "zstd -3", zstd19: "zstd -19", png: "PNG", jpegls: "JPEG-LS", htj2k: "HTJ2K",
  jxl3: "JPEG XL e3", jxl7: "JPEG XL e7", jxl9: "JPEG XL e9", ntfs: "NTFS" };
const ORDER = ["jpegls", "jxl9", "jxl7", "jxl3", "htj2k", "png", "zstd19", "zstd3", "ntfs"];
const BASE = new Set(["zstd3", "zstd19", "ntfs"]);

// 与 Windows 资源管理器同一口径：1 KB = 1024 字节，1 MB = 1024² 字节（文件实际大小，不含磁盘簇补齐）
function fmtBytes(b) {
  // 像资源管理器一样保留 3 位有效数字：7.16 MB、72.5 MB、277 MB
  const sig = (v) => (v >= 100 ? v.toFixed(0) : v >= 10 ? v.toFixed(1) : v.toFixed(2));
  if (b >= 1000 * 1024 ** 2) return sig(b / 1024 ** 3) + " GB";
  if (b >= 1000 * 1024) return sig(b / 1024 ** 2) + " MB";
  return sig(b / 1024) + " KB";
}
const exact = (b) => (b ? `${Math.round(b).toLocaleString()} 字节` : "");
function fmtTime(s) {
  s = Math.round(s || 0);
  if (s < 60) return `${s} 秒`;
  const m = Math.floor(s / 60);
  return m < 60 ? `${m} 分 ${s % 60} 秒` : `${Math.floor(m / 60)} 小时 ${m % 60} 分`;
}
const ratio = (r) => (r ? r.toFixed(2) + "×" : "—");

/* ---------------------------------------------------------------- 接口 */
let api = null;
const S = { info: null, settings: null, folder: null, opts: {}, snap: null, history: [], polling: null, page: "test" };

function ready() {
  const mock = new URLSearchParams(location.search).get("mock");
  if (mock) { api = mockApi(mock); return Promise.resolve(); }
  return new Promise((res) => {
    if (window.pywebview && window.pywebview.api) { api = window.pywebview.api; res(); }
    else window.addEventListener("pywebviewready", () => { api = window.pywebview.api; res(); });
  });
}

/* ---------------------------------------------------------------- 通用 */
function injectIcons(root = document) {
  root.querySelectorAll("[data-icon]").forEach((el) => {
    el.outerHTML = svg(el.dataset.icon, el.dataset.icon === "check" ? 3 : 1.8);
  });
}
function applyTheme(t) {
  if (t === "light" || t === "dark") document.documentElement.dataset.theme = t;
  else delete document.documentElement.dataset.theme;
}
function setSeg(el, v) { el.querySelectorAll("button").forEach((b) => b.classList.toggle("on", b.dataset.v === v)); }
function confirmDlg(title, text, yes = "删除") {
  return new Promise((res) => {
    $("#dlg-t").textContent = title; $("#dlg-d").textContent = text; $("#dlg-yes").textContent = yes;
    $("#mask").classList.add("on");
    const done = (v) => { $("#mask").classList.remove("on"); $("#dlg-yes").onclick = $("#dlg-no").onclick = null; res(v); };
    $("#dlg-yes").onclick = () => done(true); $("#dlg-no").onclick = () => done(false);
  });
}
function go(page) {
  S.page = page;
  $$(".nav").forEach((n) => n.classList.toggle("on", n.dataset.page === page));
  $$(".page").forEach((p) => p.classList.toggle("on", p.id === "page-" + page));
  if (page === "history") renderHistory();
  if (page === "roi") renderRoiRuns();
}

/* ---------------------------------------------------------------- 快速测试 */
async function pickFolder() {
  const p = await api.pick_folder(S.folder?.path || "");
  if (p) await setFolder(p);
}
async function setFolder(p) {
  const r = await api.inspect(p);
  S.folder = r.ok ? r : null;
  renderFolder(r.ok ? null : r.error);
  renderRun();
}
function renderFolder(err) {
  const f = S.folder;
  $("#path").textContent = f ? f.path : "还没有选择文件夹";
  $("#path").classList.toggle("empty", !f);
  $("#path-meta").textContent = f ? `${f.files.toLocaleString()} 个文件 · ${fmtBytes(f.bytes)}` : "";
  $("#pick").innerHTML = svg("folder") + (f ? "换一个文件夹" : "选择文件夹");
  const chips = [];
  if (err) chips.push(`<span class="chip warn">${svg("warn", 2.2)}${esc(err)}</span>`);
  if (f) {
    chips.push(f.raw_config ? `<span class="chip ok">${svg("check", 2.6)}找到 raw_config.yaml</span>`
      : `<span class="chip warn">${svg("warn", 2.2)}没有 raw_config.yaml（无头 RAW 只给候选参数）</span>`);
    chips.push(f.frames >= 2 && f.backgrounds
      ? `<span class="chip ok">${svg("check", 2.6)}找到序列 ${f.frames} 帧 + 空载背景 ${f.backgrounds} 张</span>`
      : `<span class="chip">${svg("info")}没有 sequence/ 序列，天花板测试会跳过</span>`);
    if (f.inside_output) chips.push(`<span class="chip warn">${svg("warn", 2.2)}与结果保存位置重叠，请换文件夹或改设置</span>`);
  }
  $("#chips").innerHTML = chips.join("");
}
function renderOpts(lock) {
  setSeg($("#mode"), S.opts.mode);
  $$("#page-test .chk").forEach((c) => {
    c.classList.toggle("on", !!S.opts[c.dataset.opt]);
    c.classList.toggle("dis", !!lock);
  });
  $$("#mode button").forEach((b) => (b.disabled = !!lock));
  $("#pick").disabled = !!lock;
}
async function startRun() {
  const r = await api.start({ path: S.folder.path, ...S.opts });
  if (!r.ok) { renderRun(r.error); return; }
  poll();
}
function poll() {
  clearInterval(S.polling);
  const tick = async () => {
    S.snap = await api.poll();
    renderRun(); renderLive(); renderFails();
    if (!S.snap || S.snap.status !== "running") { clearInterval(S.polling); S.polling = null; loadHistory(); }
  };
  tick();
  S.polling = setInterval(tick, 500);
}
function phaseText(p) {
  if (p.status === "skip") return "跳过";
  if (p.status === "wait") return "等待";
  const t = p.total ? `${p.done} / ${p.total}` : "";
  if (p.status === "done") return [t, p.secs != null ? `${p.secs} s` : ""].filter(Boolean).join(" · ") || "完成";
  return t || p.msg || "进行中";
}
function renderRun(err) {
  const s = S.snap;
  const running = s && s.status === "running";
  renderOpts(running);
  const acts = [], body = [];
  let title = "② 开始测试", sub = "";
  if (!s || (!running && S.folder && s.sample !== S.folder.path)) {
    sub = S.folder ? "准备好了：点“开始测试”" : "先在上面选择样本文件夹";
    acts.push(`<button class="btn-p ic" id="go" ${S.folder && !S.folder.inside_output ? "" : "disabled"}>${svg("play")}开始测试</button>`);
    const plan = ["识别格式", "拆分重组自检", "压缩对比（" + (S.opts.mode === "full" ? "8" : "6") + " 种方案）",
      S.opts.mode === "full" ? "单核测速（全部方案，各 3 次）" : "单核测速（JPEG-LS / HTJ2K / zstd）"];
    if (S.opts.ntfs) plan.push("NTFS 对照");
    if (S.opts.ceiling && S.folder && S.folder.frames >= 2) plan.push("天花板测试 + 背景潜力评估");
    if (S.opts.cpuload) plan.push("CPU 影响（需要机器空闲）");
    plan.push("生成报告");
    body.push(`<div class="hint">${svg("info")}<span>将依次进行：${plan.join(" → ")}。每个文件都会逐字节还原核对。</span></div>`);
    if (S.opts.mode === "full")
      body.push(`<div class="hint warn">${svg("warn")}<span>完整模式多跑 JPEG XL e7 / e9 两个归档档：编码极慢（e9 约 0.6 MB/s，一张 6 MB 的图约 20 秒），实测只比 JPEG-LS 多省 2%~4%；测速也重复 3 次。只想看结论用快速模式。</span></div>`);
  } else {
    const cpu = S.info ? S.info.cpu.replace(/\(R\)|\(TM\)|CPU|@.*$/g, "").replace(/\s+/g, " ").trim() : "";
    if (running) {
      title = "② 正在测试";
      sub = `${cpu} · ${S.settings.jobs} 个进程并行 · 已用 ${fmtTime(s.elapsed)}`;
      acts.push(`<button class="btn ic" id="stop">${svg("stop")}停止</button>`,
        `<button class="btn-p ic" disabled>${svg("report")}查看报告</button>`);
    } else {
      title = { done: "② 测试完成", cancelled: "② 已停止", error: "② 测试出错" }[s.status];
      sub = `用时 ${fmtTime(s.elapsed)} · 结果在 ${s.out.split(/[\\/]/).pop()}`;
      acts.push(`<button class="btn ic" id="open-out">${svg("folder")}打开输出文件夹</button>`);
      if (s.status === "done") acts.push(`<button class="btn-p ic" id="open-rep">${svg("report")}查看报告</button>`);
      else acts.push(`<button class="btn-p ic" id="go">${svg("restart")}重新开始</button>`);
    }
    const cur = s.current;
    const curText = running && cur ? `${cur.label}${cur.total ? `：${cur.done} / ${cur.total}` : ""}${cur.msg ? " · " + cur.msg : ""}`
      : ({ done: "全部完成", cancelled: "已停止，已完成的阶段结果保留在输出文件夹", error: "出错了" }[s.status] || "");
    body.push(`<div class="ovl"><b>${Math.floor(s.status === "done" ? 100 : s.overall)}%</b><span>${esc(curText)}</span></div>`,
      `<div class="bar ${s.status === "done" ? "done" : ""}" style="margin-top:8px"><i style="width:${s.status === "done" ? 100 : s.overall}%"></i></div>`,
      `<div class="phases">` + s.phases.map((p) => {
        const w = p.status === "done" || p.status === "skip" ? 100 : (p.total ? (100 * p.done) / p.total : (p.status === "run" ? 8 : 0));
        return `<span class="ic ${p.status}">${svg(p.status === "done" ? "done" : p.status === "run" ? "run" : p.status === "skip" ? "skip" : "wait", 2)}</span>` +
          `<span>${esc(p.label)}</span><div class="bar ${p.status === "done" ? "done" : ""}"><i style="width:${w}%"></i></div>` +
          `<span class="v ${p.status === "run" ? "run" : ""}" title="${esc(p.msg)}">${esc(phaseText(p))}</span>`;
      }).join("") + `</div>`);
    if (s.status === "done") {
      const r = s.result;
      body.push(r && r.mismatch ? `<div class="hint err">${svg("alert")}<span>有 ${r.mismatch} 例还原不一致（已按兜底计入，不算进压缩比），详见报告第 8 节。</span></div>`
        : `<div class="hint ok">${svg("shield")}<span>${r ? r.ok : ""} 个图像文件全部逐字节还原一致。</span></div>`);
    }
    if (s.status === "error") body.push(`<div class="hint err">${svg("alert")}<span class="selectable">${esc(s.error)}</span></div>`);
  }
  if (err) body.push(`<div class="hint err">${svg("alert")}<span>${esc(err)}</span></div>`);
  $("#run-title").textContent = title;
  $("#run-sub").textContent = sub;
  $("#run-sub").title = s ? s.out : "";
  $("#run-acts").innerHTML = acts.join("");
  $("#run-body").innerHTML = body.join("");
  $("#go") && ($("#go").onclick = startRun);
  $("#stop") && ($("#stop").onclick = () => api.stop());
  $("#open-out") && ($("#open-out").onclick = () => api.open_path(s.out));
  $("#open-rep") && ($("#open-rep").onclick = () => api.open_report(s.report));
}
function renderLive() {
  const s = S.snap;
  if (!s) { $("#live").innerHTML = `<div class="empty">${svg("chart")}还没有结果</div>`; return; }
  const fin = s.status === "done" && s.result;
  const live = { ...(s.live || {}) };
  if (fin) { s.result.schemes.forEach((x) => (live[x.id] = x.ratio)); if (s.result.ntfs) live.ntfs = s.result.ntfs; }
  const ref = ["jpegls", "htj2k", "png"].find((k) => live[k]);
  const r = ref ? live[ref] : null;
  const mism = fin ? s.result.mismatch : s.mismatch;
  const comp = s.phases.find((p) => p.key === "compress");
  $("#live-sub").textContent = fin ? `${s.result.files} 个文件 · ${fmtBytes(s.result.bytes)} · 最终结果`
    : comp && comp.done ? `已核对 ${comp.done} / ${comp.total} 个文件 · 按已核对部分推算整个文件夹` : "等待压缩对比开始";
  // 体积：完成后用实际存储字节；测试中按已核对文件的压缩比推算整个文件夹（标“预计”）
  const total = fin ? s.result.bytes : (s.total_bytes || (S.folder ? S.folder.bytes : 0));
  const sizes = {};
  if (fin) {
    s.result.schemes.forEach((x) => (sizes[x.id] = x.stored));
    if (s.result.ntfs_stored) sizes.ntfs = s.result.ntfs_stored;
  } else Object.keys(live).forEach((k) => (sizes[k] = live[k] ? total / live[k] : null));
  const after = ref ? sizes[ref] : null;
  const est = fin ? "" : "预计";
  const keys = ORDER.filter((k) => live[k]);
  const mx = Math.max(...keys.map((k) => live[k]), 1) * 1.04;
  $("#live").innerHTML = `<div class="cmp">
      <div class="cmp-h">
        <div><span>原始大小</span><b title="${exact(total)}">${total ? fmtBytes(total) : "—"}</b>
          <small>${total ? exact(total) : ""}</small>${s.disk_bytes ? `<small title="按簇补齐的磁盘占用，与资源管理器“占用空间”同口径">占用空间约 ${fmtBytes(s.disk_bytes)}</small>` : ""}</div>
        <div class="arrow">${svg("arrow", 2)}</div>
        <div><span>${est}压缩后 · ${ref ? LABELS[ref] : "JPEG-LS"}</span><b class="acc" title="${exact(after)}">${after ? fmtBytes(after) : "—"}</b>
          <small>${after ? (fin ? "" : "约 ") + exact(after) : ""}</small></div>
      </div>
      <div class="cb"><i class="o" style="width:100%"></i></div>
      <div class="cb"><i style="width:${after && total ? Math.max(1, (100 * after) / total) : 0}%"></i></div>
      <div class="cmp-f">${r ? `<b>${ratio(r)}</b><span>${est}能省 ${fmtBytes(total - after)}（${Math.round((1 - 1 / r) * 100)}%）</span>`
        : `<span class="muted">压缩对比开始后显示</span>`}</div>
      ${fin && s.result.parts ? partsLine(s.result.parts) : ""}
    </div>
    <div class="tiles">
      <div class="tile"><b>${ratio(live.ntfs)}</b><span>NTFS 对照${sizes.ntfs ? " · " + fmtBytes(sizes.ntfs) : ""}</span></div>
      <div class="tile ${mism ? "bad" : "good"}"><b>${mism} 例</b><span>还原不一致</span></div></div>` +
    (keys.length ? `<div class="rows">` + keys.map((k) => `<span class="l">${LABELS[k]}</span><div class="bar ${BASE.has(k) ? "base" : ""}"><i style="width:${(100 * live[k]) / mx}%"></i></div>` +
      `<span class="r"><em>${sizes[k] ? fmtBytes(sizes[k]) : ""}</em>${ratio(live[k])}</span>`).join("") + `</div>
      <p class="muted" style="margin:10px 0 0">灰色是通用压缩对照。大小按 Windows 资源管理器的口径（1 MB = 1024×1024 字节），是文件实际大小，不含磁盘簇补齐的“占用空间”。${fin ? "压缩后为实际存储体积（含文件头、配方等全部开销）；分格式、速度和 ROI 见报告。"
        : "测试中的大小按已核对文件的压缩比推算，完成后为实际值。"}</p>` : "");
}
function partsLine(p) {
  const a = p.coded, o = p.other;
  const seg = (n, x, what) => n ? `<div class="part"><b>${n}</b> 个${what}<span class="mono">${fmtBytes(x.orig)} → ${fmtBytes(x.stored)}</span></div>` : "";
  return `<div class="parts">${seg(a.files, a, "做了图像压缩")}${seg(o.files, o, "原样或整文件 zstd")}</div>`;
}
function renderFails() {
  const f = S.snap ? S.snap.failures : [];
  $("#fail-card").style.display = f && f.length ? "" : "none";
  $("#fails").innerHTML = (f || []).map((x) => `<div class="fail">${svg("alert", 2)}<span>${esc(x.name)}${x.tag ? `<span class="tag">${esc(x.tag)}</span>` : ""}</span>` +
    `<span class="fp">${esc(x.dir)}</span><span class="fr">${esc(x.reason)}</span></div>`).join("");
}

/* ---------------------------------------------------------------- 测试记录 */
async function loadHistory() {
  S.history = await api.history();
  $("#recent").innerHTML = S.history.slice(0, 6).map((h, i) => `<div class="recent" data-i="${i}" title="${esc(h.sample)}">
    <span>${esc((h.sample || h.name).split(/[\\/]/).pop())}</span><span>${h.brief ? ratio(h.brief.ratio) : "未完成"}</span></div>`).join("") ||
    `<div class="recent"><span class="muted">还没有测试</span></div>`;
  $$("#recent .recent[data-i]").forEach((el) => (el.onclick = () => { const h = S.history[+el.dataset.i]; if (h.report) api.open_report(h.report); }));
  if (S.page === "history") renderHistory();
}
function renderHistory() {
  $("#hist-root").textContent = S.info ? S.info.output_root : "";
  $("#hist").innerHTML = S.history.length ? S.history.map((h, i) => `<div class="hist">
      <div class="n">${esc((h.sample || "").split(/[\\/]/).pop() || h.name)} <span class="k" style="font-weight:400">· ${esc(h.started || "")}${h.elapsed ? " · 用时 " + fmtTime(h.elapsed) : ""}</span></div>
      <div class="s" title="${esc(h.sample)}">${esc(h.sample)}</div>
      <div class="k">${h.brief ? `<span class="big">${ratio(h.brief.ratio)}</span>　<span class="mono">${fmtBytes(h.brief.bytes)} → ${h.brief.stored ? fmtBytes(h.brief.stored) : "—"}</span> · ${esc(h.brief.label)} · ${h.brief.files} 个文件 · NTFS ${ratio(h.brief.ntfs)} · 不一致 ${h.brief.mismatch} 例`
        : ({ running: "正在测试…", cancelled: "已停止，只有部分阶段的结果", error: "出错，只有部分阶段的结果" }[h.status] || "未完成，只有部分阶段的结果")}</div>
      <div class="acts">
        ${h.report ? `<button class="btn ic sm" data-a="rep" data-i="${i}">${svg("report")}查看报告</button>` : ""}
        <button class="btn ic sm" data-a="dir" data-i="${i}">${svg("folder")}打开文件夹</button>
        <button class="btn-d sm" style="height:30px;padding:0 10px" data-a="del" data-i="${i}">${svg("trash")}删除</button>
      </div></div>`).join("") : `<div class="empty">${svg("list")}还没有测试记录</div>`;
  $$("#hist button[data-a]").forEach((b) => (b.onclick = async () => {
    const h = S.history[+b.dataset.i];
    if (b.dataset.a === "rep") api.open_report(h.report);
    if (b.dataset.a === "dir") api.open_path(h.path);
    if (b.dataset.a === "del" && await confirmDlg("删除这次测试结果？", `将删除文件夹 ${h.path}（只删测试结果，不碰样本）。删除后无法恢复。`)) {
      const r = await api.delete_run(h.path);
      if (!r.ok) alert(r.error);
      loadHistory();
    }
  }));
}

/* ---------------------------------------------------------------- ROI */
function roiParams() {
  return { daily_gb: +$("#roi-daily").value, debug_daily_gb: +$("#roi-debug").value || +$("#roi-daily").value,
    disk_tb: +$("#roi-disk").value, retention_days: +$("#roi-days").value, reserve: (+$("#roi-reserve").value || 0) / 100 };
}
function renderRoiRuns() {
  const done = S.history.filter((h) => h.brief);
  const cur = $("#roi-run").value;
  $("#roi-run").innerHTML = done.map((h) => `<option value="${esc(h.path)}">${esc((h.sample || "").split(/[\\/]/).pop())} · ${esc(h.started)} · ${ratio(h.brief.ratio)}</option>`).join("") ||
    `<option value="">还没有完成的测试</option>`;
  if (cur && done.some((h) => h.path === cur)) $("#roi-run").value = cur;
  calcRoi();
}
let roiTimer = null;
function calcRoi() {
  clearTimeout(roiTimer);
  roiTimer = setTimeout(async () => {
    const path = $("#roi-run").value;
    if (!path) { $("#roi-out").innerHTML = `<div class="empty">${svg("chart")}先完成一次测试</div>`; $("#roi-apply").disabled = true; return; }
    const r = await api.roi(path, roiParams());
    $("#roi-apply").disabled = !r.ok;
    if (!r.ok) { $("#roi-out").innerHTML = `<div class="hint err">${svg("alert")}<span>${esc(r.error)}</span></div>`; return; }
    $("#roi-sub").textContent = `压缩比取 ${r.label} 实测 ${ratio(r.ratio)}`;
    $("#roi-out").innerHTML = `<div class="metrics">
      <div class="metric"><div class="lab">现有硬盘可保留</div><div class="val"><s>${r.keep_days_before} 天</s><b>${r.keep_days_after}</b><em>天</em></div></div>
      <div class="metric"><div class="lab">调试期爆盘周期</div><div class="val"><s>${r.debug_fill_days_before} 天</s><b>${r.debug_fill_days_after}</b><em>天</em></div></div>
      <div class="metric"><div class="lab">满足保留天数需配硬盘</div><div class="val"><s>${r.need_tb_before} TB</s><b>${r.need_tb_after}</b><em>TB · 少配 ${r.saved_tb} TB</em></div></div></div>
      <div class="hint ${r.meets_retention_after ? "ok" : "err"}">${svg(r.meets_retention_after ? "shield" : "alert")}<span>${esc(r.text[3])}</span></div>`;
  }, 150);
}

/* ---------------------------------------------------------------- 设置 */
function renderSettings() {
  const s = S.settings;
  setSeg($("#theme"), s.theme); setSeg($("#set-mode"), s.mode);
  $("#out-root").textContent = S.info.output_root;
  $("#set-jobs").value = s.jobs;
  $("#about-sub").textContent = `版本 ${S.info.version}`;
  const l = S.info.libs;
  $("#about").innerHTML = [["测试机 CPU", S.info.cpu], ["逻辑核", S.info.cpus], ["JPEG-LS", "CharLS " + l.charls],
    ["HTJ2K", "OpenJPH " + l.openjph], ["JPEG XL", "libjxl " + l.libjxl], ["zstd", l.zstd], ["数据位置", S.info.data_dir]]
    .map(([k, v]) => `<div>${esc(k)}</div><div class="selectable" title="${esc(v)}">${esc(v)}</div>`).join("");
}
async function saveSettings(patch) {
  Object.assign(S.settings, patch);
  const r = await api.save_settings(patch);
  if (r.output_root) S.info.output_root = r.output_root;
  renderSettings();
}

/* ---------------------------------------------------------------- 启动 */
async function main() {
  injectIcons();
  await ready();
  S.info = await api.get_info();
  S.settings = S.info.settings;
  applyTheme(S.settings.theme);
  S.opts = { mode: S.settings.mode, ntfs: S.settings.ntfs, ceiling: S.settings.ceiling, cpuload: S.settings.cpuload };
  renderSettings(); renderFolder(); renderRun(); renderLive();
  const roi = S.settings.roi;
  $("#roi-daily").value = roi.daily_gb; $("#roi-debug").value = roi.debug_daily_gb; $("#roi-disk").value = roi.disk_tb;
  $("#roi-days").value = roi.retention_days; $("#roi-reserve").value = Math.round(roi.reserve * 100);

  $$(".nav").forEach((n) => (n.onclick = () => go(n.dataset.page)));
  document.addEventListener("keydown", (e) => {
    if (!e.ctrlKey) return;
    const k = { n: "test", h: "history", r: "roi", ",": "settings" }[e.key.toLowerCase()];
    if (k) { e.preventDefault(); go(k); }
  });
  $("#wb-min").onclick = () => api.win_minimize();
  $("#wb-max").onclick = () => api.win_toggle_max();
  $("#wb-close").onclick = () => api.win_close();
  $("#pick").onclick = pickFolder;
  $$("#mode button").forEach((b) => (b.onclick = () => { S.opts.mode = b.dataset.v; renderRun(); }));
  $$("#page-test .chk").forEach((c) => (c.onclick = () => { S.opts[c.dataset.opt] = !S.opts[c.dataset.opt]; renderRun(); }));
  $$("#theme button").forEach((b) => (b.onclick = () => { applyTheme(b.dataset.v); saveSettings({ theme: b.dataset.v }); }));
  $$("#set-mode button").forEach((b) => (b.onclick = () => saveSettings({ mode: b.dataset.v })));
  $("#set-jobs").onchange = () => saveSettings({ jobs: Math.max(1, +$("#set-jobs").value || 1) });
  $("#out-pick").onclick = async () => { const p = await api.pick_folder(S.info.output_root); if (p) { await saveSettings({ output_root: p }); loadHistory(); } };
  $("#hist-open-root").onclick = () => api.open_path(S.info.output_root);
  ["#roi-run", "#roi-daily", "#roi-debug", "#roi-disk", "#roi-days", "#roi-reserve"].forEach((id) => ($(id).oninput = calcRoi));
  $("#roi-apply").onclick = async () => {
    const r = await api.roi_apply($("#roi-run").value, roiParams());
    $("#roi-sub").textContent = r.ok ? "已写入该次测试的报告（第 9 节）" : r.error;
  };
  await loadHistory();
  const page = new URLSearchParams(location.search).get("page");
  if (page) go(page);
  const snap = await api.poll();
  if (snap) { S.snap = snap; if (snap.status === "running") poll(); else { renderRun(); renderLive(); renderFails(); } }
}
main();

/* ---------------------------------------------------------------- 演示数据（?mock=idle|running|done） */
function mockApi(mode) {
  const phases = (st) => [
    ["detect", "识别格式", "done", 281, 281, 3], ["layout", "拆分重组自检", "done", 281, 281, 8],
    ["compress", "压缩对比", st === "done" ? "done" : "run", st === "done" ? 281 : 158, 281, null], ["speed", "单核测速", st === "done" ? "done" : "wait", st === "done" ? 54 : 0, 54, null],
    ["ntfs", "NTFS 对照", st === "done" ? "done" : "wait", 0, null, null], ["ceiling", "天花板测试", st === "done" ? "done" : "wait", st === "done" ? 621 : 0, 621, null],
    ["bgpotential", "背景潜力评估", st === "done" ? "done" : "wait", 0, null, null], ["report", "生成报告", st === "done" ? "done" : "wait", 0, null, null],
  ].map(([key, label, status, done, total, secs]) => ({ key, label, status, done, total, secs, msg: status === "run" ? "frame_0157.bmp" : "" }));
  const live = { zstd3: 1.73, zstd19: 1.93, png: 2.1, jpegls: 2.55, htj2k: 2.29, jxl3: 2.52 };
  const fails = [{ name: "mono8_noconfig.raw", dir: "raw", tag: "需确认参数", reason: "没有 RAW 配置，猜测 Mono8 1280×1024，确认后写入 raw_config.yaml 再测" },
    { name: "truncated.bmp", dir: "other", tag: "已损坏", reason: "像素数据应到 1311798 字节，文件只有 787078 字节（截断）；按 zstd 或原样保存" },
    { name: "image.png 等 2 个", dir: "", tag: null, reason: "不是要处理的格式（JPEG、PNG 等），原样或 zstd 保存" }];
  const folder = { ok: true, path: "D:\\样本\\客户A\\工位3", files: 281, bytes: 290.6e6, raw_config: true, frames: 200, backgrounds: 8, inside_output: false };
  const snap = mode === "idle" ? null : {
    status: mode === "done" ? "done" : "running", out: "D:\\评估结果\\20261101-170512_工位3", sample: folder.path,
    elapsed: mode === "done" ? 1214 : 138, phases: phases(mode), overall: mode === "done" ? 100 : 41.2,
    current: mode === "done" ? null : { label: "压缩对比", done: 158, total: 281, msg: "frame_0157.bmp" },
    live: mode === "done" ? { ...live, ntfs: 1.17 } : live, verified: 152, mismatch: 0, failures: fails, total_bytes: 290.6e6,
    disk_bytes: 291.4e6,
    result: mode === "done" ? { label: "JPEG-LS（near=0）", ratio: 2.55, ntfs: 1.17, ntfs_stored: 290.6e6 / 1.17, files: 281, bytes: 290.6e6,
      orig: 290.6e6, stored: 290.6e6 / 2.55, ok: 264, mismatch: 0,
      parts: { coded: { files: 264, orig: 287.1e6, stored: 112.3e6 }, other: { files: 17, orig: 3.5e6, stored: 1.7e6 } },
      schemes: Object.entries(live).map(([id, ratio]) => ({ id, ratio, orig: 290.6e6, stored: 290.6e6 / ratio })) } : null, report: "x" };
  const hist = [{ path: "D:\\评估结果\\20261101-170512_工位3", name: "20261101-170512_工位3", sample: folder.path, started: "2026-11-01 17:05:12", elapsed: 1214, report: "x",
    brief: { label: "JPEG-LS（near=0）", ratio: 2.61, ntfs: 1.18, files: 281, bytes: 290.6e6, stored: 290.6e6 / 2.61, mismatch: 0 } },
    { path: "D:\\评估结果\\20261030-101200_外观线", name: "20261030-101200_外观线", sample: "D:\\样本\\客户B\\外观线", started: "2026-10-30 10:12:00", elapsed: 402, report: "x",
      brief: { label: "JPEG-LS（near=0）", ratio: 2.18, ntfs: 1.09, files: 96, bytes: 512e6, stored: 512e6 / 2.18, mismatch: 0 } }];
  const noop = async () => ({ ok: true });
  const st = { theme: new URLSearchParams(location.search).get("theme") || "system", jobs: 46, mode: "quick", ntfs: true, ceiling: true, cpuload: false,
    roi: { daily_gb: 200, debug_daily_gb: 600, disk_tb: 10, retention_days: 90, reserve: 0.1 } };
  if (mode !== "idle") setTimeout(() => { S.folder = folder; renderFolder(); renderRun(); }, 0);
  return {
    get_info: async () => ({ version: "0.1.0", cpu: "Intel(R) Xeon(R) Platinum 8160T CPU @ 2.10GHz", cpus: 96, settings: st, output_root: "D:\\评估结果",
      libs: { charls: "2.4.4", openjph: "0.32.0", libjxl: "0.12.0", zstd: "1.5.7" }, data_dir: "D:\\iis-bench\\data" }),
    save_settings: async () => ({ ok: true, output_root: "D:\\评估结果" }), pick_folder: async () => folder.path, inspect: async () => folder,
    start: noop, poll: async () => snap, stop: noop, open_path: noop, open_report: noop, history: async () => hist, delete_run: noop,
    roi: async () => ({ ok: true, label: "JPEG-LS（near=0）", ratio: 2.61, keep_days_before: 45, keep_days_after: 117.5, debug_fill_days_before: 15,
      debug_fill_days_after: 39.2, need_tb_before: 20, need_tb_after: 7.66, saved_tb: 12.34, meets_retention_after: true,
      text: ["", "", "", "现有硬盘：压缩前不满足招标保留天数，压缩后满足"] }),
    roi_apply: noop, win_minimize: noop, win_toggle_max: noop, win_close: noop,
  };
}
