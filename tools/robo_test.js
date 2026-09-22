// Link test for the robot. Join WiFi "Robo-CAM", then run:  node tools/robo_test.js
// Runs ~25 s, only sends "stop" commands (the robot does not move), and writes
// robo_test_report.txt next to this repo's README.
const net = require('net');
const http = require('http');
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');

const HOST = process.argv[2] || '192.168.4.1';
const SECONDS = 25;
const lines = [];
const log = (s) => { console.log(s); lines.push(s); };

function get(port, p, timeoutMs) {
  return new Promise((resolve) => {
    const t0 = Date.now();
    const req = http.get({ host: HOST, port, path: p, agent: keepAlive[port] }, (res) => {
      const chunks = [];
      res.on('data', (c) => chunks.push(c));
      res.on('end', () => resolve({ ok: res.statusCode === 200, ms: Date.now() - t0, body: Buffer.concat(chunks) }));
    });
    req.setTimeout(timeoutMs, () => req.destroy(new Error('timeout')));
    req.on('error', (e) => resolve({ ok: false, ms: Date.now() - t0, err: e.message }));
  });
}
const keepAlive = { 80: new http.Agent({ keepAlive: true, maxSockets: 1 }) };

function stats(a) {
  if (!a.length) return 'none';
  const s = [...a].sort((x, y) => x - y);
  const avg = Math.round(s.reduce((x, y) => x + y, 0) / s.length);
  return `min ${s[0]} / avg ${avg} / p95 ${s[Math.floor(s.length * 0.95)]} / max ${s[s.length - 1]} ms`;
}

// Minimal WebSocket client, enough for the robot's /ws: text frames are command
// echoes, binary frames are pictures. Asks for a picture ("img") every 5 s.
function wsTest(durationMs) {
  return new Promise((resolve) => {
    const r = { rtts: [], sent: 0, lost: 0, disconnects: 0, connectFails: 0,
                pics: 0, picBytes: 0, picWaits: [] };
    let imgAskedAt = null, lastImgAsk = 0;
    const end = Date.now() + durationMs;
    let pending = null, timer = null, sock = null;

    function frame(text) {
      const payload = Buffer.from(text);
      const mask = crypto.randomBytes(4);
      const f = Buffer.alloc(6 + payload.length);
      f[0] = 0x81; f[1] = 0x80 | payload.length; mask.copy(f, 2);
      for (let i = 0; i < payload.length; i++) f[6 + i] = payload[i] ^ mask[i % 4];
      return f;
    }
    function connect() {
      if (Date.now() > end) return finish();
      let buf = Buffer.alloc(0), open = false;
      sock = net.connect(80, HOST);
      sock.setNoDelay(true);
      sock.setTimeout(3000, () => sock.destroy());
      sock.on('connect', () => {
        const key = crypto.randomBytes(16).toString('base64');
        sock.write(`GET /ws HTTP/1.1\r\nHost: ${HOST}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n` +
                   `Sec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`);
      });
      sock.on('data', (d) => {
        buf = Buffer.concat([buf, d]);
        if (!open) {
          const i = buf.indexOf('\r\n\r\n');
          if (i < 0) return;
          if (!buf.slice(0, i).toString().includes(' 101 ')) { sock.destroy(); return; }
          open = true; buf = buf.slice(i + 4);
          sock.setTimeout(0);
          tick();
        }
        while (buf.length >= 2) {
          const op = buf[0] & 0x0f;
          let len = buf[1] & 0x7f, hdr = 2;
          if (len === 126) { if (buf.length < 4) break; len = buf.readUInt16BE(2); hdr = 4; }
          else if (len === 127) { if (buf.length < 10) break; len = Number(buf.readBigUInt64BE(2)); hdr = 10; }
          if (buf.length < hdr + len) break;
          const payload = buf.slice(hdr, hdr + len);
          buf = buf.slice(hdr + len);
          if (op === 2) {  // picture
            r.pics++; r.picBytes += len;
            if (imgAskedAt !== null) { r.picWaits.push(Date.now() - imgAskedAt); imgAskedAt = null; }
          } else if (op === 1 && payload.toString() !== 'img' && pending !== null) {
            r.rtts.push(Date.now() - pending); pending = null;
          }
        }
      });
      sock.on('error', () => {});
      sock.on('close', () => {
        clearTimeout(timer);
        if (open) r.disconnects++; else r.connectFails++;
        if (Date.now() < end) setTimeout(connect, 300); else finish();
      });
    }
    function tick() {
      if (Date.now() > end) { sock.end(); return; }
      if (pending !== null && Date.now() - pending > 1000) { r.lost++; pending = null; }
      if (pending === null) { pending = Date.now(); r.sent++; sock.write(frame('s')); }
      if (Date.now() - lastImgAsk >= 5000) { lastImgAsk = imgAskedAt = Date.now(); sock.write(frame('img')); }
      timer = setTimeout(tick, 200);
    }
    let done = false;
    function finish() { if (!done) { done = true; resolve(r); } }
    setTimeout(finish, durationMs + 4000);
    connect();
  });
}

(async () => {
  log(`Robo link test  ${new Date().toISOString()}  host ${HOST}`);
  const info0 = await get(80, '/info', 3000);
  log('Robot info (start): ' + (info0.ok ? info0.body.toString() : 'NO ANSWER ' + (info0.err || '')));
  if (!info0.ok) log('Is this computer connected to the "Robo-CAM" WiFi?');

  log(`\nRunning drive link + pictures for ${SECONDS} s (asking for a picture every 5 s) ...`);
  const ws = await wsTest(SECONDS * 1000);
  log(`Drive link: sent ${ws.sent}, echoed ${ws.rtts.length}, lost ${ws.lost}, disconnects ${ws.disconnects}, failed connects ${ws.connectFails}`);
  log(`Drive latency: ${stats(ws.rtts)}`);
  log(`Pictures: ${ws.pics} received, avg size ${ws.pics ? Math.round(ws.picBytes / ws.pics) : 0} bytes`);
  log(`Picture delivery after asking: ${stats(ws.picWaits)}`);

  const info1 = await get(80, '/info', 3000);
  log('\nRobot info (end): ' + (info1.ok ? info1.body.toString() : 'NO ANSWER ' + (info1.err || '')));

  const out = path.join(__dirname, '..', 'robo_test_report.txt');
  fs.writeFileSync(out, lines.join('\n') + '\n');
  console.log(`\nReport saved to ${out}`);
  process.exit(0);
})();
