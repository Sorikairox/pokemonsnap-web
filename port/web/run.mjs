// Pokémon Snap port: run the game in the host's headless Chrome and take screenshots (session S3c).
//   node port/web/run.mjs --out port_work/DIR [--rom ./pokemonsnap.z64] [--frames 600] [--shots 60,120,...]
//        [--every N] [--tap N MASK] [--input F:MASK[:LEN],...] [--logops F,...] [--pace fast|raf] [--timeout MS] [--chrome PATH]
//        [--no-readback] [--tap-end F]   (--input entries: F:MASK[:LEN[:X[:Y]]], X/Y the stick, -128..127)
//        [--audio] [--play]   (S7: OUT/audio.wav, the PCM the host drained; --play plays it in the page, headless
//        Chrome stays muted)
//        [--save FILE]   (S8: boot with that FlashRAM image; the game's writes are dumped to OUT/flash.fla)
//        [--scene N]     (S10a debug: start scene N instead of the main menu, include/enums.h; port/boot/main.c)
//        [--option ID=VALUE ...]   (F1: port options, port/ultra/pc_options.c; e.g. --option 1=0 turns the Snap button
//        off, F2: --option 2=1 presents Sharp pixels. Input masks may use bit 16 (0x10000), the Snap button, and F4:
//        bit 17 (0x20000), the Shutter: a tap is one whole picture, e.g. --input 4000:0x20000:3)
//        [--motion F:YAW:PITCH[:LEN],...]   (F3: motion aim, as the retrowser host's rtw_motion: from retrace F, for
//        LEN retraces (default 1), a rotation of YAW / PITCH (1/1024 degree, yaw > 0 right, pitch > 0 up) before each)
// Serves port/web/{index.html,worker.js}, port/build/web/pokemonsnap.{js,wasm} and the player's ROM (/rom, read from
// --rom: never copied, never served from the repo) on 127.0.0.1, starts Chrome headless (muted, SwiftShader, its own
// temporary profile) and drives it over the DevTools protocol (the port/gfx/test/cdp.mjs method: node's own WebSocket,
// no packages). The worker POSTs the canvas after the listed retraces (OUT/fNNNNN.png: ROM-derived, keep them in
// port_work/) and its log (OUT/log.txt, with the page's console and exceptions). --every N adds a shot every N
// retraces. Input is frame-indexed, so with --pace fast two runs give the same pictures. Kills only the Chrome it
// started. Exit code 0 when the worker reported done. OUT/readback.bin (unless --no-readback; ROM-derived) is the
// read-back journal: `port/docker/run.sh port-test --readback OUT/readback.bin` with the same --frames/--tap/--input
// follows this run's game state in node (S6).
import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { mkdirSync, mkdtempSync, readFileSync, writeFileSync, appendFileSync, rmSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const args = process.argv.slice(2);
const opt = (name, def) => { const i = args.indexOf('--' + name); return i >= 0 ? args[i + 1] : def; };
const out = resolve(opt('out', join(ROOT, 'port_work/web')));
const romPath = resolve(opt('rom', join(ROOT, 'pokemonsnap.z64')));
const frames = +opt('frames', 600);
const every = +opt('every', 0);
const shots = new Set((opt('shots', '') || '').split(',').filter(Boolean).map(Number));
if (every > 0) for (let f = every; f <= frames; f += every) shots.add(f);
shots.add(frames);
const tapI = args.indexOf('--tap');
const tap = tapI >= 0 ? [+args[tapI + 1], Number(args[tapI + 2])] : null;
const input = (opt('input', '') || '').split(',').filter(Boolean).map((s) => s.split(':').map(Number));
const motion = (opt('motion', '') || '').split(',').filter(Boolean).map((s) => s.split(':').map(Number));   // F3
const logops = (opt('logops', '') || '').split(',').filter(Boolean).map(Number);
const cfg = { frames, shots: [...shots].sort((a, b) => a - b), tap, input, motion, logops, pace: opt('pace', 'fast'), args: [],
  readback: !args.includes('--no-readback'), tapEnd: +opt('tap-end', 0),
  audio: args.includes('--audio'), play: args.includes('--play') };
if (opt('scene', null) !== null) cfg.args.push('--scene', String(+opt('scene')));
// F1: --option ID=VALUE (repeatable): a port option set before main, as the retrowser host's rtw_option does
cfg.options = args.flatMap((a, i) => (a === '--option' && args[i + 1] ? [args[i + 1].split('=').map(Number)] : []));
const saveIn = opt('save', null) ? resolve(opt('save')) : null;
if (saveIn && !existsSync(saveIn)) { console.error(`no save image at ${saveIn}`); process.exit(2); }
cfg.save = !!saveIn;
const timeout = +opt('timeout', 600000);
const chrome = opt('chrome', '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome');
if (!existsSync(romPath)) { console.error(`no ROM at ${romPath} (port/docker/run.sh rom PATH writes ./pokemonsnap.z64)`); process.exit(2); }
mkdirSync(out, { recursive: true });
writeFileSync(join(out, 'log.txt'), '');
writeFileSync(join(out, 'config.json'), JSON.stringify(cfg) + '\n');
const log = (s) => appendFileSync(join(out, 'log.txt'), s.endsWith('\n') ? s : s + '\n');

// SNAP_WEB_BUILD: another harness build directory (e.g. a baseline copy for an equivalence run; F5)
const BUILD = resolve(process.env.SNAP_WEB_BUILD || join(ROOT, 'port/build/web'));
const files = {
  '/': [join(ROOT, 'port/web/index.html'), 'text/html'],
  '/index.html': [join(ROOT, 'port/web/index.html'), 'text/html'],
  '/worker.js': [join(ROOT, 'port/web/worker.js'), 'text/javascript'],
  '/pokemonsnap.js': [join(BUILD, 'pokemonsnap.js'), 'text/javascript'],
  '/pokemonsnap.wasm': [join(BUILD, 'pokemonsnap.wasm'), 'application/wasm'],
  '/rom': [romPath, 'application/octet-stream'],
};
if (saveIn) files['/save'] = [saveIn, 'application/octet-stream'];
let finished, nshots = 0;
const doneP = new Promise((r) => (finished = r));
const server = createServer((req, res) => {
  const url = new URL(req.url, 'http://x');
  if (req.method === 'GET') {
    if (url.pathname === '/config') { res.writeHead(200, { 'content-type': 'application/json' }); res.end(JSON.stringify(cfg)); return; }
    const f = files[url.pathname];
    if (!f) { res.writeHead(404); res.end(); return; }
    res.writeHead(200, { 'content-type': f[1], 'cache-control': 'no-store' });
    res.end(readFileSync(f[0]));
    return;
  }
  const chunks = [];
  req.on('data', (c) => chunks.push(c));
  req.on('end', () => {
    const body = Buffer.concat(chunks);
    if (url.pathname === '/shot') {
      const name = (url.searchParams.get('name') || 'shot.png').replace(/[^\w.-]/g, '_');
      writeFileSync(join(out, name), body);
      nshots++;
    } else if (url.pathname === '/log') log(body.toString());
    else if (url.pathname === '/save') {   // S8: the FlashRAM image after a write (console byte order, 128 KiB)
      writeFileSync(join(out, 'flash.fla'), body);
      log(`[harness] flash image generation ${url.searchParams.get('gen')} -> flash.fla`);
    } else if (url.pathname === '/readback') writeFileSync(join(out, 'readback.bin'), body);
    else if (url.pathname === '/audio') writeFileSync(join(out, 'audio.wav'), body);
    else if (url.pathname === '/done') finished(true);
    res.writeHead(200); res.end();
  });
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const httpPort = server.address().port;

const prof = mkdtempSync(join(tmpdir(), 'snapweb-'));
const ch = spawn(chrome, ['--headless=new', '--mute-audio', `--user-data-dir=${prof}`, '--use-angle=swiftshader',
  '--enable-unsafe-swiftshader', '--window-size=700,540', '--remote-debugging-port=0', '--no-first-run',
  '--disable-background-timer-throttling', '--disable-renderer-backgrounding', 'about:blank'], { stdio: 'ignore' });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let exitCode = 2;
const finish = async () => {
  try { ch.kill(); } catch {}
  server.close();
  await sleep(500);
  try { rmSync(prof, { recursive: true, force: true }); } catch {}
  process.exit(exitCode);
};

let devPort;
for (let i = 0; i < 100 && !devPort; i++) {
  await sleep(100);
  try { devPort = readFileSync(join(prof, 'DevToolsActivePort'), 'utf8').split('\n')[0]; } catch {}
}
let targets;
for (let i = 0; i < 50 && !targets && devPort; i++) {
  try { targets = await (await fetch(`http://127.0.0.1:${devPort}/json`)).json(); } catch { await sleep(200); }
}
const page = targets && targets.find((t) => t.type === 'page');
if (!page) { console.error('no Chrome page target'); await finish(); }
const ws = new WebSocket(page.webSocketDebuggerUrl);
await new Promise((r) => ws.addEventListener('open', r));
let id = 0;
const pending = new Map();
ws.addEventListener('message', (m) => {
  const msg = JSON.parse(m.data);
  if (msg.id && pending.has(msg.id)) { pending.get(msg.id)(msg); pending.delete(msg.id); return; }
  if (msg.method === 'Runtime.consoleAPICalled')
    log('[console] ' + msg.params.args.map((a) => a.value ?? a.description ?? '').join(' '));
  else if (msg.method === 'Runtime.exceptionThrown')
    log('[exception] ' + (msg.params.exceptionDetails.exception?.description || msg.params.exceptionDetails.text));
  else if (msg.method === 'Target.attachedToTarget') log(`[cdp] attached ${msg.params.targetInfo.type}`);
});
const send = (method, params = {}) => new Promise((r) => { const i = ++id; pending.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
await send('Runtime.enable');
await send('Page.enable');
const t0 = Date.now();
await send('Page.navigate', { url: `http://127.0.0.1:${httpPort}/` });
const ok = await Promise.race([doneP, sleep(timeout).then(() => false)]);
const shot = await send('Page.captureScreenshot', { format: 'png' });
if (shot.result) writeFileSync(join(out, 'page.png'), Buffer.from(shot.result.data, 'base64'));
console.log(`${ok ? 'done' : 'TIMEOUT'} after ${Date.now() - t0} ms: ${nshots} screenshots in ${out}`);
exitCode = ok ? 0 : 2;
await finish();
