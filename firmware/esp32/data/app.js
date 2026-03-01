let ws;
let frames = [];
let count = 0;
let lastTs = 0;
let statRx = 0;
let statBad = 0;
let statBytes = 0;
let tRxOk = 0;
let tRxDrop = 0;
let devMode = 0;
let devSilent = 0;
let paused = false;
let filterIds = new Set();
let onlyFilter = false;

const perId = new Map();
const lastData = new Map();

function fmtId(id) {
  return (id >>> 0).toString(16).padStart(8, '0');
}

function parseDataBytes(dataStr) {
  if (!dataStr) return [];
  return dataStr.trim().split(/\s+/).map(x => parseInt(x, 16)).filter(n => !isNaN(n));
}

function dataHtml(id, bytes) {
  const prev = lastData.get(id) || [];
  let html = '';
  for (let i = 0; i < bytes.length; i++) {
    const b = bytes[i];
    const changed = prev[i] !== b;
    html += `<span class="byte${changed ? ' changed' : ''}">${b.toString(16).padStart(2, '0').toUpperCase()}</span> `;
  }
  lastData.set(id, bytes);
  return html.trim();
}

function render() {
  const tbody = document.getElementById('frames');
  tbody.innerHTML = frames.map(f => (
    `<tr><td>${f.ts}</td><td>${fmtId(f.id)}</td><td>${f.dlc}</td><td>${dataHtml(f.id, f.bytes)}</td></tr>`
  )).join('');
  document.getElementById('stats').textContent =
    `Frames: ${count}  Last ts: ${lastTs}  Rx ok: ${statRx}  CRC bad: ${statBad}  Bytes: ${statBytes}  T rx_ok: ${tRxOk}  T drop: ${tRxDrop}`;

  const modeLabel = devMode === 0 ? 'UART/Wi-Fi' : 'USB';
  const silentLabel = devSilent ? 'ON' : 'OFF';
  document.getElementById('devState').textContent = `Device: mode=${modeLabel}  silent=${silentLabel}`;

  const perIdBody = document.getElementById('perId');
  const rows = Array.from(perId.entries()).sort((a, b) => b[1].rate - a[1].rate).slice(0, 20);
  perIdBody.innerHTML = rows.map(([id, info]) => (
    `<tr><td>${fmtId(id)}</td><td>${info.rate.toFixed(1)} Hz</td><td>${info.lastSeenMs} ms</td></tr>`
  )).join('');
}

function connect() {
  const host = window.location.hostname;
  ws = new WebSocket(`ws://${host}:81`);

  ws.onopen = () => {
    document.getElementById('status').textContent = 'Connected';
  };
  ws.onclose = () => {
    document.getElementById('status').textContent = 'Disconnected; retrying...';
    setTimeout(connect, 1000);
  };
  ws.onmessage = (ev) => {
    try {
      const msg = JSON.parse(ev.data);
      if (msg.type === 'stat') {
        if (!paused) {
          statRx = msg.rx || 0;
          statBad = msg.bad || 0;
          statBytes = msg.bytes || 0;
        }
      } else if (msg.type === 'tstat') {
        if (!paused) {
          tRxOk = msg.rx_ok || 0;
          tRxDrop = msg.rx_drop || 0;
        }
      } else if (msg.type === 'state') {
        devMode = msg.mode || 0;
        devSilent = msg.silent || 0;
      } else {
        if (paused) return;

        const id = msg.id >>> 0;
        const idHex = fmtId(id);
        if (filterIds.size > 0) {
          const match = filterIds.has(idHex);
          if (onlyFilter && !match) return;
        }

        const bytes = parseDataBytes(msg.data);
        frames.unshift({ ...msg, bytes });
        if (frames.length > 25) frames.pop();
        count++;
        lastTs = msg.ts;

        const now = Date.now();
        const entry = perId.get(id) || { count: 0, rate: 0, lastSeen: now };
        entry.count++;
        entry.lastSeen = now;
        perId.set(id, entry);
      }
      render();
    } catch (e) {}
  };
}

connect();

// Controls
document.getElementById('pauseBtn').addEventListener('click', () => {
  paused = !paused;
  document.getElementById('pauseBtn').textContent = paused ? 'Resume' : 'Pause';
});

document.getElementById('modeBtn').addEventListener('click', () => {
  if (!ws || ws.readyState !== 1) return;
  const next = devMode === 0 ? 1 : 0;
  ws.send(JSON.stringify({ cmd: 'set_mode', value: next }));
});

document.getElementById('silentBtn').addEventListener('click', () => {
  if (!ws || ws.readyState !== 1) return;
  const next = devSilent ? 0 : 1;
  ws.send(JSON.stringify({ cmd: 'set_silent', value: next }));
});

document.getElementById('filterInput').addEventListener('input', (e) => {
  const raw = e.target.value.toUpperCase();
  filterIds = new Set(raw.split(/[^0-9A-F]+/).filter(Boolean).map(x => x.padStart(8, '0')));
});

document.getElementById('onlyFilter').addEventListener('change', (e) => {
  onlyFilter = e.target.checked;
});

// Per-ID rate calculation
setInterval(() => {
  const now = Date.now();
  perId.forEach((v) => {
    const dt = (now - v.lastSeen) / 1000;
    if (dt > 2) {
      v.rate = v.rate * 0.5;
    } else {
      v.rate = v.count;
    }
    v.count = 0;
    v.lastSeenMs = Math.floor(now - v.lastSeen);
  });
  if (!paused) render();
}, 1000);
