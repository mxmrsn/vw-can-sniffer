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
let mockMode = false;
const labels = new Map();
let labelFilter = '';
const idSelect = document.getElementById('idSelect');
const labelSelect = document.getElementById('labelSelect');
const observedIds = new Set();
const chartData = [];
const chartMaxPoints = 60;
let chartTarget = { id: null, byte: null, labelKey: '', lockedByLabel: false };
let lastObservedSignature = '';
let lastObservedList = [];
let lastLabelSignature = '';

function loadLabels() {
  try {
    const raw = localStorage.getItem('can_labels');
    if (!raw) return;
    const obj = JSON.parse(raw);
    Object.keys(obj).forEach(k => labels.set(k, obj[k]));
  } catch (e) {}
}

function saveLabels() {
  const obj = {};
  labels.forEach((v, k) => { obj[k] = v; });
  localStorage.setItem('can_labels', JSON.stringify(obj));
}

function splitLabelKey(key) {
  const parts = key.split(':');
  return { id: parts[0], byte: parts[1] || '' };
}

function getLabelText(idHex) {
  const general = labels.get(idHex);
  if (general) return general;
  for (const [key, val] of labels.entries()) {
    const { id } = splitLabelKey(key);
    if (id === idHex) return val;
  }
  return '';
}

function renderLabels() {
  const tbody = document.getElementById('labels');
  const rows = Array.from(labels.entries()).sort((a, b) => a[0].localeCompare(b[0]));
  tbody.innerHTML = rows.map(([id, name]) => {
    const { id: keyId, byte } = splitLabelKey(id);
    return `<tr><td>${keyId}</td><td>${byte}</td><td>${name}</td><td><button data-id="${id}">X</button></td></tr>`;
  }).join('');
  tbody.querySelectorAll('button').forEach(btn => {
    btn.addEventListener('click', () => {
      labels.delete(btn.dataset.id);
      saveLabels();
      renderLabels();
      updateDropdowns();
    });
  });
}

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
  tbody.innerHTML = frames.filter(f => {
    const idHex = fmtId(f.id);
    if (filterIds.size) {
      const match = filterIds.has(idHex);
      if (onlyFilter && !match) return false;
    }
    if (!labelFilter) return true;
    const labelText = getLabelText(idHex).toLowerCase();
    return labelText.includes(labelFilter);
  }).map(f => (
    `<tr><td>${f.ts}</td><td>${fmtId(f.id)}${getLabelText(fmtId(f.id)) ? ' ' + getLabelText(fmtId(f.id)) : ''}</td><td>${f.dlc}</td><td>${dataHtml(f.id, f.bytes)}</td></tr>`
  )).join('');
  document.getElementById('stats').textContent =
    `Frames: ${count}  Last ts: ${lastTs}  Rx ok: ${statRx}  CRC bad: ${statBad}  Bytes: ${statBytes}  T rx_ok: ${tRxOk}  T drop: ${tRxDrop}`;

  const modeLabel = devMode === 0 ? 'UART/Wi-Fi' : 'USB';
  const silentLabel = devSilent ? 'ON' : 'OFF';
  document.getElementById('devState').textContent = `Device: mode=${modeLabel}  silent=${silentLabel}`;

  const perIdBody = document.getElementById('perId');
  const rows = Array.from(perId.entries()).sort((a, b) => b[1].rate - a[1].rate).slice(0, 20);
  perIdBody.innerHTML = rows.map(([id, info]) => (
    `<tr><td>${fmtId(id)}${labels.get(fmtId(id)) ? ' ' + labels.get(fmtId(id)) : ''}</td><td>${info.rate.toFixed(1)} Hz</td><td>${info.lastSeenMs} ms</td></tr>`
  )).join('');
  updateChart();
  const observedChanged = updateObservedIds();
  if (observedChanged) updateIdDropdown();
}

function updateObservedIds() {
  const ids = Array.from(perId.keys()).map(fmtId).sort();
  const signature = ids.join(',');
  observedIds.clear();
  ids.forEach(id => observedIds.add(id));
  lastObservedList = ids;
  if (signature === lastObservedSignature) return false;
  lastObservedSignature = signature;
  return true;
}

function clearSelectOptions(select) {
  while (select.firstChild) {
    select.removeChild(select.firstChild);
  }
}

function updateIdDropdown(force = false) {
  if (!idSelect) return;
  if (chartTarget.lockedByLabel && !force) return;
  if (!force && lastObservedSignature === '') return;
  const currentValue = idSelect.value;
  clearSelectOptions(idSelect);
  const autoOpt = document.createElement('option');
  autoOpt.value = '';
  autoOpt.textContent = 'Auto';
  idSelect.appendChild(autoOpt);
  lastObservedList.forEach(id => {
    const opt = document.createElement('option');
    opt.value = id;
    opt.textContent = id;
    if (chartTarget.id === id) opt.selected = true;
    idSelect.appendChild(opt);
  });
  if (currentValue && !observedIds.has(currentValue)) {
    idSelect.value = chartTarget.id || '';
  }
}

function updateLabelDropdown(force = false) {
  if (!labelSelect) return;
  const keys = Array.from(labels.keys()).sort();
  const signature = keys.join(',');
  if (!force && signature === lastLabelSignature) return;
  lastLabelSignature = signature;
  clearSelectOptions(labelSelect);
  const noneOpt = document.createElement('option');
  noneOpt.value = '';
  noneOpt.textContent = 'None';
  labelSelect.appendChild(noneOpt);
  keys.forEach(key => {
    const opt = document.createElement('option');
    opt.value = key;
    const { id, byte } = splitLabelKey(key);
    opt.textContent = `${labels.get(key)} (${id}${byte ? ` byte ${byte}` : ''})`;
    if (chartTarget.labelKey === key) opt.selected = true;
    labelSelect.appendChild(opt);
  });
  if (!keys.includes(chartTarget.labelKey)) labelSelect.value = '';
}

function clearChartData() {
  chartData.length = 0;
}

function selectLabelTarget(key) {
  chartTarget.labelKey = key;
  if (key) {
    const { id, byte } = splitLabelKey(key);
    chartTarget.id = id;
    chartTarget.byte = byte ? parseInt(byte, 10) : 0;
    chartTarget.lockedByLabel = true;
  } else {
    chartTarget.byte = null;
    chartTarget.lockedByLabel = false;
  }
  clearChartData();
}

function getChartSample() {
  const targetId = chartTarget.id;
  let candidate = null;
  if (targetId) {
    candidate = frames.find(f => fmtId(f.id) === targetId);
  }
  if (!candidate) candidate = frames[0];
  if (!candidate || candidate.bytes.length === 0) return null;
  const index = (chartTarget.byte !== null && chartTarget.byte >= 0) ? chartTarget.byte : 0;
  return candidate.bytes[index] ?? candidate.bytes[0];
}

function updateChart() {
  const canvas = document.getElementById('chart');
  if (!canvas) return;
  const ctx = canvas.getContext('2d');
  const value = getChartSample();
  if (value === null) return;
  const timestamp = Date.now();
  chartData.unshift({ ts: timestamp, value });
  if (chartData.length > chartMaxPoints) chartData.pop();
  const width = canvas.width;
  const height = canvas.height;
  ctx.clearRect(0, 0, width, height);
  ctx.strokeStyle = '#1266d4';
  ctx.lineWidth = 2;
  ctx.beginPath();
  chartData.forEach((point, idx) => {
    const x = width - (idx / chartMaxPoints) * width;
    const y = height - (point.value / 255) * height;
    if (idx === 0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  });
  ctx.stroke();
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

// Labels
loadLabels();
renderLabels();
updateLabelDropdown(true);
document.getElementById('addLabelBtn').addEventListener('click', () => {
  const idRaw = document.getElementById('labelId').value.trim().toUpperCase();
  const byteRaw = document.getElementById('labelByte').value.trim();
  const nameRaw = document.getElementById('labelName').value.trim();
  if (!idRaw || !nameRaw) return;
  const id = idRaw.padStart(8, '0');
  const key = byteRaw ? `${id}:${byteRaw}` : id;
  labels.set(key, nameRaw);
  saveLabels();
  renderLabels();
  updateDropdowns();
});

document.getElementById('exportLabelsBtn').addEventListener('click', () => {
  const obj = {};
  labels.forEach((v, k) => { obj[k] = v; });
  const blob = new Blob([JSON.stringify(obj, null, 2)], { type: 'application/json' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = 'can_labels.json';
  a.click();
  URL.revokeObjectURL(url);
});

document.getElementById('importLabelsInput').addEventListener('change', (e) => {
  const file = e.target.files[0];
  if (!file) return;
  const reader = new FileReader();
  reader.onload = () => {
    try {
      const obj = JSON.parse(reader.result);
      labels.clear();
      Object.keys(obj).forEach(k => labels.set(k, obj[k]));
      saveLabels();
      renderLabels();
      updateLabelDropdown(true);
    } catch (err) {}
  };
  reader.readAsText(file);
});

document.getElementById('labelSearch').addEventListener('input', (e) => {
  labelFilter = e.target.value.trim().toLowerCase();
  render();
});

// Mock mode for local UI preview
function startMockMode() {
  if (mockMode) return;
  mockMode = true;
  document.getElementById('status').textContent = 'Mock mode (no hardware)';
  setInterval(() => {
    if (paused) return;
    const now = Date.now();
    const id = 0x100 + (now % 16);
    const bytes = Array.from({ length: 8 }, (_, i) => (now >> (i * 3)) & 0xFF);
    const msg = { ts: now, id, dlc: 8, data: bytes.map(b => b.toString(16).padStart(2, '0').toUpperCase()).join(' ') };
    frames.unshift({ ...msg, bytes });
    if (frames.length > 25) frames.pop();
    count++;
    lastTs = msg.ts;

    const entry = perId.get(id) || { count: 0, rate: 0, lastSeen: now };
    entry.count++;
    entry.lastSeen = now;
    perId.set(id, entry);

    tRxOk += 3;
    tRxDrop += (now % 7 === 0) ? 1 : 0;
    statRx += 5;
    statBytes += 64;
    render();
  }, 200);
}

setTimeout(() => {
  if (!ws || ws.readyState !== 1) startMockMode();
}, 1500);

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
  render();
});

document.getElementById('onlyFilter').addEventListener('change', (e) => {
  onlyFilter = e.target.checked;
  render();
});

if (idSelect) {
  idSelect.addEventListener('change', (e) => {
    const val = e.target.value;
    chartTarget.id = val || null;
    chartTarget.byte = null;
    chartTarget.labelKey = '';
    chartTarget.lockedByLabel = false;
    clearChartData();
    if (labelSelect) labelSelect.value = '';
  });
}

if (labelSelect) {
  labelSelect.addEventListener('change', (e) => {
    selectLabelTarget(e.target.value);
    updateLabelDropdown(true);
    if (idSelect) idSelect.value = '';
  });
}

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
