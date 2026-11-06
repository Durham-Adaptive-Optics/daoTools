/* daoShmViewer, web version: the page. Served by daoShmWebViewer.py; no external library. */
"use strict";

const $ = (sel, el = document) => el.querySelector(sel);
const $$ = (sel, el = document) => Array.from(el.querySelectorAll(sel));
const STORE = "daoShmViewer.v1";
const TYPED = {
  uint8: Uint8Array, int8: Int8Array, uint16: Uint16Array, int16: Int16Array,
  uint32: Uint32Array, int32: Int32Array, float32: Float32Array, float64: Float64Array,
};

/* ------------------------------------------------------------------ state */

const S = {
  ws: null, hello: null, connected: false, req: 0, pending: new Map(),
  items: new Map(), order: [], cursor: null,
  panels: [], focused: null, nextId: 1,
  sort: { key: "name", dir: 1 }, filter: "",
  prefs: { theme: null, layout: "auto", side: 330 },
  serverSkew: 0,
};

function loadPrefs() {
  try {
    const p = JSON.parse(localStorage.getItem(STORE) || "{}");
    Object.assign(S.prefs, p.prefs || {});
    if (p.sort) S.sort = p.sort;
    return p.panels || [];
  } catch (e) { return []; }
}

let saveTimer = 0;
function save() {
  clearTimeout(saveTimer);
  saveTimer = setTimeout(() => {
    try {
      localStorage.setItem(STORE, JSON.stringify({
        prefs: S.prefs, sort: S.sort,
        panels: S.panels.map(p => ({ path: p.path, s: p.s })),
      }));
    } catch (e) { /* private window: nothing kept */ }
  }, 300);
}

/* ---------------------------------------------------------------- helpers */

function fmt(v, intLike) {
  if (v === null || v === undefined) return "–";
  if (typeof v !== "number") return String(v);
  if (!isFinite(v)) return String(v);
  if (intLike || (Number.isInteger(v) && Math.abs(v) < 1e15)) return String(v);
  const a = Math.abs(v);
  if (a !== 0 && (a >= 1e6 || a < 1e-3)) return v.toExponential(3);
  return Number(v.toPrecision(5)).toString();
}
function fmtDur(s) {
  if (s === null || s === undefined || !isFinite(s)) return "–";
  const a = Math.abs(s);
  if (a < 1e-3) return (s * 1e6).toFixed(0) + " µs";
  if (a < 1) return (s * 1e3).toFixed(1) + " ms";
  if (a < 120) return s.toFixed(1) + " s";
  if (a < 7200) return (s / 60).toFixed(1) + " min";
  if (a < 172800) return (s / 3600).toFixed(1) + " h";
  return (s / 86400).toFixed(1) + " d";
}
function fmtRate(r) {
  if (!r) return "0";
  if (r >= 1000) return (r / 1000).toFixed(r >= 1e4 ? 0 : 1) + "k";
  if (r >= 100) return r.toFixed(0);
  return r.toFixed(1);
}
function base(path) { return path.replace(/^.*\//, "").replace(/\.im\.shm$/, ""); }
function dirOf(path) { return path.replace(/\/[^/]*$/, ""); }
function shapeStr(sh) { return sh ? sh.join("×") : ""; }
function intDtype(dt) { return /int/.test(dt || ""); }
function esc(s) { return String(s).replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c])); }
function clamp(x, a, b) { return x < a ? a : x > b ? b : x; }

let toastTimer = 0;
function toast(text, kind) {
  const t = $("#toast");
  t.textContent = text;
  t.className = kind || "";
  t.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { t.hidden = true; }, kind === "bad" ? 6000 : 2500);
}

function niceTicks(lo, hi, n) {
  if (!(hi > lo)) return [lo];
  const raw = (hi - lo) / Math.max(1, n);
  const mag = Math.pow(10, Math.floor(Math.log10(raw)));
  const r = raw / mag;
  const step = (r < 1.5 ? 1 : r < 3 ? 2 : r < 7 ? 5 : 10) * mag;
  const out = [];
  for (let v = Math.ceil(lo / step) * step; v <= hi + step * 1e-9; v += step) out.push(Math.abs(v) < step * 1e-9 ? 0 : v);
  return out;
}

function cssVar(name) { return getComputedStyle(document.documentElement).getPropertyValue(name).trim(); }

/* ------------------------------------------------------------- colour maps */

function poly7(c, t) {
  // c: 7 rows of [r, g, b]; c0 + t (c1 + t (c2 + ...))
  const out = [0, 0, 0];
  for (let k = 0; k < 3; k++) {
    let v = c[6][k];
    for (let i = 5; i >= 0; i--) v = c[i][k] + t * v;
    out[k] = v;
  }
  return out;
}
const POLY = {
  viridis: [[0.2777273272234177, 0.005407344544966578, 0.3340998053353061], [0.1050930431085774, 1.404613529898575, 1.384590162594685],
    [-0.3308618287255563, 0.214847559468213, 0.09509516302823659], [-4.634230498983486, -5.799100973351585, -19.33244095627987],
    [6.228269936347081, 14.17993336680509, 56.69055260068105], [4.776384997670288, -13.74514537774601, -65.35303263337234],
    [-5.435455855934631, 4.645852612178535, 26.3124352495832]],
  plasma: [[0.05873234392399702, 0.02333670892565664, 0.5433401826748754], [2.176514634195958, 0.2383834171260182, 0.7539604599784036],
    [-2.689460476458034, -7.455851135738909, 3.110799939717086], [6.130348345893603, 42.3461881477227, -28.51885465332158],
    [-11.10743619062271, -82.66631109428045, 60.13984767418263], [10.02306557647065, 71.41361770095349, -54.07218655560067],
    [-3.658713842777788, -22.93153465461149, 18.19190778539828]],
  magma: [[-0.002136485053939582, -0.000749655052795221, -0.005386127855323933], [0.2516605407371642, 0.6775232436837668, 2.494026599312351],
    [8.353717279216625, -3.577719514958484, 0.3144679030132573], [-27.66873308576866, 14.26473078096533, -13.64921318813922],
    [52.17613981234068, -27.94360607168351, 12.94416944238394], [-50.76852536473588, 29.04658282127291, 4.23415299384598],
    [18.65570506591883, -11.48977351997711, -5.601961508734096]],
  inferno: [[0.0002189403691192265, 0.001651004631001012, -0.01948089843709184], [0.1065134194856116, 0.5639564367884091, 3.932712388889277],
    [11.60249308247187, -3.972853965665698, -15.9423941062914], [-41.70399613139459, 17.43639888205313, 44.35414519872813],
    [77.162935699427, -33.40235894210092, -81.80730925738993], [-71.31942824499214, 32.62606426397723, 73.20951985803202],
    [25.13112622477341, -12.24266895238567, -23.07032500287172]],
};
function turbo(t) {
  const t2 = t * t, t3 = t2 * t, t4 = t3 * t, t5 = t4 * t;
  return [
    0.13572138 + 4.61539260 * t - 42.66032258 * t2 + 132.13108234 * t3 - 152.94239396 * t4 + 59.28637943 * t5,
    0.09140261 + 2.19418839 * t + 4.84296658 * t2 - 14.18503333 * t3 + 4.27729857 * t4 + 2.82956604 * t5,
    0.10667330 + 12.64194608 * t - 60.58204836 * t2 + 110.36276771 * t3 - 89.90310912 * t4 + 27.34824973 * t5,
  ];
}
function lerp3(stops, t) {
  const n = stops.length - 1, x = t * n, i = Math.min(n - 1, Math.floor(x)), f = x - i;
  return [0, 1, 2].map(k => stops[i][k] + (stops[i + 1][k] - stops[i][k]) * f);
}
const CMAPS = {
  gray: t => [t, t, t],
  viridis: t => poly7(POLY.viridis, t),
  inferno: t => poly7(POLY.inferno, t),
  magma: t => poly7(POLY.magma, t),
  plasma: t => poly7(POLY.plasma, t),
  turbo,
  hot: t => [clamp(3 * t, 0, 1), clamp(3 * t - 1, 0, 1), clamp(3 * t - 2, 0, 1)],
  coolwarm: t => lerp3([[0.230, 0.299, 0.754], [0.552, 0.690, 0.996], [0.865, 0.865, 0.865], [0.958, 0.604, 0.482], [0.706, 0.016, 0.150]], t),
  seismic: t => lerp3([[0, 0, 0.3], [0, 0, 1], [1, 1, 1], [1, 0, 0], [0.5, 0, 0]], t),
};
const lutCache = new Map();
function lut(name, rev) {
  const key = name + (rev ? "-r" : "");
  if (lutCache.has(key)) return lutCache.get(key);
  const f = CMAPS[name] || CMAPS.gray, out = new Uint32Array(256);
  for (let i = 0; i < 256; i++) {
    const t = rev ? 1 - i / 255 : i / 255;
    const [r, g, b] = f(t).map(v => Math.round(clamp(v, 0, 1) * 255));
    out[i] = (255 << 24) | (b << 16) | (g << 8) | r;
  }
  lutCache.set(key, out);
  return out;
}
const scaleCache = new Map();
function scaleLut(kind) {
  // 4096 normalized steps -> colour index, for the non-linear scales
  if (scaleCache.has(kind)) return scaleCache.get(kind);
  const out = new Uint8Array(4096);
  for (let i = 0; i < 4096; i++) {
    const t = i / 4095;
    let u = t;
    if (kind === "sqrt") u = Math.sqrt(t);
    else if (kind === "log") u = Math.log10(1 + 1000 * t) / 3;
    else if (kind === "asinh") u = Math.asinh(10 * t) / Math.asinh(10);
    out[i] = Math.round(clamp(u, 0, 1) * 255);
  }
  scaleCache.set(kind, out);
  return out;
}
function scaleFn(kind) {
  if (kind === "sqrt") return t => Math.sqrt(t);
  if (kind === "log") return t => Math.log10(1 + 1000 * t) / 3;
  if (kind === "asinh") return t => Math.asinh(10 * t) / Math.asinh(10);
  return t => t;
}
const NAN_COLOR = (255 << 24) | (200 << 16) | (0 << 8) | 200;

/* ------------------------------------------------------------- connection */

function connect() {
  const proto = location.protocol === "https:" ? "wss" : "ws";
  const ws = new WebSocket(`${proto}://${location.host}/ws${location.search}`);
  ws.binaryType = "arraybuffer";
  S.ws = ws;
  setConn(false, "connecting…");
  ws.onopen = () => {
    S.connected = true;
    setConn(true);
    for (const p of S.panels) p.subscribe(true);
  };
  ws.onclose = () => {
    S.connected = false;
    setConn(false, "disconnected: retrying");
    for (const [, r] of S.pending) r.reject(new Error("disconnected"));
    S.pending.clear();
    setTimeout(connect, 1500);
  };
  ws.onmessage = ev => {
    if (typeof ev.data === "string") onText(JSON.parse(ev.data));
    else onFrame(ev.data);
  };
}

function setConn(on, text) {
  const c = $("#conn");
  c.className = "pill " + (on ? "on" : "off");
  c.textContent = on ? "live" : text;
}

function send(obj) {
  if (S.ws && S.ws.readyState === 1) S.ws.send(JSON.stringify(obj));
}

function request(op, args) {
  return new Promise((resolve, reject) => {
    if (!S.connected) return reject(new Error("not connected"));
    const req = ++S.req;
    S.pending.set(req, { resolve, reject });
    send(Object.assign({ op, req }, args));
  });
}

function onText(m) {
  switch (m.type) {
    case "hello": {
      S.hello = m;
      $("#hostinfo").textContent = `${m.host} · ${m.dirs.join(", ")}`;
      const rw = $("#conn");
      if (m.write) { rw.classList.add("rw"); rw.textContent = "live · write"; }
      $("#btnCreate").hidden = !m.write;
      $("#writeTools").hidden = !m.write;
      $("#readOnlyNote").hidden = !!m.write;
      $("#cDir").innerHTML = m.dirs.map(d => `<option>${esc(d)}</option>`).join("");
      if (window.Tools) Tools.onHello(m);
      break;
    }
    case "list":
      S.serverSkew = Date.now() / 1000 - m.now;
      S.items = new Map(m.items.map(i => [i.path, i]));
      if (S.openArg) {
        // ?open=name1,name2 (names or full paths): those panels instead of the saved ones
        const want = S.openArg;
        S.openArg = null;
        S.panels.slice().forEach(removePanel);
        for (const w of want) {
          const it = m.items.find(i => i.path === w || base(i.path) === w);
          if (it) addPanel(it.path);
          else toast(`no SHM ${w}`, "bad");
        }
      }
      renderList();
      for (const p of S.panels) p.onListUpdate();
      if (window.Tools) Tools.onList();
      break;
    case "reply": {
      const r = S.pending.get(m.req);
      if (!r) break;
      S.pending.delete(m.req);
      if (m.ok) r.resolve(m); else r.reject(new Error(m.error || "error"));
      break;
    }
    case "gone":
    case "unreadable":
    case "error": {
      const p = S.panels.find(q => q.id === m.id);
      if (p) p.onNotice(m);
      break;
    }
  }
}

const decoder = new TextDecoder();
function onFrame(buf) {
  const dv = new DataView(buf);
  const hl = dv.getUint32(0, true);
  const h = JSON.parse(decoder.decode(new Uint8Array(buf, 4, hl)));
  const T = TYPED[h.dtype];
  if (!T) return;
  const n = h.view.reduce((a, b) => a * b, 1);
  const data = new T(buf, 4 + hl, n);
  const p = S.panels.find(q => q.id === h.id);
  if (p) p.onFrame(h, data);
}

/* ---------------------------------------------------------------- the list */

function sortedItems() {
  const terms = S.filter.toLowerCase().split(/\s+/).filter(Boolean);
  let items = Array.from(S.items.values());
  if (terms.length) items = items.filter(i => terms.every(t => i.path.toLowerCase().includes(t)));
  const k = S.sort.key, d = S.sort.dir;
  const val = i => k === "rate" ? (i.rate || 0) : k === "shape" ? (i.shape ? i.shape.reduce((a, b) => a * b, 1) : -1)
    : k === "dtype" ? (i.dtype || "") : base(i.path).toLowerCase();
  items.sort((a, b) => { const x = val(a), y = val(b); return (x < y ? -1 : x > y ? 1 : 0) * d || (a.path < b.path ? -1 : 1); });
  return items;
}

function renderList() {
  const items = sortedItems();
  S.order = items.map(i => i.path);
  const shown = new Set(S.panels.map(p => p.path));
  const focused = S.focused && S.focused.path;
  const multiDir = S.hello && S.hello.dirs.length > 1;
  const html = items.map(i => {
    const cls = ["row"];
    if (i.error) cls.push("err");
    if (shown.has(i.path)) cls.push("shown");
    if (i.path === focused) cls.push("focused");
    if (i.path === S.cursor) cls.push("cursor");
    const name = (multiDir ? `<span class="dir">${esc(dirOf(i.path))}/</span>` : "") + esc(base(i.path));
    if (i.error) {
      return `<div class="${cls.join(" ")}" data-path="${esc(i.path)}" title="${esc(i.path)}: ${esc(i.error)}">
        <span class="c-name">${name}</span><span class="c-shape" style="grid-column: span 3">${esc(i.error)}</span><span></span></div>`;
    }
    const live = i.rate > 0;
    const badges = (i.gpu ? `<span class="badge" title="GPU SHM${i.readable ? " (host copy)" : ", no host copy: not viewable"}">GPU</span>` : "")
      + (i.fifo > 1 ? `<span class="badge" title="FIFO of ${i.fifo}">FIFO</span>` : "");
    return `<div class="${cls.join(" ")}" data-path="${esc(i.path)}" title="${esc(i.path)}\ncnt0 ${i.cnt0} · ${(i.bytes / 1024).toFixed(1)} kB · ${i.nsem} semaphores${i.age != null ? " · written " + fmtDur(i.age) + " ago" : ""}">
      <span class="c-name">${name}${badges}</span>
      <span class="c-shape">${shapeStr(i.shape)}</span>
      <span class="c-dtype">${esc(i.dtype)}</span>
      <span class="c-rate${live ? " live" : ""}">${fmtRate(i.rate)}</span>
      <span class="c-add" title="Open in a new panel">+</span></div>`;
  }).join("");
  const list = $("#list");
  if (list._html !== html) {
    const top = list.scrollTop;
    list.innerHTML = html;
    list.scrollTop = top;
    list._html = html;
  }
  const total = S.items.size, live = Array.from(S.items.values()).filter(i => i.rate > 0).length;
  $("#listFoot").textContent = `${items.length}${items.length !== total ? " of " + total : ""} SHMs · ${live} updating`;
  $$("#listHead span[data-sort]").forEach(s => {
    s.classList.toggle("sorted", s.dataset.sort === S.sort.key);
    s.textContent = s.textContent.replace(/ [▲▼]$/, "") + (s.dataset.sort === S.sort.key ? (S.sort.dir > 0 ? " ▲" : " ▼") : "");
  });
}

function openPath(path, newPanel) {
  const it = S.items.get(path);
  if (it && it.error) return;
  S.cursor = path;
  if (newPanel || !S.focused) addPanel(path, S.focused && !newPanel ? S.focused.s : null);
  else S.focused.setPath(path);
  renderList();
  save();
}

function moveCursor(step) {
  if (!S.order.length) return;
  let i = S.order.indexOf(S.cursor);
  i = i < 0 ? 0 : clamp(i + step, 0, S.order.length - 1);
  S.cursor = S.order[i];
  const it = S.items.get(S.cursor);
  if (S.focused && it && !it.error) S.focused.setPath(S.cursor);
  renderList();
  const row = $(`#list .row[data-path="${CSS.escape(S.cursor)}"]`);
  if (row) row.scrollIntoView({ block: "nearest" });
  save();
}

/* ------------------------------------------------------------------ panels */

const DEFAULTS = {
  mode: "auto", cmap: "viridis", rev: false, scale: "lin", range: "minmax", lo: 0, hi: 1,
  rot: 0, fliph: false, flipv: false, slice: 0, part: "abs", avg: 1, strip: "off", trend: "mean", fps: 20,
};
const HIST = 1200;

class Panel {
  constructor(path, s) {
    this.id = "p" + (S.nextId++);
    this.path = path;
    this.s = Object.assign({}, DEFAULTS, s || {}, { slice: 0 });
    this.paused = false;
    this.el = $("#panelTpl").content.firstElementChild.cloneNode(true);
    this.cv = $(".cv", this.el);
    this.ctx = this.cv.getContext("2d");
    this.off = document.createElement("canvas");
    this.offCtx = this.off.getContext("2d");
    this.view = { zoom: 1, panX: 0, panY: 0 };
    this.pick = null;
    this.hover = null;
    this.reset();
    this.bind();
    this.applySettingsToUi();
    $("#panels").appendChild(this.el);
    new ResizeObserver(() => this.dirty()).observe($(".pbody", this.el));
    this.setTitle();
  }

  reset() {
    this.h = null; this.raw = null; this.data = null; this.notice = null;
    this.avgBuf = []; this.avgSum = null; this.ref = null; this.locked = null;
    this.hist = { t: new Float64Array(HIST), a: new Float64Array(HIST), b: new Float64Array(HIST), c: new Float64Array(HIST), n: 0, head: 0 };
    this.lastCnt = null;
    this.recv = [];
    this.tableAt = { r: 0, c: 0 };
    this.editing = null;
  }

  /* --- wiring */
  bind() {
    const el = this.el, s = this.s;
    el.addEventListener("pointerdown", () => focusPanel(this), true);
    $(".b-close", el).onclick = () => removePanel(this);
    $(".b-max", el).onclick = () => this.toggleMax();
    $(".b-pause", el).onclick = () => this.togglePause();
    $(".b-insp", el).onclick = () => { focusPanel(this); openInspector(); };
    $(".o-cmap", el).innerHTML = Object.keys(CMAPS).map(k => `<option>${k}</option>`).join("");
    const on = (cls, key, conv, ev = "change") => {
      $(cls, el).addEventListener(ev, e => {
        const t = e.target;
        s[key] = conv ? conv(t.type === "checkbox" ? t.checked : t.value) : (t.type === "checkbox" ? t.checked : t.value);
        if (key === "range" && s.range === "lock") this.locked = this.lastRange || null;
        if (key === "avg") { this.avgBuf = []; this.avgSum = null; }
        if (key === "rot") this.view = { zoom: 1, panX: 0, panY: 0 };
        if (["slice", "part", "fps"].includes(key)) { this.avgBuf = []; this.avgSum = null; this.subscribe(); }
        if (key !== "lo" && key !== "hi") this.applySettingsToUi();      // not under the user's typing
        this.reprocess();
        save();
      });
    };
    on(".o-mode", "mode"); on(".o-cmap", "cmap"); on(".o-rev", "rev"); on(".o-scale", "scale"); on(".o-range", "range");
    on(".o-lo", "lo", Number, "input"); on(".o-hi", "hi", Number, "input");
    on(".o-rot", "rot", Number); on(".o-fliph", "fliph"); on(".o-flipv", "flipv");
    on(".o-slice", "slice", v => Math.max(0, parseInt(v, 10) || 0));
    on(".o-part", "part"); on(".o-avg", "avg", v => clamp(parseInt(v, 10) || 1, 1, 200));
    on(".o-strip", "strip"); on(".o-trend", "trend"); on(".o-fps", "fps", Number);
    $(".b-ref", el).onclick = () => { if (this.cur) { this.ref = Float32Array.from(this.cur); this.reprocess(); } };
    $(".b-noref", el).onclick = () => { this.ref = null; this.reprocess(); };

    // image interaction
    const cv = this.cv;
    let drag = null;
    cv.addEventListener("wheel", e => {
      if (!this.isImage()) return;
      e.preventDefault();
      const r = cv.getBoundingClientRect();
      const mx = e.clientX - r.left - r.width / 2, my = e.clientY - r.top - r.height / 2;
      const f = Math.exp(-e.deltaY * (e.deltaMode ? 0.05 : 0.0015));
      const z = clamp(this.view.zoom * f, 0.2, 400), k = z / this.view.zoom;
      this.view.panX = mx - (mx - this.view.panX) * k;
      this.view.panY = my - (my - this.view.panY) * k;
      this.view.zoom = z;
      this.dirty();
    }, { passive: false });
    cv.addEventListener("pointerdown", e => {
      drag = { x: e.clientX, y: e.clientY, px: this.view.panX, py: this.view.panY, moved: false };
      cv.setPointerCapture(e.pointerId);
    });
    cv.addEventListener("pointermove", e => {
      const r = cv.getBoundingClientRect();
      this.hover = { x: e.clientX - r.left, y: e.clientY - r.top };
      if (drag && this.isImage()) {
        const dx = e.clientX - drag.x, dy = e.clientY - drag.y;
        if (Math.abs(dx) + Math.abs(dy) > 3) drag.moved = true;
        if (drag.moved) { this.view.panX = drag.px + dx; this.view.panY = drag.py + dy; }
      }
      this.dirty();
    });
    cv.addEventListener("pointerup", e => {
      if (drag && !drag.moved) {
        if (e.shiftKey) this.pick = null;
        else {
          const hit = this.hitTest(this.hover);
          if (hit) {
            this.pick = hit;
            if (this.s.strip === "off") { this.s.strip = "trend"; }
            this.s.trend = "pixel";
            this.resetHist();
            this.applySettingsToUi();
          }
        }
        this.dirty();
      }
      drag = null;
    });
    cv.addEventListener("pointerleave", () => { this.hover = null; this.dirty(); });
    cv.addEventListener("dblclick", () => { this.view = { zoom: 1, panX: 0, panY: 0 }; this.dirty(); });
  }

  applySettingsToUi() {
    const el = this.el, s = this.s;
    $(".o-mode", el).value = s.mode; $(".o-cmap", el).value = s.cmap; $(".o-rev", el).checked = s.rev;
    $(".o-scale", el).value = s.scale; $(".o-range", el).value = s.range;
    $(".o-lo", el).value = s.lo; $(".o-hi", el).value = s.hi;
    $(".o-rot", el).value = String(s.rot); $(".o-fliph", el).checked = s.fliph; $(".o-flipv", el).checked = s.flipv;
    $(".o-slice", el).value = s.slice; $(".o-part", el).value = s.part; $(".o-avg", el).value = s.avg;
    $(".o-strip", el).value = s.strip; $(".o-trend", el).value = s.trend; $(".o-fps", el).value = String(s.fps);
    const mode = this.mode();
    $$(".g-img", el).forEach(g => { g.hidden = mode !== "image"; });
    $(".g-manual", el).hidden = s.range !== "manual";
    $(".g-trend", el).hidden = s.strip !== "trend";
    $(".g-slice", el).hidden = !(this.h && this.h.depth);
    $(".g-cplx", el).hidden = !(this.h && /^complex/.test(this.h.src));
    $(".o-range", el).hidden = mode === "table" || mode === "value";
    $(".pstrip", el).hidden = s.strip === "off";
    $(".b-ref", el).classList.toggle("on", !!this.ref);
    $(".b-noref", el).disabled = !this.ref;
    $(".b-pause", el).classList.toggle("active", this.paused);
    $(".b-pause", el).textContent = this.paused ? "▶" : "❚❚";
  }

  setTitle() {
    const it = S.items.get(this.path);
    $(".pname", this.el).textContent = base(this.path);
    $(".pname", this.el).title = this.path;
    $(".pinfo", this.el).textContent = it && !it.error ? `${shapeStr(it.shape)} ${it.dtype}${it.gpu ? " · GPU" : ""}` : "";
  }

  subscribe(force) {
    send({ op: "sub", id: this.id, path: this.path, fps: this.s.fps, slice: this.s.slice, part: this.s.part,
           maxEl: 4 << 20, paused: this.paused });
    if (force) send({ op: "once", id: this.id });
  }

  setPath(path) {
    if (path === this.path) return;
    this.path = path;
    this.reset();
    this.pick = null;
    this.view = { zoom: 1, panX: 0, panY: 0 };
    this.s.slice = 0;
    if (this.s.range === "lock") this.s.range = "minmax";
    this.applySettingsToUi();
    this.setTitle();
    this.subscribe(true);
    this.dirty();
    if (S.focused === this) refreshInspector(true);
  }

  togglePause() {
    this.paused = !this.paused;
    this.subscribe();
    this.applySettingsToUi();
    this.dirty();
  }

  toggleMax() {
    const max = !this.el.classList.contains("max");
    S.panels.forEach(p => p.el.classList.remove("max"));
    this.el.classList.toggle("max", max);
    $("#main").classList.toggle("hasmax", max);
    this.dirty();
  }

  onListUpdate() {
    this.setTitle();
    this.dirty(true);
  }

  onNotice(m) {
    if (m.type === "gone") this.notice = "The SHM is gone: waiting for it to come back";
    else if (m.type === "unreadable") this.notice = m.gpu ? "GPU SHM without a host copy (DAO_GPU_MIRROR): its data cannot be read from /tmp" : "This SHM cannot be read";
    else this.notice = "Error: " + m.error;
    this.dirty();
  }

  /* --- frames */
  onFrame(h, data) {
    const now = performance.now();
    this.recv.push(now);
    while (this.recv.length && now - this.recv[0] > 2000) this.recv.shift();
    const shapeChanged = !this.h || this.h.view.join() !== h.view.join() || this.h.dtype !== h.dtype || this.h.slice !== h.slice;
    if (shapeChanged) { this.avgBuf = []; this.avgSum = null; this.ref = this.ref && this.ref.length === data.length ? this.ref : null; }
    const firstOrNew = !this.h || this.h.depth !== h.depth || this.h.src !== h.src;
    const dcnt = this.h ? h.cnt0 - this.h.cnt0 : 0;
    this.h = h;
    this.raw = data;
    this.notice = null;
    if (firstOrNew) this.applySettingsToUi();
    if (h.depth) $(".o-depth", this.el).textContent = `/ ${h.depth - 1}`;
    this.process();
    this.pushHist(dcnt);
    this.dirty();
  }

  reprocess() { if (this.raw) this.process(); this.resetHist(); this.dirty(); }

  process() {
    const raw = this.raw, n = raw.length, s = this.s;
    let cur = raw;
    if (s.avg > 1) {
      const f = Float32Array.from(raw);
      if (!this.avgSum || this.avgSum.length !== n) { this.avgSum = new Float64Array(n); this.avgBuf = []; }
      this.avgBuf.push(f);
      for (let i = 0; i < n; i++) this.avgSum[i] += f[i];
      while (this.avgBuf.length > s.avg) {
        const old = this.avgBuf.shift();
        for (let i = 0; i < n; i++) this.avgSum[i] -= old[i];
      }
      const k = 1 / this.avgBuf.length;
      cur = new Float32Array(n);
      for (let i = 0; i < n; i++) cur[i] = this.avgSum[i] * k;
    }
    if (this.ref && this.ref.length === n) {
      const d = new Float32Array(n), ref = this.ref;
      for (let i = 0; i < n; i++) d[i] = cur[i] - ref[i];
      cur = d;
    }
    this.cur = cur;
    // statistics: the server's (whole frame) unless the page changed the data or shows a part of it
    this.stats = (cur === raw && this.h.stride === 1) ? this.h.stats : localStats(cur);
  }

  resetHist() { this.hist.n = 0; this.hist.head = 0; }

  pushHist(dcnt) {
    const H = this.hist, st = this.stats || {}, i = H.head;
    H.t[i] = performance.now() / 1000;
    let a = NaN, b = NaN, c = NaN;
    switch (this.s.trend) {
      case "mean": a = st.mean; break;
      case "std": a = st.std; break;
      case "sum": a = st.sum; break;
      case "minmax": a = st.min; b = st.mean; c = st.max; break;
      case "cnt": a = dcnt; break;
      case "pixel": a = this.valueAt(this.pick); break;
    }
    if (this.mode() === "value" && this.s.trend !== "cnt") a = this.cur ? this.cur[0] : NaN;
    H.a[i] = a; H.b[i] = b; H.c[i] = c;
    H.head = (i + 1) % HIST;
    H.n = Math.min(HIST, H.n + 1);
  }

  valueAt(p) {
    if (!p || !this.cur || !this.h) return NaN;
    const [rows, cols] = this.dims();
    if (p.r < 0 || p.c < 0 || p.r >= rows || p.c >= cols) return NaN;
    return this.cur[p.r * cols + p.c];
  }

  /* --- geometry */
  dims() {
    const v = this.h.view;
    return v.length === 1 ? [1, v[0]] : [v[0], v[1]];
  }
  is1d() {
    if (!this.h) return false;
    const [r, c] = this.dims();
    return r === 1 || c === 1;
  }
  mode() {
    const m = this.s.mode;
    if (m !== "auto") return m;
    if (!this.h) return "image";
    const n = this.cur ? this.cur.length : 0;
    if (n === 1) return "value";
    return this.is1d() ? "plot" : "image";
  }
  isImage() { return this.mode() === "image"; }

  imageMatrix(cw, ch) {
    const [rows, cols] = this.dims();
    const rot = this.s.rot, swap = rot === 90 || rot === 270;
    const w = swap ? rows : cols, hh = swap ? cols : rows;
    const fit = Math.min((cw - 56) / w, (ch - 8) / hh);
    const k = Math.max(1e-6, fit * this.view.zoom);
    return new DOMMatrix()
      .translate(cw / 2 + this.view.panX - 26, ch / 2 + this.view.panY)
      .scale(k, k)
      .rotate(rot)
      .scale(this.s.fliph ? -1 : 1, this.s.flipv ? -1 : 1)
      .translate(-cols / 2, -rows / 2);
  }

  hitTest(pt) {
    if (!pt || !this.h) return null;
    const mode = this.mode();
    const [rows, cols] = this.dims();
    if (mode === "image") {
      const m = this.imageMatrix(this.cv.clientWidth, this.cv.clientHeight).inverse();
      const p = m.transformPoint(new DOMPoint(pt.x, pt.y));
      const c = Math.floor(p.x), r = Math.floor(p.y);
      if (r < 0 || c < 0 || r >= rows || c >= cols) return null;
      return { r, c };
    }
    if (mode === "plot" || mode === "bar") {
      const g = this.plotGeom;
      if (!g) return null;
      const i = Math.round((pt.x - g.x0) / g.dx);
      const n = rows * cols;
      if (i < 0 || i >= n) return null;
      return cols === 1 ? { r: i, c: 0 } : { r: 0, c: i };
    }
    return null;
  }

  /* --- range */
  range() {
    const s = this.s, st = this.stats || {};
    let lo, hi;
    if (s.range === "manual") { lo = s.lo; hi = s.hi; }
    else if (s.range === "lock" && this.locked) { [lo, hi] = this.locked; }
    else if (s.range.startsWith("p")) {
      const q = parseFloat(s.range.slice(1)) / 100;
      [lo, hi] = percentiles(this.cur, (1 - q) / 2, 1 - (1 - q) / 2);
    } else if (s.range === "sym") {
      const m = Math.max(Math.abs(st.min || 0), Math.abs(st.max || 0));
      lo = -m; hi = m;
    } else { lo = st.min; hi = st.max; }
    if (!isFinite(lo) || !isFinite(hi)) { lo = 0; hi = 1; }
    if (s.range !== "manual" && hi - lo <= 1e-5 * Math.max(Math.abs(lo), Math.abs(hi))) {
      // a flat frame (or one with float rounding only): one colour, not its noise stretched
      const c = (lo + hi) / 2, w = Math.abs(c) * 0.01 || 0.5;
      lo = c - w; hi = c + w;
    }
    if (hi <= lo) hi = lo + 1;
    this.lastRange = [lo, hi];
    return [lo, hi];
  }

  /* --- drawing */
  dirty(statusOnly) {
    this.needDraw = this.needDraw || !statusOnly;
    this.needStatus = true;
    scheduleDraw();
  }

  draw() {
    if (this.needDraw) { this.needDraw = false; this.drawBody(); this.drawStrip(); }
    if (this.needStatus) { this.needStatus = false; this.drawStatus(); }
  }

  drawBody() {
    const body = $(".pbody", this.el), cv = this.cv, ctx = this.ctx;
    const dpr = window.devicePixelRatio || 1;
    const cw = body.clientWidth, ch = body.clientHeight;
    if (cv.width !== Math.round(cw * dpr) || cv.height !== Math.round(ch * dpr)) {
      cv.width = Math.round(cw * dpr); cv.height = Math.round(ch * dpr);
    }
    const ov = $(".overlay", this.el);
    ov.hidden = !this.notice && !!this.h;
    ov.textContent = this.notice || (this.h ? "" : (S.connected ? "waiting for the first frame…" : "not connected"));
    const mode = this.mode();
    $(".tablewrap", this.el).hidden = mode !== "table";
    $(".valuebox", this.el).hidden = mode !== "value";
    $(".cbar", this.el).hidden = mode !== "image";
    cv.style.visibility = mode === "image" || mode === "plot" || mode === "bar" ? "visible" : "hidden";
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, cw, ch);
    if (!this.h || !this.cur) return;
    if (mode === "image") this.drawImage(ctx, cw, ch);
    else if (mode === "plot" || mode === "bar") this.drawPlot(ctx, cw, ch, mode === "bar");
    else if (mode === "table") this.drawTable();
    else if (mode === "value") this.drawValue();
  }

  renderOffscreen() {
    const [rows, cols] = this.dims();
    if (this.off.width !== cols || this.off.height !== rows) {
      this.off.width = cols; this.off.height = rows;
      this.img = this.offCtx.createImageData(cols, rows);
      this.img32 = new Uint32Array(this.img.data.buffer);
    }
    const [lo, hi] = this.range();
    const d = this.cur, out = this.img32, L = lut(this.s.cmap, this.s.rev), n = d.length;
    const inv = 4095 / (hi - lo);
    if (this.s.scale === "lin") {
      const k = 255 / (hi - lo);
      for (let i = 0; i < n; i++) {
        const v = d[i];
        if (v !== v) { out[i] = NAN_COLOR; continue; }
        let x = (v - lo) * k;
        x = x < 0 ? 0 : x > 255 ? 255 : x;
        out[i] = L[x | 0];
      }
    } else {
      const SL = scaleLut(this.s.scale);
      for (let i = 0; i < n; i++) {
        const v = d[i];
        if (v !== v) { out[i] = NAN_COLOR; continue; }
        let x = (v - lo) * inv;
        x = x < 0 ? 0 : x > 4095 ? 4095 : x;
        out[i] = L[SL[x | 0]];
      }
    }
    this.offCtx.putImageData(this.img, 0, 0);
    return [lo, hi];
  }

  drawImage(ctx, cw, ch) {
    const [rows, cols] = this.dims();
    const [lo, hi] = this.renderOffscreen();
    const m = this.imageMatrix(cw, ch);
    const dpr = window.devicePixelRatio || 1;
    ctx.save();
    ctx.setTransform(new DOMMatrix().scale(dpr, dpr).multiply(m));
    ctx.imageSmoothingEnabled = false;
    ctx.drawImage(this.off, 0, 0);
    const k = Math.hypot(m.a, m.b);                   // CSS px per pixel
    if (k >= 6 && rows * cols > 1) {
      ctx.strokeStyle = "rgba(128,128,128,.35)";
      ctx.lineWidth = 1 / k;
      ctx.beginPath();
      for (let r = 0; r <= rows; r++) { ctx.moveTo(0, r); ctx.lineTo(cols, r); }
      for (let c = 0; c <= cols; c++) { ctx.moveTo(c, 0); ctx.lineTo(c, rows); }
      ctx.stroke();
    }
    if (this.pick) {
      ctx.strokeStyle = "#ff3df0";
      ctx.lineWidth = 2 / k;
      ctx.strokeRect(this.pick.c, this.pick.r, 1, 1);
    }
    ctx.restore();
    // values in the pixels once they are big enough
    if (k >= 38) {
      const inv = m.inverse();
      const corners = [[0, 0], [cw, 0], [0, ch], [cw, ch]].map(([x, y]) => inv.transformPoint(new DOMPoint(x, y)));
      const c0 = clamp(Math.floor(Math.min(...corners.map(p => p.x))), 0, cols), c1 = clamp(Math.ceil(Math.max(...corners.map(p => p.x))), 0, cols);
      const r0 = clamp(Math.floor(Math.min(...corners.map(p => p.y))), 0, rows), r1 = clamp(Math.ceil(Math.max(...corners.map(p => p.y))), 0, rows);
      if ((c1 - c0) * (r1 - r0) <= 900) {
        ctx.font = `${Math.min(13, k / 4.2)}px ${cssVar("--mono")}`;
        ctx.textAlign = "center"; ctx.textBaseline = "middle";
        const intLike = intDtype(this.h.dtype) && this.s.avg === 1 && !this.ref;
        const L = lut(this.s.cmap, this.s.rev), f = scaleFn(this.s.scale);
        for (let r = r0; r < r1; r++) for (let c = c0; c < c1; c++) {
          const v = this.cur[r * cols + c];
          const p = m.transformPoint(new DOMPoint(c + 0.5, r + 0.5));
          const col = L[Math.round(clamp(f(clamp((v - lo) / (hi - lo), 0, 1)), 0, 1) * 255)];
          const lum = 0.299 * (col & 255) + 0.587 * ((col >>> 8) & 255) + 0.114 * ((col >>> 16) & 255);
          ctx.fillStyle = v !== v ? "#000" : lum > 140 ? "#000" : "#fff";
          ctx.fillText(fmt(v, intLike), p.x, p.y);
        }
      }
    }
    this.drawColorbar(lo, hi);
  }

  drawColorbar(lo, hi) {
    const cb = $(".cbar canvas", this.el);
    if (cb.width !== 1 || cb.height !== 256) { cb.width = 1; cb.height = 256; }
    const g = cb.getContext("2d"), img = g.createImageData(1, 256), u = new Uint32Array(img.data.buffer);
    const L = lut(this.s.cmap, this.s.rev), f = scaleFn(this.s.scale);
    for (let i = 0; i < 256; i++) u[255 - i] = L[Math.round(clamp(f(i / 255), 0, 1) * 255)];
    g.putImageData(img, 0, 0);
    $(".cb-hi", this.el).textContent = fmt(hi);
    $(".cb-lo", this.el).textContent = fmt(lo);
  }

  drawPlot(ctx, cw, ch, bars) {
    const d = this.cur, n = d.length;
    let [lo, hi] = this.range();
    if (this.s.range === "minmax") { const pad = (hi - lo) * 0.05; lo -= pad; hi += pad; }
    const ml = 58, mr = 12, mt = 10, mb = 22;
    const w = cw - ml - mr, h = ch - mt - mb;
    if (w < 10 || h < 10) return;
    const dx = w / Math.max(1, n - (bars ? 0 : 1));
    const x0 = ml + (bars ? dx / 2 : 0);
    this.plotGeom = { x0, dx };
    const Y = v => mt + h - (v - lo) / (hi - lo) * h;
    const fg = cssVar("--fg"), muted = cssVar("--muted"), grid = cssVar("--grid"), acc = cssVar("--accent");
    ctx.font = `10.5px ${cssVar("--mono")}`;
    ctx.fillStyle = muted; ctx.strokeStyle = grid; ctx.lineWidth = 1;
    ctx.textAlign = "right"; ctx.textBaseline = "middle";
    for (const t of niceTicks(lo, hi, Math.max(2, Math.floor(h / 40)))) {
      const y = Math.round(Y(t)) + 0.5;
      ctx.beginPath(); ctx.moveTo(ml, y); ctx.lineTo(ml + w, y); ctx.stroke();
      ctx.fillText(fmt(t), ml - 6, y);
    }
    ctx.textAlign = "center"; ctx.textBaseline = "top";
    for (const t of niceTicks(0, n - 1, Math.max(2, Math.floor(w / 80)))) {
      if (!Number.isInteger(t)) continue;
      const x = x0 + t * dx;
      ctx.fillText(String(t * (this.h.stride || 1)), x, mt + h + 5);
    }
    if (lo < 0 && hi > 0) { ctx.strokeStyle = muted; ctx.beginPath(); ctx.moveTo(ml, Y(0)); ctx.lineTo(ml + w, Y(0)); ctx.stroke(); }
    ctx.save();
    ctx.beginPath(); ctx.rect(ml, mt, w, h); ctx.clip();
    if (bars) {
      ctx.fillStyle = acc;
      const y0 = Y(clamp(0, lo, hi)), bw = Math.max(1, dx * 0.8);
      for (let i = 0; i < n; i++) {
        const y = Y(d[i]);
        ctx.fillRect(x0 + i * dx - bw / 2, Math.min(y, y0), bw, Math.abs(y0 - y) || 1);
      }
    } else {
      ctx.strokeStyle = acc; ctx.lineWidth = 1.4;
      ctx.beginPath();
      if (n > w * 2) {
        // min / max envelope per pixel column
        const per = n / w;
        for (let px = 0; px < w; px++) {
          const a = Math.floor(px * per), b = Math.min(n, Math.floor((px + 1) * per));
          let mn = Infinity, mx = -Infinity;
          for (let i = a; i < b; i++) { const v = d[i]; if (v < mn) mn = v; if (v > mx) mx = v; }
          ctx.moveTo(ml + px + 0.5, Y(mn)); ctx.lineTo(ml + px + 0.5, Y(mx));
        }
      } else {
        for (let i = 0; i < n; i++) { const x = x0 + i * dx, y = Y(d[i]); i ? ctx.lineTo(x, y) : ctx.moveTo(x, y); }
        if (n === 1) ctx.arc(x0, Y(d[0]), 2, 0, 7);
      }
      ctx.stroke();
      if (dx >= 6) {
        ctx.fillStyle = acc;
        for (let i = 0; i < n; i++) { ctx.beginPath(); ctx.arc(x0 + i * dx, Y(d[i]), 2, 0, 7); ctx.fill(); }
      }
    }
    const hit = this.hitTest(this.hover), mark = hit || this.pick;
    if (mark) {
      const i = mark.r * this.dims()[1] + mark.c, x = x0 + (this.dims()[1] === 1 ? mark.r : mark.c) * dx;
      ctx.strokeStyle = hit ? fg : "#ff3df0"; ctx.setLineDash([3, 3]);
      ctx.beginPath(); ctx.moveTo(x, mt); ctx.lineTo(x, mt + h); ctx.stroke(); ctx.setLineDash([]);
      ctx.fillStyle = "#ff3df0"; ctx.beginPath(); ctx.arc(x, Y(d[i]), 3.5, 0, 7); ctx.fill();
    }
    ctx.restore();
  }

  drawTable() {
    const wrap = $(".tablewrap", this.el);
    if (this.editing) return;                          // do not redraw under the user's typing
    const now = performance.now();
    if (this.tableT && now - this.tableT < 100 && wrap._path === this.path) { this.needDraw = true; scheduleDraw(); return; }
    this.tableT = now;
    const [rows, cols] = this.dims(), d = this.cur;
    const R = 40, C = 16;
    const r0 = clamp(this.tableAt.r, 0, Math.max(0, rows - 1)), c0 = clamp(this.tableAt.c, 0, Math.max(0, cols - 1));
    const r1 = Math.min(rows, r0 + R), c1 = Math.min(cols, c0 + C);
    const intLike = intDtype(this.h.dtype) && this.s.avg === 1 && !this.ref;
    const st = this.h.stride || 1;
    let html = `<div class="tablenav">rows ${r0}–${r1 - 1} of ${rows}, columns ${c0}–${c1 - 1} of ${cols}
      <button data-nav="up">▲</button><button data-nav="down">▼</button><button data-nav="left">◀</button><button data-nav="right">▶</button>
      ${st > 1 ? `<span class="warn">every ${st}th element: not editable</span>` : ""}</div><table><tr><th class="rh"></th>`;
    for (let c = c0; c < c1; c++) html += `<th>${c * st}</th>`;
    html += "</tr>";
    for (let r = r0; r < r1; r++) {
      html += `<tr><th class="rh">${r * st}</th>`;
      for (let c = c0; c < c1; c++) html += `<td data-r="${r}" data-c="${c}">${esc(fmt(d[r * cols + c], intLike))}</td>`;
      html += "</tr>";
    }
    html += "</table>";
    wrap.innerHTML = html;
    wrap._path = this.path;
    $$("button[data-nav]", wrap).forEach(b => b.onclick = () => {
      const k = b.dataset.nav;
      if (k === "up") this.tableAt.r = Math.max(0, r0 - R);
      if (k === "down") this.tableAt.r = Math.min(Math.max(0, rows - R), r0 + R);
      if (k === "left") this.tableAt.c = Math.max(0, c0 - C);
      if (k === "right") this.tableAt.c = Math.min(Math.max(0, cols - C), c0 + C);
      this.tableT = 0; this.dirty();
    });
    if (S.hello && S.hello.write && st === 1 && !this.ref && this.s.avg === 1) {
      wrap.ondblclick = e => {
        const td = e.target.closest("td");
        if (!td) return;
        const r = +td.dataset.r, c = +td.dataset.c;
        this.editCell(td, r, c);
      };
    } else wrap.ondblclick = null;
  }

  editCell(td, r, c) {
    const [rows, cols] = this.dims();
    const old = this.cur[r * cols + c];
    this.editing = { r, c };
    td.classList.add("edit");
    td.innerHTML = `<input type="number" step="any" value="${old}">`;
    const inp = $("input", td);
    inp.focus(); inp.select();
    const done = async ok => {
      if (!this.editing) return;
      this.editing = null;
      if (ok && inp.value !== "" && Number(inp.value) !== old) {
        const slice = this.h.depth ? this.h.slice * rows * cols : 0;
        try {
          await request("set", { path: this.path, mode: "elements", index: [slice + r * cols + c], values: [Number(inp.value)] });
          toast(`${base(this.path)}[${r}, ${c}] = ${inp.value}`, "ok");
        } catch (e) { toast(e.message, "bad"); }
      }
      this.tableT = 0; this.dirty();
    };
    inp.onkeydown = e => { if (e.key === "Enter") done(true); if (e.key === "Escape") done(false); e.stopPropagation(); };
    inp.onblur = () => done(true);
  }

  drawValue() {
    const box = $(".valuebox", this.el);
    const v = this.cur[0];
    const intLike = intDtype(this.h.dtype) && this.s.avg === 1 && !this.ref;
    const html = `<div class="big">${esc(fmt(v, intLike))}</div><div class="sub">${esc(this.h.src)}${this.ref ? " − ref" : ""}${this.s.avg > 1 ? ` · avg ${this.s.avg}` : ""}</div>`
      + (S.hello && S.hello.write ? `<div class="form"><input class="v-new" type="number" step="any" placeholder="new value"><button class="v-set">Set</button></div>` : "");
    if (box._html !== html.replace(/<div class="big">.*?<\/div>/, "") || !box.firstChild) {
      box.innerHTML = html;
      box._html = html.replace(/<div class="big">.*?<\/div>/, "");
      const b = $(".v-set", box);
      if (b) {
        const go = async () => {
          const inp = $(".v-new", box);
          if (inp.value === "") return;
          try { await request("set", { path: this.path, mode: "value", value: Number(inp.value) }); toast(`${base(this.path)} = ${inp.value}`, "ok"); inp.value = ""; }
          catch (e) { toast(e.message, "bad"); }
        };
        b.onclick = go;
        $(".v-new", box).onkeydown = e => { if (e.key === "Enter") go(); e.stopPropagation(); };
      }
    } else $(".big", box).textContent = fmt(v, intLike);
  }

  drawStrip() {
    if (this.s.strip === "off") return;
    const wrap = $(".pstrip", this.el), cv = $("canvas", wrap), ctx = cv.getContext("2d");
    const dpr = window.devicePixelRatio || 1, cw = wrap.clientWidth, ch = wrap.clientHeight;
    if (cv.width !== Math.round(cw * dpr) || cv.height !== Math.round(ch * dpr)) { cv.width = Math.round(cw * dpr); cv.height = Math.round(ch * dpr); }
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, cw, ch);
    const ml = 58, mr = 12, mt = 6, mb = 16, w = cw - ml - mr, h = ch - mt - mb;
    if (w < 10 || h < 10) return;
    const muted = cssVar("--muted"), grid = cssVar("--grid"), acc = cssVar("--accent");
    ctx.font = `10px ${cssVar("--mono")}`;
    if (this.s.strip === "hist") return this.drawHist(ctx, ml, mt, w, h, muted, grid, acc);
    const H = this.hist;
    if (H.n < 1) { ctx.fillStyle = muted; ctx.fillText(this.s.trend === "pixel" && !this.pick ? "click a pixel to follow it" : "collecting…", ml + 8, mt + h / 2); return; }
    const idx = k => (H.head - H.n + k + HIST) % HIST;
    const tEnd = H.t[idx(H.n - 1)], tStart = H.t[idx(0)], span = Math.max(1e-3, tEnd - tStart);
    let lo = Infinity, hi = -Infinity;
    const series = this.s.trend === "minmax" ? [H.a, H.b, H.c] : [H.a];
    for (let k = 0; k < H.n; k++) for (const s of series) { const v = s[idx(k)]; if (isFinite(v)) { if (v < lo) lo = v; if (v > hi) hi = v; } }
    if (!isFinite(lo)) { lo = 0; hi = 1; }
    if (hi === lo) { hi += 0.5 || 1; lo -= 0.5; }
    const pad = (hi - lo) * 0.08; lo -= pad; hi += pad;
    const X = t => ml + (t - tStart) / span * w, Y = v => mt + h - (v - lo) / (hi - lo) * h;
    ctx.fillStyle = muted; ctx.strokeStyle = grid; ctx.textAlign = "right"; ctx.textBaseline = "middle";
    for (const t of niceTicks(lo, hi, Math.max(2, Math.floor(h / 26)))) {
      const y = Math.round(Y(t)) + 0.5;
      ctx.beginPath(); ctx.moveTo(ml, y); ctx.lineTo(ml + w, y); ctx.stroke();
      ctx.fillText(fmt(t), ml - 6, y);
    }
    ctx.textAlign = "center"; ctx.textBaseline = "top";
    ctx.fillText(`−${fmtDur(span)}`, ml + 18, mt + h + 3);
    ctx.fillText("now", ml + w - 10, mt + h + 3);
    const colors = this.s.trend === "minmax" ? [muted, acc, muted] : [acc];
    series.forEach((s, j) => {
      ctx.strokeStyle = colors[j]; ctx.lineWidth = j === 1 || series.length === 1 ? 1.5 : 1;
      ctx.beginPath();
      let pen = false;
      for (let k = 0; k < H.n; k++) {
        const i = idx(k), v = s[i];
        if (!isFinite(v)) { pen = false; continue; }
        const x = X(H.t[i]), y = Y(v);
        pen ? ctx.lineTo(x, y) : ctx.moveTo(x, y);
        pen = true;
      }
      ctx.stroke();
    });
    const last = H.a[idx(H.n - 1)];
    ctx.textAlign = "left"; ctx.textBaseline = "top"; ctx.fillStyle = cssVar("--fg");
    const label = { mean: "mean", std: "std", sum: "sum", minmax: "min / mean / max", cnt: "Δcnt0 per frame shown", pixel: this.pick ? `pixel [${this.pick.r * (this.h.stride || 1)}, ${this.pick.c * (this.h.stride || 1)}]` : "pixel" }[this.s.trend];
    ctx.fillText(`${label}: ${fmt(last)}`, ml + 6, mt + 2);
  }

  drawHist(ctx, ml, mt, w, h, muted, grid, acc) {
    if (!this.cur) return;
    const [lo, hi] = this.lastRange || this.range();
    const nb = Math.max(10, Math.min(200, Math.floor(w / 4)));
    const counts = new Float64Array(nb), d = this.cur, k = nb / (hi - lo);
    let out = 0;
    for (let i = 0; i < d.length; i++) {
      const b = Math.floor((d[i] - lo) * k);
      if (b >= 0 && b < nb) counts[b]++; else if (b === nb && d[i] === hi) counts[nb - 1]++; else out++;
    }
    let mx = 0;
    for (const c of counts) if (c > mx) mx = c;
    const lg = v => Math.log10(1 + v), top = lg(mx) || 1;
    ctx.fillStyle = acc;
    const bw = w / nb;
    for (let b = 0; b < nb; b++) {
      const bh = lg(counts[b]) / top * h;
      ctx.fillRect(ml + b * bw, mt + h - bh, Math.max(1, bw - 0.5), bh);
    }
    ctx.fillStyle = muted; ctx.textBaseline = "top";
    ctx.textAlign = "left"; ctx.fillText(fmt(lo), ml, mt + h + 3);
    ctx.textAlign = "right"; ctx.fillText(fmt(hi), ml + w, mt + h + 3);
    ctx.textAlign = "left"; ctx.fillStyle = cssVar("--fg");
    ctx.fillText(`log count · ${nb} bins${out ? ` · ${out} outside` : ""}`, ml + 6, mt + 2);
    ctx.textAlign = "right"; ctx.fillStyle = muted; ctx.textBaseline = "middle";
    ctx.fillText(String(mx), ml - 6, mt + 4);
  }

  drawStatus() {
    const el = this.el, h = this.h, it = S.items.get(this.path);
    const dot = $(".dot", el);
    const live = it && it.rate > 0;
    dot.className = "dot " + (this.notice ? "gone" : this.paused ? "" : live ? "live" : (h ? "stale" : ""));
    if (!h) { $(".s-cnt", el).textContent = ""; $(".s-rate", el).textContent = ""; $(".s-age", el).textContent = ""; $(".s-stats", el).textContent = ""; $(".s-cursor", el).textContent = ""; return; }
    const span = this.recv.length > 1 ? (this.recv[this.recv.length - 1] - this.recv[0]) / 1000 : 0;
    const fps = span > 0.05 ? (this.recv.length - 1) / span : 0;
    $(".s-cnt", el).textContent = `cnt0 ${h.cnt0}`;
    $(".s-cnt", el).title = `cnt1 ${h.cnt1} · cnt2 ${h.cnt2}`;
    $(".s-rate", el).innerHTML = `${it ? fmtRate(it.rate) : "–"} Hz <span title="frames shown per second">(${fps.toFixed(fps < 10 ? 1 : 0)} shown)</span>`;
    const age = h.ts > 0 ? h.now - h.ts : null;
    const ageNow = h.ts > 0 ? (Date.now() / 1000 - S.serverSkew) - h.ts : null;
    const ageEl = $(".s-age", el);
    ageEl.textContent = age === null ? "" : (live ? `read ${fmtDur(age)} after write` : `written ${fmtDur(ageNow)} ago`);
    ageEl.classList.toggle("hot", !live && ageNow !== null && ageNow > 5);
    const st = this.stats || {};
    const intLike = intDtype(h.dtype) && this.s.avg === 1 && !this.ref;
    $(".s-stats", el).textContent = `min ${fmt(st.min, intLike)} max ${fmt(st.max, intLike)} mean ${fmt(st.mean)} std ${fmt(st.std)}`
      + (st.nonfinite ? ` · ${st.nonfinite} NaN/inf` : "")
      + (h.stride > 1 ? ` · 1/${h.stride} shown` : "")
      + (this.ref ? " · −ref" : "") + (this.s.avg > 1 ? ` · avg ${this.avgBuf.length}/${this.s.avg}` : "")
      + (this.paused ? " · paused" : "");
    const hit = this.hitTest(this.hover);
    const cur = $(".s-cursor", el);
    if (hit) {
      const v = this.valueAt(hit), sd = h.stride || 1;
      const where = this.is1d() && this.mode() !== "image" ? `[${(this.dims()[1] === 1 ? hit.r : hit.c) * sd}]` : `[${hit.r * sd}, ${hit.c * sd}]`;
      cur.textContent = `${where} = ${fmt(v, intLike)}`;
    } else if (this.pick) {
      cur.textContent = `picked [${this.pick.r * (h.stride || 1)}, ${this.pick.c * (h.stride || 1)}] = ${fmt(this.valueAt(this.pick), intLike)}`;
    } else cur.textContent = "";
  }

  pngBlob() {
    if (!this.h || this.mode() !== "image") return null;
    this.renderOffscreen();
    const [rows, cols] = this.dims(), rot = this.s.rot, swap = rot === 90 || rot === 270;
    const c = document.createElement("canvas");
    c.width = swap ? rows : cols; c.height = swap ? cols : rows;
    const g = c.getContext("2d");
    g.setTransform(new DOMMatrix().translate(c.width / 2, c.height / 2).rotate(rot)
      .scale(this.s.fliph ? -1 : 1, this.s.flipv ? -1 : 1).translate(-cols / 2, -rows / 2));
    g.drawImage(this.off, 0, 0);
    return new Promise(res => c.toBlob(res, "image/png"));
  }
}

function localStats(d) {
  let mn = Infinity, mx = -Infinity, s = 0, s2 = 0, n = 0, bad = 0;
  for (let i = 0; i < d.length; i++) {
    const v = d[i];
    if (!isFinite(v)) { bad++; continue; }
    if (v < mn) mn = v; if (v > mx) mx = v;
    s += v; s2 += v * v; n++;
  }
  if (!n) return { nonfinite: bad };
  const mean = s / n;
  return { min: mn, max: mx, mean, std: Math.sqrt(Math.max(0, s2 / n - mean * mean)), sum: s, nonfinite: bad };
}

function percentiles(d, a, b) {
  const n = d.length, step = Math.max(1, Math.floor(n / 50000));
  const sample = new Float64Array(Math.ceil(n / step));
  let m = 0;
  for (let i = 0; i < n; i += step) { const v = d[i]; if (isFinite(v)) sample[m++] = v; }
  if (!m) return [0, 1];
  const s = sample.subarray(0, m).sort();
  return [s[Math.floor(a * (m - 1))], s[Math.ceil(b * (m - 1))]];
}

let drawQueued = false;
function scheduleDraw() {
  if (drawQueued) return;
  drawQueued = true;
  requestAnimationFrame(() => {
    drawQueued = false;
    for (const p of S.panels) p.draw();
  });
}

function addPanel(path, settings) {
  const p = new Panel(path, settings);
  S.panels.push(p);
  $("#empty").hidden = true;
  layout();
  focusPanel(p);
  p.subscribe(true);
  save();
  return p;
}

function removePanel(p) {
  send({ op: "unsub", id: p.id });
  p.el.remove();
  if (p.el.classList.contains("max")) $("#main").classList.remove("hasmax");
  S.panels = S.panels.filter(q => q !== p);
  if (S.focused === p) focusPanel(S.panels[S.panels.length - 1] || null);
  $("#empty").hidden = S.panels.length > 0;
  layout();
  renderList();
  save();
}

function focusPanel(p) {
  if (S.focused === p) return;
  S.focused = p;
  S.panels.forEach(q => q.el.classList.toggle("focused", q === p));
  if (p) S.cursor = p.path;
  renderList();
  refreshInspector(true);
}

function layout() {
  const n = S.panels.length, el = $("#panels"), L = S.prefs.layout;
  let cols;
  if (L === "rows") cols = Math.max(1, n);
  else if (L === "auto") cols = n <= 1 ? 1 : n <= 4 ? 2 : n <= 9 ? 3 : 4;
  else cols = Math.min(Math.max(1, n), parseInt(L, 10));
  el.style.gridTemplateColumns = `repeat(${cols}, minmax(0, 1fr))`;
  const rows = Math.ceil(n / cols) || 1;
  el.style.gridTemplateRows = rows <= 3 ? `repeat(${rows}, minmax(260px, 1fr))` : "";   // more: scroll
  S.panels.forEach(p => p.dirty());
}

/* --------------------------------------------------------------- inspector */

let inspTimer = 0, inspTab = "info";
function openInspector(toggle) {
  const insp = $("#inspector");
  insp.hidden = toggle ? !insp.hidden : false;
  if (!insp.hidden) $("#tools").hidden = true;          // one drawer at a time
  S.panels.forEach(p => p.dirty());
  refreshInspector(true);
}

function setTab(t) {
  inspTab = t;
  $$(".tabs button").forEach(b => b.classList.toggle("on", b.dataset.tab === t));
  ["info", "sems", "data", "rec"].forEach(k => { $("#tab-" + k).hidden = k !== t; });
  refreshInspector(true);
}

async function refreshInspector(now) {
  clearTimeout(inspTimer);
  const insp = $("#inspector");
  if (insp.hidden) return;
  const p = S.focused;
  $("#inspTitle").textContent = p ? p.path : "no panel selected";
  if (p && S.connected) {
    try {
      if (inspTab === "info") {
        const r = await request("meta", { path: p.path });
        if (S.focused === p) renderMeta(r.meta);
      } else if (inspTab === "sems") {
        const r = await request("sems", { path: p.path });
        if (S.focused === p) renderSems(r);
      } else if (inspTab === "rec" && window.Tools) {
        Tools.recPanel(p);
      }
    } catch (e) {
      if (inspTab === "info" || inspTab === "sems") $("#tab-" + inspTab).innerHTML = `<p class="msg bad">${esc(e.message)}</p>`;
    }
  }
  inspTimer = setTimeout(refreshInspector, 1000);
}

function renderMeta(m) {
  const t = v => v ? new Date(v * 1000).toLocaleString() + "." + String(Math.floor((v % 1) * 1e6)).padStart(6, "0") : "–";
  const it = S.items.get(m.path) || {};
  const rows = [
    ["name", m.name], ["shape", shapeStr(m.shape) + ` (naxis ${m.naxis})`], ["type", `${m.dtype} (atype ${m.atype})`],
    ["elements", m.nelement], ["bytes", `${m.bytes} per frame · file ${m.file_bytes}`],
    ["cnt0", m.cnt0], ["cnt1", m.cnt1], ["cnt2", m.cnt2], ["rate", (it.rate != null ? fmtRate(it.rate) + " Hz" : "–")],
    ["written", t(m.atime)], ["created", t(m.creation_time)], ["semaphores", m.sem],
    ["FIFO", m.fifo_size > 1 ? `${m.fifo_size} frames, last ${m.fifo_last_written}` : "no"],
    ["write flag", m.write], ["status", m.status], ["keywords", m.NBkw], ["layout", m.layout],
  ];
  if (m.gpu) rows.push(["GPU", `device ${m.gpu_device} · ${m.gpu_bytes} bytes · ${m.gpu_mirror ? "host copy kept current" : "no host copy"}`]);
  $("#tab-info").innerHTML = `<table class="kv">${rows.map(([k, v]) => `<tr><td>${esc(k)}</td><td>${esc(v)}</td></tr>`).join("")}</table>
    <h4>Python</h4><pre class="code">import dao
s = dao.shm("${esc(m.path)}")
x = s.get_data(check=True)    # waits for the next frame</pre>`;
}

function shortCmd(cmd) {
  const parts = (cmd || "").split(/\s+/).filter(Boolean);
  if (!parts.length) return "?";
  const exe = parts[0].replace(/^.*\//, "");
  if (exe.startsWith("python") && parts.length > 1) {
    if (parts[1] === "-c") return exe + " -c";
    if (parts[1] === "-m" && parts[2]) return parts[2];
    const a = parts.slice(1).find(x => !x.startsWith("-"));
    if (a) return a.replace(/^.*\//, "");
  }
  return exe;
}

function renderSems(r) {
  const el = $("#tab-sems");
  if (r.users === null) { el.innerHTML = `<p class="muted">Semaphore users cannot be read on this system.</p>`; return; }
  const used = Object.keys(r.users).length;
  let html = `<div class="semsum">${r.nsem} semaphores: ${used} in use, ${r.nsem - used} free</div><div class="semgrid">`;
  for (let s = 0; s < r.nsem; s++) {
    const u = r.users[String(s)];
    if (!u) html += `<div class="sem free"><b>${String(s).padStart(2)}</b>  free</div>`;
    else if (!u.length) html += `<div class="sem unk" title="Taken by a process of another user"><b>${String(s).padStart(2)}</b>  in use · process not visible</div>`;
    else html += `<div class="sem used" title="${esc(u.map(x => `PID ${x.pid}: ${x.cmd}`).join("\n"))}"><b>${String(s).padStart(2)}</b>  in use${u.length > 1 ? " +" + (u.length - 1) : ""} · PID ${u[0].pid} · ${esc(shortCmd(u[0].cmd))}</div>`;
  }
  el.innerHTML = html + "</div><p class=\"muted\">Each reader waiting on this SHM holds a semaphore of its own. The viewer holds none.</p>";
}

function dataMsg(text, ok) {
  const m = $("#dataMsg");
  m.textContent = text;
  m.className = "msg " + (ok ? "ok" : "bad");
}

function bindInspector() {
  $$(".tabs button").forEach(b => b.onclick = () => setTab(b.dataset.tab));
  $("#inspClose").onclick = () => { $("#inspector").hidden = true; S.panels.forEach(p => p.dirty()); };
  $$("button[data-dl]").forEach(b => b.onclick = () => {
    if (!S.focused) return;
    const q = new URLSearchParams(location.search);
    q.set("path", S.focused.path); q.set("fmt", b.dataset.dl);
    location.href = "/api/download?" + q.toString();
  });
  $("#dlPng").onclick = async () => {
    const p = S.focused;
    if (!p) return;
    const blob = await p.pngBlob();
    if (!blob) return dataMsg("PNG: image view only", false);
    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = `${base(p.path)}_${p.h.cnt0}.png`;
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 2000);
  };
  const confirmWrite = what => confirm(`${what}\n\n${S.focused.path}\n\nThis writes the SHM (readers see a new frame).`);
  $("#btnSetValue").onclick = async () => {
    if (!S.focused || !confirmWrite(`Set every element to ${$("#setValue").value}?`)) return;
    try { const r = await request("set", { path: S.focused.path, mode: "value", value: Number($("#setValue").value) }); dataMsg(r.info, true); }
    catch (e) { dataMsg(e.message, false); }
  };
  $("#btnRandom").onclick = async () => {
    if (!S.focused || !confirmWrite(`Fill with random values in [${$("#rndMin").value}, ${$("#rndMax").value}]?`)) return;
    try { const r = await request("set", { path: S.focused.path, mode: "random", min: Number($("#rndMin").value), max: Number($("#rndMax").value) }); dataMsg(r.info, true); }
    catch (e) { dataMsg(e.message, false); }
  };
  $("#loadFile").onchange = async e => {
    const f = e.target.files[0];
    e.target.value = "";
    if (!f || !S.focused || !confirmWrite(`Load ${f.name} into the SHM?`)) return;
    const q = new URLSearchParams(location.search);
    q.set("path", S.focused.path); q.set("name", f.name);
    if ($("#loadCast").checked) q.set("cast", "1");
    try {
      const r = await fetch("/api/load?" + q.toString(), { method: "POST", body: f });
      const j = await r.json().catch(() => ({ ok: false, error: r.statusText }));
      dataMsg(j.ok ? j.info : j.error, j.ok);
    } catch (err) { dataMsg(err.message, false); }
  };
}

/* ------------------------------------------------------------- page setup */

function applyTheme() {
  const t = S.prefs.theme || (matchMedia("(prefers-color-scheme: light)").matches ? "light" : "dark");
  document.documentElement.dataset.theme = t;
  S.panels.forEach(p => p.dirty());
}

function bindPage() {
  $("#filter").addEventListener("input", e => { S.filter = e.target.value; renderList(); });
  $("#filter").addEventListener("keydown", e => {
    if (e.key === "ArrowDown") { e.preventDefault(); $("#list").focus(); moveCursor(S.cursor ? 0 : 1); }
    if (e.key === "Enter" && S.order.length) { openPath(S.order[0], e.ctrlKey || e.metaKey); }
    if (e.key === "Escape") { e.target.value = ""; S.filter = ""; renderList(); $("#list").focus(); }
  });
  $("#list").addEventListener("click", e => {
    const row = e.target.closest(".row");
    if (!row) return;
    const add = e.target.classList.contains("c-add");
    openPath(row.dataset.path, add || e.ctrlKey || e.metaKey);
  });
  $("#list").addEventListener("auxclick", e => {
    const row = e.target.closest(".row");
    if (row && e.button === 1) { e.preventDefault(); openPath(row.dataset.path, true); }
  });
  $("#list").addEventListener("keydown", e => {
    if (e.key === "ArrowDown") { e.preventDefault(); moveCursor(1); }
    else if (e.key === "ArrowUp") { e.preventDefault(); moveCursor(-1); }
    else if (e.key === "Enter" && S.cursor) { e.preventDefault(); openPath(S.cursor, true); }
  });
  $$("#listHead span[data-sort]").forEach(s => s.onclick = () => {
    const k = s.dataset.sort;
    S.sort = { key: k, dir: S.sort.key === k ? -S.sort.dir : (k === "rate" ? -1 : 1) };
    renderList(); save();
  });

  // sidebar width
  const side = $("#side"), rz = $("#resizer");
  side.style.width = S.prefs.side + "px";
  rz.addEventListener("pointerdown", e => {
    rz.setPointerCapture(e.pointerId); rz.classList.add("drag");
    const x0 = e.clientX, w0 = side.offsetWidth;
    const move = ev => { side.style.width = clamp(w0 + ev.clientX - x0, 200, innerWidth * 0.7) + "px"; S.panels.forEach(p => p.dirty()); };
    const up = () => { rz.classList.remove("drag"); rz.removeEventListener("pointermove", move); S.prefs.side = side.offsetWidth; save(); };
    rz.addEventListener("pointermove", move);
    rz.addEventListener("pointerup", up, { once: true });
  });

  $("#layout").value = S.prefs.layout;
  $("#layout").onchange = e => { S.prefs.layout = e.target.value; layout(); save(); };
  $("#btnTheme").onclick = () => {
    S.prefs.theme = document.documentElement.dataset.theme === "light" ? "dark" : "light";
    applyTheme(); save();
  };
  $("#btnHelp").onclick = () => $("#dlgHelp").showModal();

  // create
  $("#btnCreate").onclick = () => { $("#cMsg").textContent = ""; $("#dlgCreate").showModal(); };
  $("#dlgCreate").addEventListener("close", async () => {
    if ($("#dlgCreate").returnValue !== "ok") return;
    const shape = $("#cShape").value.split(",").map(x => parseInt(x, 10)).filter(x => x > 0);
    try {
      const r = await request("create", { name: $("#cName").value.trim(), dir: $("#cDir").value, shape, dtype: $("#cType").value });
      toast(r.info, "ok");
      const path = `${$("#cDir").value}/${$("#cName").value.trim()}.im.shm`;
      setTimeout(() => openPath(path, true), 300);
    } catch (e) { toast(e.message, "bad"); }
  });

  // shortcuts
  document.addEventListener("keydown", e => {
    const tag = (e.target.tagName || "").toLowerCase();
    if (tag === "input" || tag === "select" || tag === "textarea" || e.ctrlKey || e.metaKey || e.altKey) return;
    if ($("dialog[open]")) return;
    const p = S.focused;
    const k = e.key;
    if (k === "/") { e.preventDefault(); $("#filter").focus(); $("#filter").select(); }
    else if (k === "?") $("#dlgHelp").showModal();
    else if (k === "i") openInspector(true);
    else if (k === "o" && window.Tools) Tools.open(true);
    else if (!p) return;
    else if (k === " " && e.target.id !== "list") { e.preventDefault(); p.togglePause(); }
    else if (k === "r") { p.view = { zoom: 1, panX: 0, panY: 0 }; p.dirty(); }
    else if (k === "f") p.toggleMax();
    else if (k === "Escape" && p.el.classList.contains("max")) p.toggleMax();
    else if (k === "l") { p.s.range = p.s.range === "lock" ? "minmax" : "lock"; p.locked = p.lastRange || null; p.applySettingsToUi(); p.dirty(); save(); }
    else if (k === "c") { const ks = Object.keys(CMAPS); p.s.cmap = ks[(ks.indexOf(p.s.cmap) + 1) % ks.length]; p.applySettingsToUi(); p.dirty(); save(); }
    else if (k === "t") { const ks = ["off", "trend", "hist"]; p.s.strip = ks[(ks.indexOf(p.s.strip) + 1) % ks.length]; p.applySettingsToUi(); p.dirty(); save(); }
  });
  addEventListener("resize", () => S.panels.forEach(p => p.dirty()));
  matchMedia("(prefers-color-scheme: light)").addEventListener("change", applyTheme);
  bindInspector();
}

function main() {
  const panels = loadPrefs();
  const q = new URLSearchParams(location.search);
  if (q.get("open")) S.openArg = q.get("open").split(",").map(x => x.trim()).filter(Boolean);
  if (q.get("theme")) S.prefs.theme = q.get("theme");
  applyTheme();
  bindPage();
  if (!S.openArg) for (const sp of panels) if (sp && sp.path) addPanel(sp.path, sp.s);
  if (S.panels.length) focusPanel(S.panels[0]);
  connect();
}

main();
