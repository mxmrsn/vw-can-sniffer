let ws;
let frames = [];
let count = 0;
let lastTs = 0;

function fmtId(id) {
  return (id >>> 0).toString(16).padStart(8, '0');
}

function render() {
  const tbody = document.getElementById('frames');
  tbody.innerHTML = frames.map(f => (
    `<tr><td>${f.ts}</td><td>${fmtId(f.id)}</td><td>${f.dlc}</td><td>${f.data}</td></tr>`
  )).join('');
  document.getElementById('stats').textContent = `Frames: ${count}  Last ts: ${lastTs}`;
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
      frames.unshift(msg);
      if (frames.length > 25) frames.pop();
      count++;
      lastTs = msg.ts;
      render();
    } catch (e) {}
  };
}

connect();
