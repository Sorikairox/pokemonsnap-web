// port/gfx test driver: headless Chrome over the DevTools protocol (node >= 22: global WebSocket and fetch, no
// packages). Waits for the page's document.title === 'DONE' in real time, then writes OUT/log.txt, OUT/shown.png
// (the canvas as presented), OUT/rdram.png (the frame from RDRAM) and OUT/page.png (a screenshot).
//   node cdp.mjs CHROME URL OUT_DIR [timeout_ms]
import { spawn } from 'node:child_process';
import { mkdtempSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const [chrome, url, out, tmo = '60000'] = process.argv.slice(2);
const prof = mkdtempSync(join(tmpdir(), 'gfxtest-'));
const port = 9300 + (process.pid % 500);
const ch = spawn(chrome, ['--headless=new', '--mute-audio', `--user-data-dir=${prof}`, '--use-angle=swiftshader',
  '--enable-unsafe-swiftshader', '--window-size=1320,1000', `--remote-debugging-port=${port}`, 'about:blank'],
  { stdio: 'ignore' });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const done = (code) => { try { ch.kill(); } catch {} ; setTimeout(() => { try { rmSync(prof, { recursive: true, force: true }); } catch {} ; process.exit(code); }, 300); };

let targets;
for (let i = 0; i < 50 && !targets; i++) {
  await sleep(200);
  try { targets = await (await fetch(`http://127.0.0.1:${port}/json`)).json(); } catch {}
}
const page = targets && targets.find((t) => t.type === 'page');
if (!page) { console.error('no Chrome page target'); done(1); }
const ws = new WebSocket(page.webSocketDebuggerUrl);
await new Promise((r) => ws.addEventListener('open', r));
let id = 0;
const pending = new Map();
ws.addEventListener('message', (m) => {
  const msg = JSON.parse(m.data);
  if (msg.id && pending.has(msg.id)) { pending.get(msg.id)(msg); pending.delete(msg.id); }
});
const send = (method, params = {}) => new Promise((r) => { const i = ++id; pending.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
const evaluate = async (expr) => (await send('Runtime.evaluate', { expression: expr, returnByValue: true })).result?.result?.value;

await send('Page.enable');
await send('Page.navigate', { url });
const t0 = Date.now();
let title = '';
while (Date.now() - t0 < +tmo) {
  await sleep(250);
  title = await evaluate('document.title');
  if (title === 'DONE') break;
}
const log = (await evaluate("document.getElementById('log').textContent")) || '(no log)';
writeFileSync(join(out, 'log.txt'), log);
for (const name of ['shown', 'rdram', 'shown2', 'rdram2']) {
  const d = await evaluate(`document.getElementById('png_${name}').textContent`);
  if (d && d.startsWith('data:image/png;base64,')) writeFileSync(join(out, `${name}.png`), Buffer.from(d.slice(22), 'base64'));
  else console.log(`no ${name}.png`);
}
const shot = await send('Page.captureScreenshot', { format: 'png' });
if (shot.result) writeFileSync(join(out, 'page.png'), Buffer.from(shot.result.data, 'base64'));
console.log(log);
console.log(title === 'DONE' ? `page done after ${Date.now() - t0} ms` : 'TIMEOUT waiting for the page');
done(title === 'DONE' ? 0 : 2);
