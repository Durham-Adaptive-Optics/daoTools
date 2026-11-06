/* daoShmViewer, web version: quick record, latency, snapshots, tmux, DAQ (after app.js). */
"use strict";

(() => {
  const T = {
    tab: "lat", timer: 0, info: null,
    recs: [], recSel: null, recTimer: 0,
    lat: { sel: null, data: null },
    tmux: { sel: null, pane: "", text: null },
    daq: { config: null, state: null },
  };
  const DAQ_STORE = "daoShmViewer.daq";
  const WRITE_BUTTONS = ["latStart", "latStop", "snapCreate", "tmuxTerm", "daqGo", "daqRecover", "recSave"];

  /* ---------------------------------------------------------------- helpers */

  const writable = () => !!(S.hello && S.hello.write);
  function msg(id, text, ok) {
    const m = $(id);
    m.textContent = text || "";
    m.className = "msg " + (ok ? "ok" : "bad");
  }
  function needWrite(id) {
    if (writable()) return false;
    msg(id, "read-only server: start it with --write", false);
    return true;
  }
  function apiUrl(path, params) {
    const q = new URLSearchParams(location.search);
    q.delete("open"); q.delete("theme");
    for (const [k, v] of Object.entries(params)) q.set(k, v);
    return `${path}?${q.toString()}`;
  }
  function stamp() {
    const d = new Date(), p = n => String(n).padStart(2, "0");
    return `${d.getFullYear()}${p(d.getMonth() + 1)}${p(d.getDate())}_${p(d.getHours())}${p(d.getMinutes())}${p(d.getSeconds())}`;
  }
  function fmtBytes(b) {
    if (b >= 1 << 30) return (b / (1 << 30)).toFixed(2) + " GB";
    if (b >= 1 << 20) return (b / (1 << 20)).toFixed(1) + " MB";
    if (b >= 1 << 10) return (b / (1 << 10)).toFixed(1) + " kB";
    return b + " B";
  }
  function fmtDate(t) { return t ? new Date(t * 1000).toLocaleString() : "–"; }
  function canvas(cv) {
    const dpr = window.devicePixelRatio || 1, w = cv.clientWidth, h = cv.clientHeight;
    if (cv.width !== Math.round(w * dpr) || cv.height !== Math.round(h * dpr)) { cv.width = Math.round(w * dpr); cv.height = Math.round(h * dpr); }
    const ctx = cv.getContext("2d");
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);
    ctx.font = `10px ${cssVar("--mono")}`;
    return { ctx, w, h };
  }
  function shmOptions(sel, keep) {
    const cur = keep ? sel.value : "";
    const paths = Array.from(S.items.values()).filter(i => !i.error).map(i => i.path).sort();
    const html = `<option value="">—</option>` + paths.map(p => `<option value="${esc(p)}">${esc(base(p))}</option>`).join("");
    if (sel._html !== html) { sel.innerHTML = html; sel._html = html; sel.value = paths.includes(cur) ? cur : ""; }
  }
  function download(name, text) {
    const a = document.createElement("a");
    a.href = URL.createObjectURL(new Blob([text], { type: "text/yaml" }));
    a.download = name;
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 2000);
  }

  /* ----------------------------------------------------------------- drawer */

  function open(toggle) {
    const el = $("#tools");
    el.hidden = toggle ? !el.hidden : false;
    if (!el.hidden) $("#inspector").hidden = true;
    S.panels.forEach(p => p.dirty());
    setTab(T.tab);
  }
  function setTab(t) {
    T.tab = t;
    $$("#tools .tabs button").forEach(b => b.classList.toggle("on", b.dataset.ttab === t));
    ["lat", "snap", "tmux", "daq"].forEach(k => { $("#ttab-" + k).hidden = k !== t; });
    refresh();
  }

  async function refresh() {
    clearTimeout(T.timer);
    if ($("#tools").hidden || !S.connected) { T.timer = setTimeout(refresh, 1000); return; }
    try {
      if (T.tab === "lat") await refreshLatency();
      else if (T.tab === "snap") await refreshSnapshots();
      else if (T.tab === "tmux") await refreshTmux();
      else if (T.tab === "daq") await refreshDaq();
    } catch (e) { /* shown by each tab; keep polling */ }
    T.timer = setTimeout(refresh, T.tab === "lat" ? 500 : 1000);
  }

  /* ---------------------------------------------------------- quick record */

  function recPanel(p) {
    renderRecList();
    if (!T.recSel) $("#recFile").placeholder = `${(T.info && T.info.daodata) || "~"}/${base(p.path)}_${stamp()}.npy`;
  }

  async function recStart() {
    const p = S.focused;
    if (!p) return msg("#recMsg", "select a panel first", false);
    const n = parseInt($("#recN").value, 10) || 1;
    try {
      const r = await request("record_start", { path: p.path, n, timeout: Number($("#recTimeout").value) || 5 });
      T.recs.unshift({ id: r.id, path: p.path, n, state: "recording", done: 0 });
      selectRec(r.id);
      pollRec();
    } catch (e) { msg("#recMsg", e.message, false); }
  }

  async function pollRec() {
    clearTimeout(T.recTimer);
    const active = T.recs.filter(r => r.state === "recording");
    for (const r of active) {
      try {
        const st = (await request("record_status", { id: r.id })).rec;
        Object.assign(r, st);
      } catch (e) { r.state = "error"; r.error = e.message; }
    }
    renderRecList();
    showRec();
    if (T.recs.some(r => r.state === "recording")) T.recTimer = setTimeout(pollRec, 250);
  }

  function selectRec(id) {
    T.recSel = id;
    const r = T.recs.find(x => x.id === id);
    if (r) $("#recFile").value = `${(T.info && T.info.daodata) || "~"}/${base(r.path)}_${stamp()}_${r.n}frames.npy`;
    renderRecList();
    showRec();
  }

  function showRec() {
    const r = T.recs.find(x => x.id === T.recSel);
    $("#recBar").hidden = !r;
    $("#recDone").hidden = !(r && r.state === "done");
    $("#recCancel").hidden = !(r && r.state === "recording");
    $("#recStart").disabled = !!(r && r.state === "recording");
    $("#recSaveRow").hidden = !writable();
    if (!r) return;
    $("#recBar > div").style.width = `${100 * (r.done || 0) / r.n}%`;
    const secs = r.t1 && r.t0 ? r.t1 - r.t0 : (r.t0 ? Date.now() / 1000 - S.serverSkew - r.t0 : 0);
    if (r.state === "recording") msg("#recMsg", `${base(r.path)}: ${r.done || 0} / ${r.n} frames · ${secs.toFixed(1)} s (${r.mode || ""})`, true);
    else if (r.state === "done") {
      const rate = secs > 0 ? ` · ${fmtRate((r.n - 1) / secs)} Hz` : "";
      msg("#recMsg", `${base(r.path)}: ${r.n} frames in ${secs.toFixed(2)} s${rate} · ${fmtBytes(r.bytes || 0)}`
        + (r.gaps ? `\n${r.gaps} frames missed (gaps in cnt0): the SHM is written faster than the recorder copies` : " · no frame missed"), !r.gaps);
    } else msg("#recMsg", `${base(r.path)}: ${r.error || r.state}`, false);
  }

  function renderRecList() {
    const el = $("#recList");
    if (!T.recs.length) { el.textContent = "none yet"; el.className = "muted"; return; }
    el.className = "items";
    el.innerHTML = T.recs.map(r => `<div class="item${r.id === T.recSel ? " sel" : ""}" data-rec="${r.id}">
      <span class="nm">${esc(base(r.path))}</span><span class="meta">${r.n} frames · ${esc(r.state)}${r.state === "recording" ? ` ${r.done || 0}` : ""}${r.gaps ? ` · ${r.gaps} missed` : ""}</span></div>`).join("");
  }

  /* ---------------------------------------------------------------- latency */

  async function refreshLatency() {
    const r = await request("latency_list", {});
    const list = r.sessions;
    const el = $("#latList");
    if (!list.length) { el.className = "muted"; el.textContent = "none"; }
    else {
      el.className = "items";
      el.innerHTML = list.map(s => `<div class="item${s.name === T.lat.sel ? " sel" : ""}" data-lat="${esc(s.name)}">
        <span class="nm">${esc(s.name.replace(/^daoLatency_/, ""))}</span>
        <span class="meta">${s.ready ? `${fmt(s.avg)} ± ${fmt(s.rms)} µs · n ${s.n}/${s.pop}` : "starting…"}</span></div>`).join("");
    }
    if (T.lat.sel && !list.some(s => s.name === T.lat.sel)) T.lat.sel = null;
    $("#latView").hidden = !T.lat.sel;
    if (!T.lat.sel) return;
    const s = list.find(x => x.name === T.lat.sel);
    $("#latCmd").textContent = s.cmd || "";
    const d = await request("latency_data", { name: T.lat.sel });
    T.lat.data = d;
    if (!d.ready) { $("#latStats").textContent = "waiting for daoTimeDiff's SHMs…"; return; }
    $("#latStats").textContent = `AVG ${fmt(d.avg)} µs   RMS ${fmt(d.rms)} µs   n ${d.n}/${d.pop}`;
    drawLatency(d);
  }

  function drawLatency(d) {
    const v = d.values || [], acc = cssVar("--accent"), ok = cssVar("--ok"), muted = cssVar("--muted"), grid = cssVar("--grid");
    // scatter of the window, AVG and ±RMS
    {
      const { ctx, w, h } = canvas($("#latScatter"));
      const ml = 44, mb = 14, mt = 6, pw = w - ml - 6, ph = h - mt - mb;
      if (v.length && pw > 10) {
        let lo = Math.min(...v, d.avg - d.rms), hi = Math.max(...v, d.avg + d.rms);
        if (hi <= lo) { hi = lo + 1; }
        const pad = (hi - lo) * 0.08; lo -= pad; hi += pad;
        const X = i => ml + (v.length > 1 ? i / (v.length - 1) : 0.5) * pw, Y = y => mt + ph - (y - lo) / (hi - lo) * ph;
        ctx.strokeStyle = grid; ctx.fillStyle = muted; ctx.textAlign = "right"; ctx.textBaseline = "middle";
        for (const t of niceTicks(lo, hi, 4)) { const y = Math.round(Y(t)) + 0.5; ctx.beginPath(); ctx.moveTo(ml, y); ctx.lineTo(ml + pw, y); ctx.stroke(); ctx.fillText(fmt(t), ml - 4, y); }
        ctx.textAlign = "center"; ctx.textBaseline = "top"; ctx.fillText("µs, last " + v.length, ml + pw / 2, mt + ph + 2);
        ctx.fillStyle = acc;
        v.forEach((y, i) => ctx.fillRect(X(i) - 1.5, Y(y) - 1.5, 3, 3));
        ctx.strokeStyle = ok; ctx.lineWidth = 1;
        ctx.beginPath(); ctx.moveTo(ml, Y(d.avg)); ctx.lineTo(ml + pw, Y(d.avg)); ctx.stroke();
        ctx.setLineDash([4, 3]);
        for (const y of [d.avg - d.rms, d.avg + d.rms]) { ctx.beginPath(); ctx.moveTo(ml, Y(y)); ctx.lineTo(ml + pw, Y(y)); ctx.stroke(); }
        ctx.setLineDash([]);
      }
    }
    // histogram, as daoShmViewer's: bins = clip(n / 2, 5, 40)
    {
      const { ctx, w, h } = canvas($("#latHist"));
      const ml = 30, mb = 14, mt = 6, pw = w - ml - 6, ph = h - mt - mb;
      if (v.length && pw > 10) {
        const nb = clamp(Math.floor(v.length / 2), 5, 40);
        let lo = Math.min(...v), hi = Math.max(...v);
        if (hi <= lo) hi = lo + 1;
        const c = new Array(nb).fill(0);
        for (const y of v) c[Math.min(nb - 1, Math.floor((y - lo) / (hi - lo) * nb))]++;
        const mx = Math.max(...c);
        ctx.fillStyle = acc;
        c.forEach((n, i) => { const bh = n / mx * ph; ctx.fillRect(ml + i * pw / nb, mt + ph - bh, Math.max(1, pw / nb - 1), bh); });
        ctx.fillStyle = muted; ctx.textBaseline = "top";
        ctx.textAlign = "left"; ctx.fillText(fmt(lo), ml, mt + ph + 2);
        ctx.textAlign = "right"; ctx.fillText(fmt(hi), ml + pw, mt + ph + 2);
        ctx.textBaseline = "middle"; ctx.fillText(String(mx), ml - 4, mt + 4);
      }
    }
  }

  async function latStart() {
    if (needWrite("#latMsg")) return;
    const a = $("#latA").value, b = $("#latB").value;
    if (!a || !b) return msg("#latMsg", "pick the start and end SHMs", false);
    try {
      const r = await request("latency_start", { a, b, n: parseInt($("#latN").value, 10) || 100 });
      T.lat.sel = r.name;
      msg("#latMsg", `started ${r.name}`, true);
      refresh();
    } catch (e) { msg("#latMsg", e.message, false); }
  }

  /* -------------------------------------------------------------- snapshots */

  async function refreshSnapshots() {
    const r = await request("snapshot_list", {});
    $("#snapDir").textContent = r.dir;
    const el = $("#snapList");
    if (!r.files.length) { el.className = "muted"; el.textContent = "none"; return; }
    el.className = "items";
    const html = r.files.map(f => `<div class="item" data-snap="${esc(f.name)}">
      <span class="nm">${esc(f.name)}</span><span class="meta">${fmtBytes(f.bytes)} · ${esc(fmtDate(f.mtime))}</span>
      <span class="spacer"></span>
      <button data-snapdl="${esc(f.name)}" title="Download">⤓</button>
      <button data-snapload="${esc(f.name)}"${writable() ? "" : " class=\"ro\" title=\"needs --write\""}>Load</button></div>`).join("");
    if (el._html !== html) { el.innerHTML = html; el._html = html; }
  }

  async function snapRun(what, name, btn) {
    if (needWrite("#snapMsg")) return;
    if (what === "load" && !confirm(`Load the snapshot ${name}?\n\nIt recreates every SHM saved in it (overwriting the current ones, with the running loop's readers and writers on them) and starts the tmux sessions' commands again.`)) return;
    btn.disabled = true;
    msg("#snapMsg", `daoSnapshot.py ${what} ${name}…`, true);
    try {
      const r = await request(what === "create" ? "snapshot_create" : "snapshot_load", { name });
      msg("#snapMsg", `${what === "create" ? "created" : "loaded"} ${r.path}`, true);
      $("#snapOut").hidden = !r.output;
      $("#snapOut").textContent = r.output || "";
      refresh();
    } catch (e) {
      msg("#snapMsg", e.message, false);
    } finally { btn.disabled = false; }
  }

  /* ------------------------------------------------------------------- tmux */

  async function refreshTmux() {
    const r = await request("tmux_list", {});
    const list = r.sessions;
    $("#tmuxCount").textContent = list.length ? `(${list.length})` : "";
    const el = $("#tmuxList");
    if (!list.length) { el.className = "muted"; el.textContent = "no tmux server running"; }
    else {
      el.className = "items";
      const html = list.map(s => `<div class="item${s.name === T.tmux.sel ? " sel" : ""}" data-tmux="${esc(s.name)}">
        <span class="nm">${esc(s.name)}</span><span class="meta">${s.windows} window${s.windows > 1 ? "s" : ""}${s.attached ? " · attached" : ""} · since ${esc(fmtDate(s.created))}</span></div>`).join("");
      if (el._html !== html) { el.innerHTML = html; el._html = html; }
    }
    if (T.tmux.sel && !list.some(s => s.name === T.tmux.sel)) T.tmux.sel = null;
    $("#tmuxView").hidden = !T.tmux.sel;
    if (!T.tmux.sel) return;
    const c = await request("tmux_capture", { name: T.tmux.sel, pane: T.tmux.pane, lines: 400 });
    T.tmux.pane = c.pane;
    $("#tmuxName").textContent = c.name;
    const sel = $("#tmuxPane");
    const opts = c.panes.map(p => `<option value="${esc(p.id)}">${esc(p.id)} ${esc(p.window)} · ${esc(p.cmd)}</option>`).join("");
    if (sel._html !== opts) { sel.innerHTML = opts; sel._html = opts; }
    sel.value = c.pane;
    const out = $("#tmuxOut");
    if (T.tmux.text !== c.text) {
      const atEnd = out.scrollTop + out.clientHeight >= out.scrollHeight - 4;
      out.textContent = c.text;
      T.tmux.text = c.text;
      if ($("#tmuxFollow").checked || atEnd) out.scrollTop = out.scrollHeight;
    }
    $("#tmuxAttach").textContent = `ssh -t ${S.hello ? S.hello.host : "<host>"} tmux attach -t ${c.name}`;
    $("#tmuxTerm").hidden = !(T.info && T.info.display);
  }

  /* -------------------------------------------------------------------- DAQ */

  const SMEM_NUM = [["samples", "Sample limit"], ["file_rollover", "File rollover"], ["daq_affinity", "DAQ affinity"],
    ["sink_affinity", "Sink affinity"], ["buffer_limit", "Buffer limit"]];

  function saveDaq() {
    try { localStorage.setItem(DAQ_STORE, JSON.stringify(T.daq.config)); } catch (e) { /* not kept */ }
  }
  function newSmem(path) { return { uri: "smem://" + path, format: "fits", metadata_only: false, eager_start: true }; }
  function addSource(src) {
    const c = T.daq.config;
    if (c.sources.some(s => s.uri === src.uri)) return false;
    c.sources.push(src);
    return true;
  }

  function renderDaq() {
    const c = T.daq.config;
    $("#daqRoot").value = c.root_storage || "";
    const el = $("#daqSources");
    if (!c.sources.length) { el.className = "muted"; el.textContent = "none: add SHMs or files below"; saveDaq(); return; }
    el.className = "";
    el.innerHTML = c.sources.map((s, i) => {
      const smem = s.uri.startsWith("smem://");
      const params = smem ? `<div class="params">
          <span>Export as</span><select data-k="format" data-i="${i}"><option>fits</option></select>
          ${SMEM_NUM.map(([k, label]) => `<span>${label}</span><input type="number" data-k="${k}" data-i="${i}" placeholder="unset" value="${s[k] ?? ""}">`).join("")}
          <span></span><span><label class="chk"><input type="checkbox" data-k="metadata_only" data-i="${i}"${s.metadata_only ? " checked" : ""}>metadata only</label>
          <label class="chk"><input type="checkbox" data-k="eager_start" data-i="${i}"${s.eager_start !== false ? " checked" : ""}>eager start</label></span>
        </div>` : `<div class="params"><span>File</span><span class="mono">${esc(s.uri.slice(7))}</span></div>`;
      return `<details class="srcrow"><summary><span class="nm" title="${esc(s.uri)}">${esc(smem ? base(s.uri.slice(7)) : s.uri)}</span>
        <span class="muted">${smem ? "SHM" : "file"}</span><button class="icon" data-del="${i}" title="Remove">✕</button></summary>${params}</details>`;
    }).join("");
    saveDaq();
  }

  function onDaqParam(e) {
    const t = e.target, i = +t.dataset.i, k = t.dataset.k;
    if (!k || isNaN(i)) return;
    const s = T.daq.config.sources[i];
    if (t.type === "checkbox") s[k] = t.checked;
    else if (t.type === "number") { if (t.value === "" || +t.value < 0) delete s[k]; else s[k] = parseInt(t.value, 10); }
    else s[k] = t.value;
    saveDaq();
  }

  async function refreshDaq() {
    const r = await request("daq_state", {});
    T.daq.state = r.state;
    const pill = $("#daqState");
    pill.textContent = r.state || "unreachable";
    pill.title = r.error || "";
    pill.className = "pill " + (r.state === "Acquiring" ? "on" : r.state === "Error" || !r.state ? "off" : "");
    const go = $("#daqGo");
    go.textContent = r.state === "Acquiring" ? "Finish DAQ" : "Start DAQ";
    go.disabled = false;                 // a click says why when it cannot (read-only, DAQ unreachable)
    go.classList.toggle("ro", !writable() || !r.state);
    $("#daqRecover").hidden = r.state !== "Error";
  }

  async function daqGo() {
    if (needWrite("#daqMsg")) return;
    if (!T.daq.state) return msg("#daqMsg", "the DAQ server does not answer (is daoDAQ running?): " + ($("#daqState").title || "no state"), false);
    try {
      if (T.daq.state === "Acquiring") {
        msg("#daqMsg", (await request("daq_finish", {})).info, true);
      } else {
        const c = T.daq.config;
        if (!c.sources.length) return msg("#daqMsg", "no source", false);
        msg("#daqMsg", (await request("daq_start", { config: c })).info, true);
      }
    } catch (e) { msg("#daqMsg", e.message, false); }
    refresh();
  }

  /* ----------------------------------------------------------------- wiring */

  function bind() {
    $("#btnTools").onclick = () => open(true);
    $("#toolsClose").onclick = () => { $("#tools").hidden = true; S.panels.forEach(p => p.dirty()); };
    $$("#tools .tabs button").forEach(b => b.onclick = () => setTab(b.dataset.ttab));

    // record
    $("#recStart").onclick = recStart;
    $("#recCancel").onclick = () => T.recSel && request("record_cancel", { id: T.recSel }).catch(() => {});
    $("#recList").onclick = e => { const it = e.target.closest("[data-rec]"); if (it) selectRec(it.dataset.rec); };
    $$("button[data-rdl]").forEach(b => b.onclick = () => {
      if (T.recSel) location.href = apiUrl("/api/record", { id: T.recSel, fmt: b.dataset.rdl });
    });
    $("#recSave").onclick = async () => {
      if (needWrite("#recMsg") || !T.recSel) return;
      try { msg("#recMsg", (await request("record_save", { id: T.recSel, file: $("#recFile").value || $("#recFile").placeholder })).info, true); }
      catch (e) { msg("#recMsg", e.message, false); }
    };

    // latency
    $("#latStart").onclick = latStart;
    $$("[data-latpick]").forEach(b => b.onclick = () => { if (S.focused) $("#" + b.dataset.latpick).value = S.focused.path; });
    $("#latList").onclick = e => { const it = e.target.closest("[data-lat]"); if (it) { T.lat.sel = it.dataset.lat; refresh(); } };
    $("#latStop").onclick = async () => {
      if (needWrite("#latMsg") || !T.lat.sel) return;
      try { msg("#latMsg", (await request("latency_stop", { name: T.lat.sel })).info, true); T.lat.sel = null; refresh(); }
      catch (e) { msg("#latMsg", e.message, false); }
    };
    $("#latShow").onclick = () => { if (T.lat.data && T.lat.data.array) openPath(T.lat.data.array, true); };

    // snapshots
    $("#snapCreate").onclick = e => snapRun("create", $("#snapName").value.trim() || `snapshot_${stamp()}`, e.target);
    $("#snapList").onclick = e => {
      const dl = e.target.closest("[data-snapdl]"), ld = e.target.closest("[data-snapload]");
      if (dl) location.href = apiUrl("/api/snapshot", { name: dl.dataset.snapdl });
      if (ld) snapRun("load", ld.dataset.snapload, ld);
    };

    // tmux
    $("#tmuxList").onclick = e => {
      const it = e.target.closest("[data-tmux]");
      if (it) { T.tmux.sel = it.dataset.tmux; T.tmux.pane = ""; T.tmux.text = null; refresh(); }
    };
    $("#tmuxPane").onchange = e => { T.tmux.pane = e.target.value; T.tmux.text = null; refresh(); };
    $("#tmuxCopy").onclick = async () => {
      const text = $("#tmuxAttach").textContent;
      try { await navigator.clipboard.writeText(text); toast("copied", "ok"); }
      catch (e) { const r = document.createRange(); r.selectNodeContents($("#tmuxAttach")); getSelection().removeAllRanges(); getSelection().addRange(r); toast("selected: Ctrl-C to copy"); }
    };
    $("#tmuxTerm").onclick = async () => {
      if (!T.tmux.sel) return;
      if (!writable()) return toast("read-only server: start it with --write", "bad");
      try { toast((await request("tmux_terminal", { name: T.tmux.sel })).info, "ok"); } catch (e) { toast(e.message, "bad"); }
    };

    // DAQ
    $("#daqRoot").oninput = e => { T.daq.config.root_storage = e.target.value; saveDaq(); };
    $("#daqSources").addEventListener("change", onDaqParam);
    $("#daqSources").addEventListener("click", e => {
      const d = e.target.closest("[data-del]");
      if (d) { e.preventDefault(); T.daq.config.sources.splice(+d.dataset.del, 1); renderDaq(); }
    });
    $("#daqAddShmBtn").onclick = () => { const p = $("#daqAddShm").value; if (p && addSource(newSmem(p))) renderDaq(); };
    $("#daqAddPanels").onclick = () => { let n = 0; S.panels.forEach(p => { n += addSource(newSmem(p.path)) ? 1 : 0; }); renderDaq(); msg("#daqMsg", `${n} added`, true); };
    $("#daqAddFile").onclick = () => {
      const f = $("#daqFile").value.trim();
      if (!f.startsWith("/")) return msg("#daqMsg", "an absolute path on the server", false);
      if (addSource({ uri: "file://" + f })) renderDaq();
      $("#daqFile").value = "";
    };
    $("#daqClear").onclick = () => { if (confirm("Remove every source?")) { T.daq.config.sources = []; renderDaq(); } };
    $("#daqExport").onclick = async () => {
      try { download("daq.yaml", (await request("daq_yaml", { config: T.daq.config })).text); }
      catch (e) { msg("#daqMsg", e.message, false); }
    };
    $("#daqImport").onchange = async e => {
      const f = e.target.files[0];
      e.target.value = "";
      if (!f) return;
      try {
        const c = (await request("daq_parse", { text: await f.text() })).config;
        T.daq.config = { root_storage: c.root_storage || "", sources: c.sources || [] };
        renderDaq();
        msg("#daqMsg", `imported ${f.name}: ${T.daq.config.sources.length} sources`, true);
      } catch (err) { msg("#daqMsg", err.message, false); }
    };
    $("#daqGo").onclick = daqGo;
    $("#daqRecover").onclick = async () => {
      if (needWrite("#daqMsg")) return;
      try { msg("#daqMsg", (await request("daq_recover", {})).info, true); } catch (e) { msg("#daqMsg", e.message, false); }
      refresh();
    };
  }

  window.Tools = {
    onHello(m) {
      T.info = m.tools || {};
      $("#snapDir").textContent = T.info.snapdir || "";
      if (!T.daq.config) {
        try { T.daq.config = JSON.parse(localStorage.getItem(DAQ_STORE)); } catch (e) { T.daq.config = null; }
        if (!T.daq.config || !Array.isArray(T.daq.config.sources)) T.daq.config = { root_storage: T.info.daodata || "", sources: [] };
      }
      for (const id of WRITE_BUTTONS) {
        const b = $("#" + id);
        b.disabled = false;              // clickable: a click says why it cannot run
        b.classList.toggle("ro", !m.write);
        b.title = m.write ? b.title.replace(/ \(needs --write\)$/, "") : (b.title.replace(/ \(needs --write\)$/, "") + " (needs --write)");
      }
      if (!m.write) {
        ["#latMsg", "#snapMsg", "#daqMsg"].forEach(id => {
          if (!$(id).textContent) msg(id, "read-only server: starting, stopping and loading need daoShmWebViewer.py --write", false);
        });
      }
      const missing = [];
      if (!T.info.daoTimeDiff) missing.push("daoTimeDiff");
      if (!T.info.tmux) missing.push("tmux");
      if (missing.length) msg("#latMsg", `not on the server: ${missing.join(", ")}`, false);
      if (!T.info.daoSnapshot) msg("#snapMsg", "daoSnapshot.py is not in the server's PATH", false);
      if (!T.info.yaml) msg("#daqMsg", "PyYAML missing on the server: no import / export / start", false);
      renderDaq();
      showRec();
    },
    onList() {
      shmOptions($("#latA"), true);
      shmOptions($("#latB"), true);
      shmOptions($("#daqAddShm"), true);
    },
    open,
    recPanel,
  };

  bind();
  refresh();
})();
