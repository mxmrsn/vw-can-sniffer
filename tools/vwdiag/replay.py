"""Drive replay: one log file as a self-contained HTML page.

Engine values (the OBD answers in the log), GPS speed and the GPS track, and the marks, on one time
axis. Each measure gets its own small chart (never two scales on one chart); a cursor moved over any
chart, or the time slider, moves through all of them and the map at once. The page needs no network:
the track is drawn as a plain outline, without map tiles, so opening it sends your position nowhere.

    python3 -m vwdiag replay LOG00003.VWL --out drive.html
"""
import json
import time

from . import obd

MAX_POINTS = 4000   # per series; longer series are thinned evenly (the table keeps every row it shows)


def _thin(points):
    if len(points) <= MAX_POINTS:
        return points
    step = len(points) / MAX_POINTS
    return [points[int(i * step)] for i in range(MAX_POINTS)]


def collect(log):
    """What the page shows, as plain data (also what the tests look at)."""
    series = {}
    for t, name, value, unit in log.samples():
        series.setdefault(name, {"name": name, "unit": unit, "points": []})["points"].append((round(t, 3), value))
    fixes = [f for f in log.fixes if f.valid]
    if fixes:
        series["gps_speed"] = {"name": "gps_speed", "unit": "km/h",
                               "points": [(round(log.seconds(f.ms), 3), round(f.speed_ms * 3.6, 2)) for f in fixes]}
    # Engine speed and road speed first, then the rest in the OBD list's order.
    # Measuring values follow, in the order they first appear.
    order = ["rpm", "speed", "gps_speed"] + [n for n in obd.PIDS if n not in ("rpm", "speed")]
    order += [n for n in series if n not in order]
    out = sorted(series.values(), key=lambda s: order.index(s["name"]) if s["name"] in order else len(order))
    for s in out:
        s["points"] = _thin(s["points"])
    track = _thin([(round(log.seconds(f.ms), 3), round(f.lat, 7), round(f.lon, 7)) for f in fixes])
    marks = [(round(log.seconds(ms), 3), text) for ms, text in log.marks]
    end = max([log.duration] + [s["points"][-1][0] for s in out if s["points"]] + [m[0] for m in marks] +
              [p[0] for p in track])
    start = log.epoch(log.start_ms)
    return {"series": out, "track": track, "marks": marks, "duration": round(end, 3),
            "start_utc": time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime(start)) if start else None,
            "frames": len(log.frames), "lost": log.lost, "truncated": log.truncated,
            "drift_ppm": log.clock_drift_ppm()}


PAGE = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Drive replay</title>
<style>
.viz-root {
  color-scheme: light;
  --surface-1: #fcfcfb; --surface-2: #f3f2ef; --border: #dcdbd6;
  --text-primary: #0b0b0b; --text-secondary: #52514e; --text-muted: #77766f;
  --series-1: #2a78d6; --cursor: #0b0b0b; --grid: #e7e6e2;
}
@media (prefers-color-scheme: dark) {
  :root:where(:not([data-theme="light"])) .viz-root {
    color-scheme: dark;
    --surface-1: #1a1a19; --surface-2: #232321; --border: #3a3a37;
    --text-primary: #f0efec; --text-secondary: #c3c2b7; --text-muted: #9a998f;
    --series-1: #3987e5; --cursor: #f0efec; --grid: #2e2e2b;
  }
}
:root[data-theme="dark"] .viz-root {
  color-scheme: dark;
  --surface-1: #1a1a19; --surface-2: #232321; --border: #3a3a37;
  --text-primary: #f0efec; --text-secondary: #c3c2b7; --text-muted: #9a998f;
  --series-1: #3987e5; --cursor: #f0efec; --grid: #2e2e2b;
}
html, body { margin: 0; }
body.viz-root { background: var(--surface-1); color: var(--text-primary);
  font: 14px/1.4 -apple-system, "Helvetica Neue", Arial, sans-serif; }
main { max-width: 1100px; margin: 0 auto; padding: 16px; }
h1 { font-size: 20px; margin: 0 0 4px; }
.sub { color: var(--text-secondary); margin: 0 0 12px; }
.readout { position: sticky; top: 0; z-index: 2; background: var(--surface-2); border: 1px solid var(--border);
  border-radius: 8px; padding: 8px 12px; margin-bottom: 12px; display: flex; flex-wrap: wrap; gap: 4px 18px; }
.readout .v { font-weight: 600; font-variant-numeric: tabular-nums; }
.readout .k { color: var(--text-secondary); margin-left: 4px; }
.slider { width: 100%; margin: 4px 0 12px; }
.layout { display: grid; grid-template-columns: minmax(0, 2fr) minmax(0, 1fr); gap: 16px; }
@media (max-width: 760px) { .layout { grid-template-columns: minmax(0, 1fr); } }
.chart h2, .map h2 { font-size: 13px; font-weight: 600; margin: 0; color: var(--text-primary); }
.chart h2 span, .map h2 span { color: var(--text-secondary); font-weight: 400; }
.chart { margin-bottom: 10px; }
svg { display: block; width: 100%; touch-action: none; }
svg text { fill: var(--text-muted); font-size: 11px; font-variant-numeric: tabular-nums; }
.line { fill: none; stroke: var(--series-1); stroke-width: 2; stroke-linejoin: round; }
.grid { stroke: var(--grid); stroke-width: 1; }
.hair { stroke: var(--cursor); stroke-width: 1; }
.mark { stroke: var(--text-muted); stroke-width: 1; stroke-dasharray: 3 3; }
.dot { fill: var(--series-1); stroke: var(--surface-1); stroke-width: 2; }
.map svg { border: 1px solid var(--border); border-radius: 8px; background: var(--surface-2); }
.note { color: var(--text-secondary); font-size: 12px; }
details { margin-top: 16px; }
table { border-collapse: collapse; font-size: 12px; font-variant-numeric: tabular-nums; }
th, td { border-bottom: 1px solid var(--border); padding: 3px 8px; text-align: right; }
th:first-child, td:first-child { text-align: left; }
.empty { color: var(--text-secondary); padding: 24px 0; }
</style>
</head>
<body class="viz-root">
<main>
<h1>Drive replay</h1>
<p class="sub" id="sub"></p>
<div class="readout" id="readout" aria-live="polite"></div>
<label class="note" for="slider">Time</label>
<input class="slider" id="slider" type="range" min="0" max="1000" value="0">
<div class="layout">
  <div id="charts"></div>
  <div class="map" id="mapbox"></div>
</div>
<details><summary>Data table</summary><div id="table"></div></details>
</main>
<script type="application/json" id="data">__DATA__</script>
<script>
(function () {
  const D = JSON.parse(document.getElementById("data").textContent);
  const NS = "http://www.w3.org/2000/svg";
  const H = 120, PAD = { l: 44, r: 8, t: 8, b: 20 };
  const el = (tag, attrs, parent) => {
    const e = document.createElementNS(NS, tag);
    for (const k in attrs) e.setAttribute(k, attrs[k]);
    if (parent) parent.appendChild(e);
    return e;
  };
  const fmt = v => Math.abs(v) >= 100 ? v.toFixed(0) : Math.abs(v) >= 10 ? v.toFixed(1) : v.toFixed(2);
  const fmtT = t => { const m = Math.floor(t / 60); return m + ":" + (t - m * 60).toFixed(1).padStart(4, "0"); };
  const label = s => s.name === "gps_speed" ? "GPS speed" : s.name.replace(/_/g, " ");
  const nearest = (pts, t) => {          // index of the sample closest in time
    let lo = 0, hi = pts.length - 1;
    while (hi - lo > 1) { const mid = (lo + hi) >> 1; if (pts[mid][0] < t) lo = mid; else hi = mid; }
    return Math.abs(pts[lo][0] - t) <= Math.abs(pts[hi][0] - t) ? lo : hi;
  };
  const dur = Math.max(D.duration, 0.001);

  const sub = document.getElementById("sub");
  const bits = [D.start_utc ? "Started " + D.start_utc : "Start time unknown (the interface was not told the time)",
                "length " + fmtT(D.duration), D.frames + " CAN frames"];
  if (D.lost) bits.push(D.lost + " frames lost");
  if (D.truncated) bits.push("file ends mid-record (power removed)");
  if (D.drift_ppm !== null) bits.push("clock " + (D.drift_ppm >= 0 ? "+" : "") + D.drift_ppm.toFixed(2) + " ppm against GPS");
  sub.textContent = bits.join(" \\u00b7 ");

  const charts = [];
  const box = document.getElementById("charts");
  if (!D.series.length) {
    const p = document.createElement("p");
    p.className = "empty";
    p.textContent = "No engine values or GPS speed in this log. Log with a poll list (vwdiag log start --poll rpm speed ...) to record them.";
    box.appendChild(p);
  }
  for (const s of D.series) {
    const wrap = document.createElement("div");
    wrap.className = "chart";
    const h = document.createElement("h2");
    h.textContent = label(s) + " ";
    const u = document.createElement("span");
    u.textContent = s.unit;
    h.appendChild(u);
    wrap.appendChild(h);
    const svg = el("svg", { role: "img", "aria-label": label(s) + " against time" }, wrap);
    box.appendChild(wrap);
    charts.push({ s, svg });
  }

  const mapbox = document.getElementById("mapbox");
  let map = null;
  if (D.track.length > 1) {
    const h = document.createElement("h2");
    h.textContent = "GPS track ";
    const u = document.createElement("span");
    u.textContent = "north up, no map tiles";
    h.appendChild(u);
    mapbox.appendChild(h);
    map = { svg: el("svg", { role: "img", "aria-label": "GPS track" }, mapbox) };
  }

  let cursor = 0, W = 600;
  function xOf(t) { return PAD.l + (W - PAD.l - PAD.r) * t / dur; }
  function tOf(x) { return Math.min(dur, Math.max(0, (x - PAD.l) / (W - PAD.l - PAD.r) * dur)); }

  function drawChart(c) {
    const svg = c.svg, pts = c.s.points;
    svg.textContent = "";
    W = svg.clientWidth || 600;
    svg.setAttribute("viewBox", "0 0 " + W + " " + H);
    svg.setAttribute("height", H);
    let lo = Infinity, hi = -Infinity;
    for (const p of pts) { lo = Math.min(lo, p[1]); hi = Math.max(hi, p[1]); }
    if (lo === hi) { lo -= 1; hi += 1; }
    const pad = (hi - lo) * 0.08; lo -= pad; hi += pad;
    const yOf = v => PAD.t + (H - PAD.t - PAD.b) * (1 - (v - lo) / (hi - lo));
    for (const v of [lo + pad, (lo + hi) / 2, hi - pad]) {
      el("line", { class: "grid", x1: PAD.l, x2: W - PAD.r, y1: yOf(v), y2: yOf(v) }, svg);
      el("text", { x: PAD.l - 6, y: yOf(v) + 4, "text-anchor": "end" }, svg).textContent = fmt(v);
    }
    el("text", { x: PAD.l, y: H - 4 }, svg).textContent = "0:00.0";
    el("text", { x: W - PAD.r, y: H - 4, "text-anchor": "end" }, svg).textContent = fmtT(dur);
    for (const m of D.marks) el("line", { class: "mark", x1: xOf(m[0]), x2: xOf(m[0]), y1: PAD.t, y2: H - PAD.b }, svg);
    el("path", { class: "line", d: pts.map((p, i) => (i ? "L" : "M") + xOf(p[0]).toFixed(1) + " " + yOf(p[1]).toFixed(1)).join("") }, svg);
    c.hair = el("line", { class: "hair", y1: PAD.t, y2: H - PAD.b }, svg);
    c.dot = el("circle", { class: "dot", r: 4 }, svg);
    c.yOf = yOf;
    svg.onpointerdown = svg.onpointermove = e => { const r = svg.getBoundingClientRect(); setCursor(tOf((e.clientX - r.left) * W / r.width)); };
  }

  function drawMap() {
    if (!map) return;
    const svg = map.svg;
    svg.textContent = "";
    const MW = svg.clientWidth || 300, MH = Math.max(220, Math.min(420, MW));
    svg.setAttribute("viewBox", "0 0 " + MW + " " + MH);
    svg.setAttribute("height", MH);
    const lat0 = D.track[0][1] * Math.PI / 180;
    const xs = D.track.map(p => p[2] * Math.cos(lat0)), ys = D.track.map(p => p[1]);
    let x0 = Math.min(...xs), x1 = Math.max(...xs), y0 = Math.min(...ys), y1 = Math.max(...ys);
    const span = Math.max(x1 - x0, y1 - y0, 1e-5), m = 16;
    const k = Math.min((MW - 2 * m) / span, (MH - 2 * m) / span);
    const cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    map.px = i => MW / 2 + (xs[i] - cx) * k;
    map.py = i => MH / 2 - (ys[i] - cy) * k;
    el("path", { class: "line", d: D.track.map((p, i) => (i ? "L" : "M") + map.px(i).toFixed(1) + " " + map.py(i).toFixed(1)).join("") }, svg);
    const metres = span * 111320;
    el("text", { x: 8, y: MH - 8 }, svg).textContent = "across: " + (metres >= 1000 ? (metres / 1000).toFixed(2) + " km" : metres.toFixed(0) + " m");
    map.dot = el("circle", { class: "dot", r: 6 }, svg);
    svg.onpointerdown = svg.onpointermove = e => {
      const r = svg.getBoundingClientRect(), x = (e.clientX - r.left) * MW / r.width, y = (e.clientY - r.top) * MH / r.height;
      let best = 0, bd = Infinity;
      D.track.forEach((p, i) => { const d = (map.px(i) - x) ** 2 + (map.py(i) - y) ** 2; if (d < bd) { bd = d; best = i; } });
      setCursor(D.track[best][0]);
    };
  }

  const readout = document.getElementById("readout"), slider = document.getElementById("slider");
  function item(value, key) {
    const span = document.createElement("span");
    const v = document.createElement("span"); v.className = "v"; v.textContent = value;
    const k = document.createElement("span"); k.className = "k"; k.textContent = key;
    span.appendChild(v); span.appendChild(k);
    readout.appendChild(span);
  }
  function setCursor(t) {
    cursor = t;
    slider.value = Math.round(t / dur * 1000);
    readout.textContent = "";
    item(fmtT(t), "time");
    for (const c of charts) {
      const p = c.s.points[nearest(c.s.points, t)];
      item(fmt(p[1]) + " " + c.s.unit, label(c.s));
      if (c.hair) { c.hair.setAttribute("x1", xOf(t)); c.hair.setAttribute("x2", xOf(t)); }
      if (c.dot) { c.dot.setAttribute("cx", xOf(p[0])); c.dot.setAttribute("cy", c.yOf(p[1])); }
    }
    if (map && map.dot) {
      const i = nearest(D.track, t);
      map.dot.setAttribute("cx", map.px(i)); map.dot.setAttribute("cy", map.py(i));
      item(D.track[i][1].toFixed(5) + ", " + D.track[i][2].toFixed(5), "position");
    }
    let mark = null;
    for (const m of D.marks) if (m[0] <= t) mark = m;
    if (mark) item(mark[1], "last mark (" + fmtT(mark[0]) + ")");
  }
  slider.oninput = () => setCursor(slider.value / 1000 * dur);

  function drawAll() { for (const c of charts) drawChart(c); drawMap(); setCursor(cursor); }
  window.addEventListener("resize", drawAll);
  drawAll();

  // The table view: every series at the same moments (one row per second, or 200 rows at most).
  const rows = Math.min(200, Math.max(2, Math.ceil(dur) + 1));
  const table = document.createElement("table");
  const head = table.createTHead().insertRow();
  const th = t => { const c = document.createElement("th"); c.textContent = t; head.appendChild(c); };
  th("time");
  for (const c of charts) th(label(c.s) + " (" + c.s.unit + ")");
  if (D.track.length) { th("latitude"); th("longitude"); }
  const body = table.createTBody();
  for (let r = 0; r < rows; r++) {
    const t = dur * r / (rows - 1), row = body.insertRow();
    row.insertCell().textContent = fmtT(t);
    for (const c of charts) row.insertCell().textContent = fmt(c.s.points[nearest(c.s.points, t)][1]);
    if (D.track.length) {
      const p = D.track[nearest(D.track, t)];
      row.insertCell().textContent = p[1].toFixed(6); row.insertCell().textContent = p[2].toFixed(6);
    }
  }
  if (D.marks.length) {
    const p = document.createElement("p");
    p.className = "note";
    p.textContent = "Marks: " + D.marks.map(m => fmtT(m[0]) + " " + m[1]).join("; ");
    document.getElementById("table").appendChild(p);
  }
  document.getElementById("table").appendChild(table);
})();
</script>
</body>
</html>
"""


def render(log):
    data = json.dumps(collect(log), separators=(",", ":"))
    # Inside a script element only "</" can end it early; marks are free text.
    return PAGE.replace("__DATA__", data.replace("</", "<\\/"))

