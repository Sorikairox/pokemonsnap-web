// End-to-end test of the retrowser build (session S9, docs/port/RTW.md) in a LOCAL retrowser host: the built host
// (RTW_REPO/dist, tools/serve-worker.ts: the real Worker fetch handler, service workers on) serves the game from its
// GitHub mock (tools/mock-github.ts), one muted headless Chrome (playwright-core from the retrowser checkout) plays.
//   node port/rtw/e2e.mjs [build dir] [out dir]       (defaults port/build/rtw/dist, port_work/s9/e2e)
// Env: RTW_REPO (default ~/Code/retrowser), ROM (default "./Pokemon Snap (USA).n64": the player's file, byte-swapped
// .n64, converted by the host), SAVE (a FlashRAM image for the import / continue phase, default
// port_work/merge_all/flash.fla), CHROME_PATH, PHASES (default "desk,touch,options,wrong,offline").
// Phases: desk   boot from the ROM picker (remembered), input, audio, pacing, pause, save import -> Continue ->
//                Oak's lab -> Save (the host stores the new image; snap_save.py checks it), reload + continue;
//         touch  phone portrait 390x844 and landscape 844x390 (isMobile, hasTouch): pictures, touch A reaches the pad;
//                the Snap extra button (F1) is shown and reaches the pad as bit 16, the Shutter (F4) as bit 17;
//         options  (F1) the host passes option 1 at start, the game's Options menu changes it (rtw_option_changed),
//                the host remembers it across a reload; the Snap key (Digit1) reaches the pad;
//                (F2) option 2 "Pixels" likewise (Smooth -> Sharp), and Sharp is visible: the canvas takes the view's
//                size and the picture's neighbouring pixels are equal far more often than with Smooth;
//                (F5) option 3 "Motion Aim" -> On likewise; the Snap touch button ("when": 1) goes at once when
//                Snap Button is set Off (the host's touch layout forced on for the first visit);
//         menu   (F5, not in the default list) the Options menu on a phone (dpr 3) in portrait and landscape, each
//                port row chosen and edited: pictures to look at;
//         motionopt  (F5, not in the default list; a host that knows controls.motion { when }, e.g.
//                RTW_REPO=~/Code/retrowser-wt-motionopt, and a build whose shipped manifest keeps the object form)
//                no ☰ Motion choice, no motion while option 3 is Off, the host's Allow prompt when the game's menu
//                turns it On, rtw_motion after the tap, the sensors stopped when Off again;
//         mouse  (F6, not in the default list) desktop 1024x768: option 4 "Mouse Aim" Off then On again in the game's
//                Options menu (reported, passed back after a reload), no pointer lock asked outside a course; the SAVE
//                continued to the Beach: the module asks for the mouse in camera control, the host's click captures
//                it (the game does not see that click), left clicks take the tutorial's pictures ("[snap] photo taken
//                ... (mouse click)"), mouse motion turns the view by 0.08 degree a pixel (the "[mouse] movement
//                stopped" line, +-5 %), the right button held zooms and a left click then takes a picture, a middle
//                click throws an apple, the pause menu gives the mouse back (and camera control asks again), Esc
//                releases it and the module does not ask again by itself;
//         wrong  a ROM with one byte changed: the host's wrong-ROM warning, then the module's plain refusal;
//         offline  every server stopped, the network off: reload, boot from the caches and the remembered ROM.
//         beach  (F2, not in the default list) phone portrait 390x844 at dpr 3, the SAVE continued to the Beach with
//                Pixels Smooth and then Sharp (the host's stored option): screenshots to compare by eye.
//         motion  (F3 + F4, not in the default list; a host with ☰ -> Motion "Off / Motion aim" and Full-screen
//                landscape, e.g. RTW_REPO=~/Code/retrowser-wt-motion; before offline) phone landscape 844x390: the
//                host's ☰ opened by a touch tap in both layouts, Full-screen landscape and Motion aim from its menu,
//                the SAVE continued to the Beach's camera control; a simulated phone (rates + gravity, so the host's
//                calibration locks) turns the camera by the turn's angle (+-5 %, the module's "[motion] rotation
//                stopped" line, pictures) at 90 and 270, nothing when Off; a tap on the Shutter button takes exactly
//                one picture (also with a finger on the stick); then a fresh page with WebKit's rotationRate labels:
//                the calibration locks on them and the turn is still +90.
// Screenshots and the stored save go to the out dir (game pictures: local only, never commit). Every server and
// Chrome it starts is stopped at the end. Prints PASS / FAIL lines.
import { createRequire } from 'node:module';
import { execFileSync } from 'node:child_process';
import { inflateSync } from 'node:zlib';
import { existsSync, mkdirSync, readFileSync, writeFileSync, rmSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const buildDir = resolve(process.argv[2] || join(ROOT, 'port/build/rtw/dist'));
const outDir = resolve(process.argv[3] || join(ROOT, 'port_work/s9/e2e'));
const RTW = process.env.RTW_REPO || join(process.env.HOME, 'Code/retrowser');
const ROM = resolve(process.env.ROM || join(ROOT, 'Pokemon Snap (USA).n64'));
const SAVE = resolve(process.env.SAVE || join(ROOT, 'port_work/merge_all/flash.fla'));
const PHASES = (process.env.PHASES || 'desk,touch,options,wrong,offline').split(',');
const chromePath = process.env.CHROME_PATH || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
mkdirSync(outDir, { recursive: true });
for (const f of [join(buildDir, 'retrowser.json'), ROM]) if (!existsSync(f)) { console.error(`missing ${f}`); process.exit(2); }

const { startMockGithub } = await import(pathToFileURL(join(RTW, 'tools/mock-github.ts')).href);
const { serveWorker } = await import(pathToFileURL(join(RTW, 'tools/serve-worker.ts')).href);
const { chromium } = createRequire(join(RTW, 'package.json'))('playwright-core');

const results = [];
const check = (name, ok, detail = '') => { results.push(!!ok); console.log(`${ok ? 'PASS' : 'FAIL'} ${name}${detail ? ': ' + detail : ''}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const COMMIT = '5ea95a9c0ffee5ea95a9c0ffee5ea95a9c0ffee5';
const REPO = 'rtw-test/pokemonsnap';
let mock = await startMockGithub({ [REPO]: { refs: { main: COMMIT }, commits: [{ sha: COMMIT, dir: buildDir }] } });
const HPORT = 8788 + 21;
let host = await serveWorker(HPORT);
const site = `http://localhost:${HPORT}`;
console.log(`host on ${site}, mock ${mock.base}, build ${buildDir}`);
const stopHost = async () => {
  if (host) { await new Promise((ok) => { host.closeAllConnections?.(); host.close(() => ok()); }); host = null; }
  if (mock) { await mock.close().catch(() => {}); mock = null; }
};

const launchArgs = ['--mute-audio', '--enable-unsafe-swiftshader', '--use-angle=swiftshader', '--autoplay-policy=no-user-gesture-required'];
const browser = await chromium.launch({ executablePath: chromePath, headless: true, args: launchArgs });
const endpoints = JSON.stringify(mock.endpoints);
async function newCtx(opts = {}) {
  const ctx = await browser.newContext({ viewport: { width: 1024, height: 768 }, reducedMotion: 'reduce', ...opts });
  await ctx.addInitScript((ep) => { try { if (location.hostname === 'localhost') localStorage.setItem('rtw:test-endpoints', ep); } catch { /* */ } }, endpoints);
  return ctx;
}

// the host's data-log keeps the last 200 lines: collect the lines appended since the previous poll (the overlap of
// the old tail and the new head), so a line logged again by a restarted module counts again
function logger(page) {
  const seen = [];
  const all = [];   // every line since the start (reset() does not clear it)
  let prev = [];
  const poll = async () => {
    const l = (await page.getAttribute('[data-state=playing]', 'data-log').catch(() => null));
    if (l == null) return;
    const cur = l ? l.split('\n') : [];
    let k = Math.min(prev.length, cur.length);
    for (; k > 0; k--) {
      let ok = true;
      for (let i = 0; i < k && ok; i++) ok = prev[prev.length - k + i] === cur[i];
      if (ok) break;
    }
    for (const line of cur.slice(k)) { seen.push(line); all.push(line); }
    prev = cur;
  };
  const timer = setInterval(() => { poll().catch(() => {}); }, 200);
  return {
    async text() { await poll(); return seen.join('\n'); },
    async allText() { await poll(); return all.join('\n'); },
    async wait(re, secs) {
      const t0 = Date.now();
      while (Date.now() - t0 < secs * 1000) {
        await poll();
        const m = seen.find((x) => re.test(x));
        if (m) return m;
        if (await page.$('.play .error')) return null;
        await sleep(300);
      }
      return null;
    },
    async reset() { await poll(); seen.length = 0; },
    stop() { clearInterval(timer); },
  };
}
let shotN = 0;
async function shot(page, name, full = false) {
  const f = join(outDir, `${String(shotN++).padStart(2, '0')}-${name}.png`);
  if (full) await page.screenshot({ path: f }).catch((e) => console.log('shot failed ' + e.message));
  else await page.locator('[data-field=screen]').screenshot({ path: f }).catch((e) => console.log('shot failed ' + e.message));
  console.log(`  shot ${f}`);
  return f;
}
const press = async (page, key, ms = 120) => { await page.keyboard.down(key); await sleep(ms); await page.keyboard.up(key); };
async function openGame(page, { stored = false, rom = ROM, remember = true } = {}) {
  await page.goto(`${site}/g/${REPO}`);
  await page.click('[data-action=cartridge]', { timeout: 120000 });
  await page.waitForSelector('[data-state=playing], [data-state=rom]', { timeout: 180000 });
  if (await page.$('[data-state=playing]')) return 'started at once';
  if (stored && await page.$('[data-action=use-stored]')) { await page.click('[data-action=use-stored]'); return 'remembered ROM'; }
  const r = await page.$('[data-field=remember]');
  if (r) { if (remember) await r.check(); else await r.uncheck(); }
  await page.setInputFiles('[data-field=rom]', rom);
  return 'ROM picked';
}
async function audioState(page) { return JSON.parse((await page.getAttribute('[data-state=playing]', 'data-audio').catch(() => null)) ?? '{}'); }
async function storedSave(page) {
  return page.evaluate(async () => {
    const db = await new Promise((ok, bad) => { const r = indexedDB.open('retrowser'); r.onsuccess = () => ok(r.result); r.onerror = () => bad(r.error); });
    const keys = await new Promise((ok) => { const r = db.transaction('saves').objectStore('saves').getAllKeys(); r.onsuccess = () => ok(r.result); r.onerror = () => ok([]); });
    const k = keys.find((x) => String(x).startsWith('pokemonsnap-us'));
    const v = k ? await new Promise((ok) => { const r = db.transaction('saves').objectStore('saves').get(k); r.onsuccess = () => ok(r.result); r.onerror = () => ok(null); }) : null;
    db.close();
    if (!v) return { keys };
    const b = v.bytes instanceof Uint8Array ? v.bytes : new Uint8Array(v.bytes);
    let s = '';
    for (let i = 0; i < b.length; i++) s += String.fromCharCode(b[i]);
    return { key: k, n: b.length, b64: btoa(s), keys };
  });
}
function snapCheck(file) {
  try { return execFileSync('python3', ['-I', join(ROOT, 'port/save/snap_save.py'), file], { encoding: 'utf8' }); } catch (e) { return String(e.stdout || e); }
}
const errors = [];

// F2: decode a PNG screenshot (8-bit RGB / RGBA, not interlaced: what Chrome writes) and measure how often a pixel
// equals its right-hand neighbour exactly. A nearest-neighbour picture is made of flat runs (each N64 pixel 2-4 screen
// pixels wide); a linearly filtered one differs a little almost everywhere.
function pngPixels(file) {
  const b = readFileSync(file);
  let i = 8, w = 0, h = 0, ct = 0;
  const idat = [];
  while (i < b.length) {
    const n = b.readUInt32BE(i), t = b.toString('ascii', i + 4, i + 8), d = b.subarray(i + 8, i + 8 + n);
    if (t === 'IHDR') { w = d.readUInt32BE(0); h = d.readUInt32BE(4); ct = d[9]; if (d[8] !== 8 || d[12]) throw new Error('png format'); }
    if (t === 'IDAT') idat.push(d);
    i += 12 + n;
  }
  const bpp = ct === 6 ? 4 : 3, stride = w * bpp, raw = inflateSync(Buffer.concat(idat)), px = Buffer.alloc(h * stride);
  for (let y = 0; y < h; y++) {
    const f = raw[y * (stride + 1)], src = raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1)), o = y * stride;
    for (let x = 0; x < stride; x++) {
      const a = x >= bpp ? px[o + x - bpp] : 0, up = y ? px[o - stride + x] : 0, c = x >= bpp && y ? px[o - stride + x - bpp] : 0;
      let v = src[x];
      if (f === 1) v += a; else if (f === 2) v += up; else if (f === 3) v += (a + up) >> 1;
      else if (f === 4) { const p = a + up - c, pa = Math.abs(p - a), pb = Math.abs(p - up), pc = Math.abs(p - c); v += pa <= pb && pa <= pc ? a : pb <= pc ? up : c; }
      px[o + x] = v & 255;
    }
  }
  return { w, h, bpp, px };
}
function flatness(file) {   // share of horizontal neighbour pairs with the same colour (non-black pixels only)
  const { w, h, bpp, px } = pngPixels(file);
  let same = 0, n = 0;
  for (let y = 0; y < h; y++) for (let x = 0; x + 1 < w; x++) {
    const o = (y * w + x) * bpp;
    if (px[o] + px[o + 1] + px[o + 2] < 24) continue;   // the letterbox
    n++;
    if (px[o] === px[o + bpp] && px[o + 1] === px[o + bpp + 1] && px[o + 2] === px[o + bpp + 2]) same++;
  }
  return n ? same / n : 0;
}

// Pacing (rtw_glue.js; guide section 3, traps 13 and 14). The line covers a window of 600 retraces:
//   "rtw: pacing N retraces: R retraces/s, D display frames/s, timer wake-ups T/s (K ran a retrace), L late"
// What must hold, whatever the display does:
//  - the game runs at the console's rate: R = 60 (+-2);
//  - no idle timer chain: every wake-up runs a catch-up retrace (wake-ups - K <= 3 + 2 %; the rates are rounded);
//  - the display frames pace the game: most retraces are started by one (600 - K >= 60 % of the display frames).
// The number of timer retraces K is NOT a fixed limit: it is the display deficit (60 - D per second) plus every
// display frame that came more than half a field late (its retrace already ran on the timer). Headless Chrome's
// frame times are irregular under SwiftShader: the same build measured K = 28, 107 and 193 per window (D 57.0, 49.3,
// 56.6), all with every wake-up running a retrace. A timer-paced loop (trap 13) has 600 - K near 0; an idle chain
// (trap 14) has wake-ups >> K.
function pacingVerdict(line) {
  const m = line && line.match(/([\d.]+) retraces\/s, ([\d.]+) display frames\/s, timer wake-ups ([\d.]+)\/s \((\d+) ran a retrace\)/);
  if (!m) return { ok: false, detail: line ?? 'no pacing line' };
  const [R, D, T, K] = [+m[1], +m[2], +m[3], +m[4]];
  const secs = 600 / R, woke = T * secs, frames = D * secs, byDisplay = 600 - K;
  const idle = woke - K, deficit = Math.max(0, (R - D) * secs);
  const rateOk = Math.abs(R - 60) <= 2;
  const idleOk = idle <= 3 + 0.02 * woke;
  const displayOk = byDisplay >= 0.6 * Math.min(frames, 600);
  const detail = `${line.replace(/^.*rtw: pacing /, '')} -> ${secs.toFixed(1)} s: ${Math.round(woke)} wake-ups, ` +
    `${Math.round(idle)} idle${idleOk ? '' : ' (IDLE CHAIN)'}; ${byDisplay} retraces started by ${Math.round(frames)} display ` +
    `frames${displayOk ? '' : ' (NOT DISPLAY-PACED)'}; timer retraces ${K} = deficit ${Math.round(deficit)} + ` +
    `${Math.max(0, Math.round(K - deficit))} late display frames${rateOk ? '' : '; RATE OFF'}`;
  return { ok: rateOk && idleOk && displayOk, detail };
}

async function desk() {
  const ctx = await newCtx();
  const page = await ctx.newPage();
  page.on('pageerror', (e) => errors.push(e.message));
  const L = logger(page);
  try {
    console.log('desk: ' + await openGame(page));
    await page.waitForSelector('[data-state=playing]', { timeout: 180000 });
    const filled = await L.wait(/rtw: template filled/, 60);
    check('boot: template filled from the ROM, hash OK', filled && /\(OK\)/.test(filled), filled ?? 'no line');
    const st = await L.wait(/rtw: game started/, 60);
    check('boot: game started (no stored save)', st && /no stored save/.test(st), st ?? '');
    const err = await page.$('.play .error');
    if (err) check('no game error', false, await err.textContent());
    await sleep(4000); await shot(page, 'intro');
    const a0 = await audioState(page);
    // intro -> title: A (KeyX) every 2 s, then Start
    for (let i = 0; i < 6; i++) { await press(page, 'KeyX'); await sleep(2000); }
    await shot(page, 'title');
    await press(page, 'Enter'); await sleep(1500);
    await page.keyboard.down('ArrowDown'); await sleep(200); await page.keyboard.up('ArrowDown'); await sleep(1500);
    await shot(page, 'title-input');
    const lt = await L.text();
    check('input: A, Start and the stick reached pad 0', /pad0 buttons 8000/.test(lt) && /pad0 buttons 1000/.test(lt) && /stick 0,-80/.test(lt));
    const a1 = await audioState(page);
    check('audio: the host drains and plays', (a1.played ?? 0) > (a0.played ?? 0) || (a1.queuedMs ?? 0) > 0, JSON.stringify(a1));
    const pace = await L.wait(/rtw: pacing/, 30);
    const pv = pacingVerdict(pace);
    check('pacing: 60 retraces/s, started by display frames, every timer wake-up a catch-up retrace', pv.ok, pv.detail);
    // pause: the host's pause button -> rtw_pause(1): no retraces until resumed
    await page.click('[data-action=pause]'); await sleep(2500);
    await page.click('[data-action=pause]'); await sleep(500);
    const lp = await L.text();
    const pm = lp.match(/rtw: paused at retrace (\d+)/), rm = lp.match(/rtw: resumed at retrace (\d+)/);
    check('pause: no retraces while paused', pm && rm && pm[1] === rm[1], `${pm?.[0]} / ${rm?.[0]}`);
    // save import (the host restarts the game with it): Continue -> Oak's lab -> Save
    if (existsSync(SAVE)) {
      await page.click('[data-action=menu]'); await sleep(500);
      await L.reset();
      await page.setInputFiles('[data-field=import-save]', SAVE);
      const st2 = await L.wait(/rtw: game started/, 90);
      check('save import: the game restarts with the image before main()', st2 && /save from the host/.test(st2) && /flash image from the host \(131072 bytes at the flash array\)/.test(await L.text()), st2 ?? '');
      if (await page.$('[data-action=close-menu]')) await page.click('[data-action=close-menu]', { timeout: 5000 }).catch(() => {});
      await sleep(3000);
      for (let i = 0; i < 5; i++) { await press(page, 'KeyX'); await sleep(2000); }
      await sleep(3000);
      await shot(page, 'continue-lab');   // Oak: "I have been waiting for you, ..." (the dialogue waits for A)
      await press(page, 'KeyX'); await sleep(2500);
      await shot(page, 'lab-menu');
      for (let i = 0; i < 3; i++) { await press(page, 'ArrowDown', 120); await sleep(1000); }
      await shot(page, 'lab-menu-save');
      await press(page, 'KeyX'); await sleep(2000); await shot(page, 'save-ask');
      const before = await storedSave(page);
      await press(page, 'KeyX');
      const sv = await L.wait(/\[save\] retrace \d+: flash generation/, 30);
      await sleep(3000); await shot(page, 'saved');
      const after = await storedSave(page);
      let ok = false, detail = sv ?? 'no flash write';
      if (after.b64) {
        const f = join(outDir, 'stored.fla');
        writeFileSync(f, Buffer.from(after.b64, 'base64'));
        const rep = snapCheck(f);
        ok = after.n === 131072 && /version_ok: True/.test(rep) && /md4_ok: True/.test(rep) && after.b64 !== before.b64;
        detail += `; host key ${after.key}, ${after.n} bytes, ${/md4_ok: True/.test(rep) ? 'MD4 OK' : 'MD4 bad'}, ${after.b64 !== before.b64 ? 'changed' : 'unchanged'}`;
      }
      check('save: the game wrote the flash, the host stored a valid native image', sv && ok, detail);
      // reload: the stored save is handed back before main()
      await L.reset();
      await page.reload();
      console.log('reload: ' + await openGame(page, { stored: true }));
      await page.waitForSelector('[data-state=playing]', { timeout: 180000 });
      const st3 = await L.wait(/rtw: game started/, 90);
      check('reload: started with the save from the host', st3 && /save from the host/.test(st3), st3 ?? '');
      await sleep(3000);
      for (let i = 0; i < 5; i++) { await press(page, 'KeyX'); await sleep(2000); }
      await shot(page, 'reload-continue');
    } else console.log(`no save image at ${SAVE}: import / continue skipped`);
    writeFileSync(join(outDir, 'log-desk.txt'), await L.text());
  } finally { L.stop(); await ctx.close(); }
}

async function title() {   // quick look: boot, A every 2 s to the name card
  const ctx = await newCtx();
  const page = await ctx.newPage();
  const L = logger(page);
  try {
    await openGame(page, { remember: false });
    await page.waitForSelector('[data-state=playing]', { timeout: 180000 });
    check('title: boot', !!(await L.wait(/rtw: game started/, 90)));
    await sleep(4000);
    for (let i = 0; i < 6; i++) { await press(page, 'KeyX'); await sleep(2000); }
    await shot(page, 'title');
  } finally { L.stop(); await ctx.close(); }
}

async function touch() {
  for (const [name, w, h] of [['portrait', 390, 844], ['landscape', 844, 390]]) {
    const ctx = await newCtx({ viewport: { width: w, height: h }, deviceScaleFactor: 3, isMobile: true, hasTouch: true });
    const page = await ctx.newPage();
    page.on('pageerror', (e) => errors.push(e.message));
    const L = logger(page);
    try {
      await openGame(page, { remember: false });
      await page.waitForSelector('[data-state=playing]', { timeout: 180000 });
      const st = await L.wait(/rtw: game started/, 90);
      check(`touch ${name}: boot`, !!st);
      await sleep(12000);
      await shot(page, `touch-${name}`, true);
      const a = page.locator('[data-touch=A]').first();
      const b = await a.boundingBox();
      if (b) {
        const cdp = await ctx.newCDPSession(page);
        const pt = [{ x: b.x + b.width / 2, y: b.y + b.height / 2, id: 7, radiusX: 2, radiusY: 2, force: 1 }];
        // up to three taps: the first touch of a fresh page was sometimes not seen as a press (1 of 4 runs)
        for (let i = 0; i < 3 && !/pad0 buttons 8000/.test(await L.text()); i++) {
          if (i) console.log(`  touch ${name}: tap ${i + 1}`);
          await cdp.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: pt });
          await sleep(150);
          if (!i) await shot(page, `touch-${name}-A-down`, true);
          await cdp.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
          await sleep(800);
        }
      }
      check(`touch ${name}: the A button reached pad 0`, b && /pad0 buttons 8000/.test(await L.text()), b ? '' : 'no [data-touch=A]');
      // F1 / F4: the manifest's extra buttons "Snap" (bit 16) and "Shutter" (bit 17) are on the layout, inside the view,
      // and reach pad 0 as their bit (the host's default layout draws "Shutter" as an ordinary extra button)
      for (const [bit, name2, hex, id] of [[16, 'Snap', '10000', 8], [17, 'Shutter', '20000', 9]]) {
        const sel = `[data-touch=x${bit}]`, re = new RegExp(`pad0 buttons ${hex}`);
        const sb = await page.locator(sel).first().boundingBox().catch(() => null);
        const label = sb ? (await page.locator(sel).first().textContent()) : '';
        check(`touch ${name}: the ${name2} button is shown`, sb && sb.width >= 20 && sb.height >= 20 && sb.x >= 0 && sb.y >= 0 &&
          sb.x + sb.width <= w && sb.y + sb.height <= h && new RegExp(name2).test(label), sb ? `${label} ${Math.round(sb.x)},${Math.round(sb.y)} ${Math.round(sb.width)}x${Math.round(sb.height)}` : `no ${sel}`);
        if (!sb) continue;
        const cdp = await ctx.newCDPSession(page);
        const pt = [{ x: sb.x + sb.width / 2, y: sb.y + sb.height / 2, id, radiusX: 2, radiusY: 2, force: 1 }];
        for (let i = 0; i < 3 && !re.test(await L.text()); i++) {
          await cdp.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: pt });
          await sleep(150);
          if (!i) await shot(page, `touch-${name}-${name2}-down`, true);
          await cdp.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
          await sleep(800);
        }
        check(`touch ${name}: the ${name2} button reached pad 0 (bit ${bit})`, re.test(await L.text()));
      }
      const boxes = await page.evaluate(() => [...document.querySelectorAll('[data-touch], [data-field=screen]')].map((e) => {
        const r = e.getBoundingClientRect(); return `${e.dataset.touch ?? 'screen'} ${Math.round(r.x)},${Math.round(r.y)} ${Math.round(r.width)}x${Math.round(r.height)}`;
      }));
      writeFileSync(join(outDir, `touch-${name}-boxes.txt`), boxes.join('\n') + '\n');
      writeFileSync(join(outDir, `log-touch-${name}.txt`), await L.text());
    } finally { L.stop(); await ctx.close(); }
  }
}

// F1: option 1 "Snap button" (inGame): the host passes its value at start (rtw_option), the game's Options menu
// changes it (rtw_option_changed), the host remembers it across a reload; the Snap key (Digit1) reaches pad 0.
async function options() {
  const ctx = await newCtx();
  // F5: the host's touch layout shown on this desktop page too, so the Snap button (`when`: 1) can be seen to go
  // (until the reload: the Sharp check below compares the desktop picture)
  await ctx.addInitScript(() => { try { if (!sessionStorage.getItem('f5-touch-done')) localStorage.setItem('rtw:touch', JSON.stringify('on')); } catch { /* */ } });
  const page = await ctx.newPage();
  page.on('pageerror', (e) => errors.push(e.message));
  const L = logger(page);
  // boot -> Start (skips the intro to "PRESS START") -> Start (title menu; no save: New Game / Options) -> Options.
  // A presses through the intro (as desk does) can land on the menu and pick New Game: Start only reaches the menu.
  const toOptions = async (tag) => {
    await sleep(3000);
    await press(page, 'Enter', 200); await sleep(5000);
    await press(page, 'Enter', 200); await sleep(2500);
    await press(page, 'ArrowDown'); await sleep(1500);
    await press(page, 'KeyX'); await sleep(3500);
    menuShots.push(await shot(page, `options-${tag}`));
  };
  const menuShots = [];
  try {
    await openGame(page, { remember: true });
    await page.waitForSelector('[data-state=playing]', { timeout: 180000 });
    await L.wait(/rtw: game started/, 90);
    const t0 = await L.text();
    check('options: the host passes the default at start (rtw_option 1 = 1)', /rtw: option 1 = 1 from the host/.test(t0),
      (t0.match(/rtw: option.*/) || ['no rtw_option line'])[0]);
    await press(page, 'Digit1', 200); await sleep(500);
    check('options: the Snap key (Digit1) reaches pad 0 as bit 16', /pad0 buttons 10000/.test(await L.text()));
    await toOptions('menu');
    for (let i = 0; i < 4; i++) { await press(page, 'ArrowDown'); await sleep(1000); }   // rows 1, 2, 3, Snap Button
    await shot(page, 'options-snap-row');
    await press(page, 'KeyX'); await sleep(1200);
    await press(page, 'ArrowRight'); await sleep(1200);
    await shot(page, 'options-snap-off');
    await press(page, 'KeyX'); await sleep(1200);
    // F2: the Pixels row (below Snap Button): Smooth -> Sharp, previewed at once like the Sound row
    await press(page, 'ArrowDown'); await sleep(1000);
    await shot(page, 'options-pixels-row');
    await press(page, 'KeyX'); await sleep(1200);
    await press(page, 'ArrowRight'); await sleep(1500);
    await shot(page, 'options-pixels-sharp');
    await press(page, 'KeyX'); await sleep(1200);
    // F5: the Motion Aim row (below Pixels): Off -> On (On is the left value)
    await press(page, 'ArrowDown'); await sleep(1000);
    await shot(page, 'options-motion-row');
    await press(page, 'KeyX'); await sleep(1200);
    await press(page, 'ArrowLeft'); await sleep(1200);
    await shot(page, 'options-motion-on');
    await press(page, 'KeyX'); await sleep(1200);
    const snapShown = async () => { const b = await page.locator('[data-touch=x16]').first().boundingBox().catch(() => null); return !!b && b.width > 0; };
    const snapBefore = await snapShown();
    await press(page, 'ArrowDown'); await sleep(1000);    // F6: Mouse Aim (left as it is)
    await press(page, 'ArrowDown'); await sleep(1000);    // Return
    await press(page, 'KeyX');
    const ch = await L.wait(/rtw: option 1 = 0 changed in the game's Options menu/, 20);
    check('options: the game\'s menu reports the change (rtw_option_changed 1 = 0)', !!ch, ch ?? 'no line');
    const ch2 = await L.wait(/rtw: option 2 = 1 changed in the game's Options menu/, 20);
    check('options: Pixels -> Sharp is reported (rtw_option_changed 2 = 1)', !!ch2, ch2 ?? 'no line');
    const ch3 = await L.wait(/rtw: option 3 = 1 changed in the game's Options menu/, 20);
    check('options: Motion Aim -> On is reported (rtw_option_changed 3 = 1)', !!ch3, ch3 ?? 'no line');
    // F5: the Snap extra button has "when": 1: the host takes it off its touch layout at once (no reload)
    await sleep(500);
    const snapAfter = await snapShown();
    check('options: the Snap touch button goes at once when Snap Button is Off', snapBefore && !snapAfter,
      `shown before ${snapBefore}, after ${snapAfter}; Shutter still shown ${!!(await page.locator('[data-touch=x17]').first().boundingBox().catch(() => null))}`);
    await shot(page, 'options-snap-gone', true);
    await page.evaluate(() => { sessionStorage.setItem('f5-touch-done', '1'); localStorage.setItem('rtw:touch', JSON.stringify('auto')); });
    await sleep(2000);
    await L.reset();
    await page.reload();
    console.log('options reload: ' + await openGame(page, { stored: true }));
    await page.waitForSelector('[data-state=playing]', { timeout: 180000 });
    await L.wait(/rtw: game started/, 90);
    const t1 = await L.text();
    check('options: after a reload the host passes the remembered value (rtw_option 1 = 0)', /rtw: option 1 = 0 from the host/.test(t1),
      (t1.match(/rtw: option.*/) || ['no rtw_option line'])[0]);
    check('options: after a reload the host passes Pixels Sharp (rtw_option 2 = 1)', /rtw: option 2 = 1 from the host/.test(t1),
      (t1.match(/rtw: option 2.*/) || ['no rtw_option 2 line'])[0]);
    check('options: after a reload the host passes Motion Aim On (rtw_option 3 = 1)', /rtw: option 3 = 1 from the host/.test(t1),
      (t1.match(/rtw: option 3.*/) || ['no rtw_option 3 line'])[0]);

    await toOptions('menu-reload');   // the rows show Off and Sharp
    // Sharp visible: the canvas has the view's size (not the Smooth 4:3 one) and the same menu screen is made of flat
    // runs of pixels (Smooth: options-menu, the first visit; Sharp: options-menu-reload)
    const t2 = await L.text();
    const cv = [...t2.matchAll(/rtw: canvas (\d+)x(\d+)/g)].map((m) => [+m[1], +m[2]]);
    // the last size the glue logged; none after the reload when the host's first canvas already had the view's size
    // (F5: the first visit runs with the touch layout): then the canvas element itself
    let last = cv[cv.length - 1];
    if (!last) {
      for (const f of page.frames()) {
        const wh = await f.evaluate(() => { const c = document.querySelector('canvas'); return c ? [c.width, c.height] : null; }).catch(() => null);
        if (wh) { last = wh; break; }
      }
    }
    const fs = flatness(menuShots[0]), fh = flatness(menuShots[1]);
    check('options: Sharp is visible (canvas at the view size, flat pixel runs)', last && last[0] * 3 !== last[1] * 4 && fh > fs + 0.15,
      `canvas ${last ? last.join('x') : '?'}; equal neighbours ${(fs * 100).toFixed(0)} % Smooth, ${(fh * 100).toFixed(0)} % Sharp`);
    writeFileSync(join(outDir, 'log-options.txt'), await L.text());
  } finally { L.stop(); await ctx.close(); }
}

// F5: the Options menu on a phone (dpr 3, as beach), portrait 390x844 and landscape 844x390: the whole page and the
// picture alone with the menu open, then each port row chosen and its value help shown (A on the row, B back).
// Pictures only, to look at (MENU_ROWS: how many port rows to visit below "Control Stick Setup", default 4).
async function menu() {
  const phone = await chromium.launch({ executablePath: chromePath, headless: true, args: [...launchArgs, '--force-device-scale-factor=3'] });
  const rows = +(process.env.MENU_ROWS || 4);
  try {
    for (const [tag, vp] of [['portrait', { width: 390, height: 844 }], ['landscape', { width: 844, height: 390 }]]) {
      const ctx = await phone.newContext({ viewport: vp, deviceScaleFactor: 3, isMobile: true, hasTouch: true, reducedMotion: 'reduce' });
      await ctx.addInitScript((ep) => { try { if (location.hostname === 'localhost') localStorage.setItem('rtw:test-endpoints', ep); } catch { /* */ } }, endpoints);
      const page = await ctx.newPage();
      page.on('pageerror', (e) => errors.push(e.message));
      const L = logger(page);
      try {
        await openGame(page, { remember: false });
        await page.waitForSelector('[data-state=playing]', { timeout: 180000 });
        await L.wait(/rtw: game started/, 90);
        await sleep(3000);
        await press(page, 'Enter', 200); await sleep(5000);
        await press(page, 'Enter', 200); await sleep(2500);
        await press(page, 'ArrowDown'); await sleep(1500);
        await press(page, 'KeyX'); await sleep(3500);
        await shot(page, `menu-${tag}-page`, true);
        await shot(page, `menu-${tag}`);
        for (let i = 0; i < 3; i++) { await press(page, 'ArrowDown'); await sleep(700); }
        for (let r = 0; r < rows; r++) {
          await press(page, 'ArrowDown'); await sleep(1000);
          await shot(page, `menu-${tag}-row${r + 1}`);
          await press(page, 'KeyX'); await sleep(1000);
          await press(page, 'ArrowRight'); await sleep(1000);
          await shot(page, `menu-${tag}-row${r + 1}-edit`);
          await press(page, 'KeyC'); await sleep(1000);   // B: back, the value unchanged
        }
        check(`menu ${tag}: the Options menu was shown (pictures to look at)`, true);
        writeFileSync(join(outDir, `log-menu-${tag}.txt`), await L.text());
      } finally { L.stop(); await ctx.close(); }
    }
  } finally { await phone.close().catch(() => {}); }
}

// F2: a Beach frame in phone portrait, Pixels Smooth then Sharp (the host's stored inGame option, as after a change in
// the game's menu). The save continues to Oak's lab; A every 2 s then reaches the Beach as in the harness route
// (port_work/merge_all/config.json: A every 120 retraces). Pictures only: look at them.
// Its own Chrome with --force-device-scale-factor=3: with the emulated deviceScaleFactor alone the sandboxed game
// frame reports devicePixelRatio 1 (rtw_resize dpr 100, canvas 392x294, upscaled 3x by the browser), unlike a phone;
// the emulated factor is kept so the screenshots have the device pixels.
async function beach() {
  const phone = await chromium.launch({ executablePath: chromePath, headless: true, args: [...launchArgs, '--force-device-scale-factor=3'] });
  try {
  for (const [tag, sharp] of [['smooth', 0], ['sharp', 1]]) {
    const ctx = await phone.newContext({ viewport: { width: 390, height: 844 }, deviceScaleFactor: 3, hasTouch: true, reducedMotion: 'reduce' });
    await ctx.addInitScript((ep) => { try { if (location.hostname === 'localhost') localStorage.setItem('rtw:test-endpoints', ep); } catch { /* */ } }, endpoints);
    await ctx.addInitScript((v) => { try { localStorage.setItem('rtw:options:pokemonsnap-us', JSON.stringify({ 2: v })); } catch { /* */ } }, sharp);
    const page = await ctx.newPage();
    page.on('pageerror', (e) => errors.push(e.message));
    const L = logger(page);
    try {
      await openGame(page, { remember: false });
      await page.waitForSelector('[data-state=playing]', { timeout: 180000 });
      await L.wait(/rtw: game started/, 90);
      await page.click('[data-action=menu]'); await sleep(500);
      await page.setInputFiles('[data-field=import-save]', SAVE);
      await L.wait(/rtw: game started.*save from the host/, 90);
      if (await page.$('[data-action=close-menu]')) await page.click('[data-action=close-menu]', { timeout: 5000 }).catch(() => {});
      const view = await page.evaluate(() => `${innerWidth}x${innerHeight} css px, dpr ${devicePixelRatio}`);
      const t0 = Date.now();
      while (Date.now() - t0 < 75000) { await press(page, 'KeyX'); await sleep(2000); }
      await shot(page, `beach-${tag}`, true);
      await sleep(5000);
      await shot(page, `beach-${tag}-b`);   // the picture only, at the page's device pixels
      const t = await L.text();
      check(`beach ${tag}: option 2 = ${sharp} from the host`, new RegExp(`rtw: option 2 = ${sharp} from the host`).test(t),
        `${view}; ` + [...t.matchAll(/rtw: (option 2|canvas).*/g)].map((m) => m[0]).join('; '));
      writeFileSync(join(outDir, `log-beach-${tag}.txt`), t);
    } finally { L.stop(); await ctx.close(); }
  }
  } finally { await phone.close().catch(() => {}); }
}

// F5: motion aim switched by the game's own Options row "Motion Aim" (option 3; manifest controls.motion = { when: 3 };
// needs a host that knows the object form, e.g. RTW_REPO=~/Code/retrowser-wt-motionopt). Phone landscape 844x390,
// touch, DeviceMotionEvent.requestPermission stubbed the iOS way (granted only from a tap), devicemotion events sent
// all the time (a turn about the screen's normal and about gravity). Checks: the host's ☰ has no Motion choice; with
// the option Off no rtw_motion reaches the module; Motion Aim On in the game's menu -> the host's "Allow" prompt; a tap
// on it -> rtw_motion reaches the module ("rtw: motion aim: first rotation from the host"); Off again -> the host stops
// reading (data-motion "off"). Pictures of the prompt.
async function motionopt() {
  const LAND = { width: 844, height: 390 };
  const ctx = await newCtx({ viewport: LAND, deviceScaleFactor: 3, isMobile: true, hasTouch: true });
  await ctx.addInitScript(() => {
    window.__asks = 0;
    let granted = false;
    DeviceMotionEvent.requestPermission = () => {
      window.__asks++;
      const tap = window.event?.type;
      if (!granted && tap !== 'click' && tap !== 'touchend') return Promise.reject(new DOMException('needs a tap', 'NotAllowedError'));
      granted = true;
      return Promise.resolve('granted');
    };
    // a phone held sideways that keeps turning slowly: rates in deg/s, gravity along the device's x (landscape 90)
    setInterval(() => {
      try {
        window.dispatchEvent(new DeviceMotionEvent('devicemotion', {
          rotationRate: { alpha: 20, beta: 15, gamma: 10 }, interval: 16,
          accelerationIncludingGravity: { x: 9.81, y: 0, z: 0 }, acceleration: { x: 0, y: 0, z: 0 },
        }));
      } catch { /* */ }
    }, 16);
  });
  const page = await ctx.newPage();
  page.on('pageerror', (e) => errors.push(e.message));
  const L = logger(page);
  const first = /rtw: motion aim: first rotation from the host/;
  const attr = (a) => page.getAttribute('[data-state=playing]', a).catch(() => null);
  const motionRow = async (right) => {   // Options -> Motion Aim: left (On) or right (Off) -> Return
    await sleep(3000);
    await press(page, 'Enter', 200); await sleep(5000);
    await press(page, 'Enter', 200); await sleep(2500);
    await press(page, 'ArrowDown'); await sleep(1500);
    await press(page, 'KeyX'); await sleep(3500);
    for (let i = 0; i < 6; i++) { await press(page, 'ArrowDown'); await sleep(700); }
    await press(page, 'KeyX'); await sleep(1000);
    await press(page, right ? 'ArrowRight' : 'ArrowLeft'); await sleep(1000);
    await press(page, 'KeyX'); await sleep(1000);
    await press(page, 'ArrowDown'); await sleep(1000);
    await press(page, 'KeyX');
  };
  try {
    await openGame(page, { remember: false });
    await page.waitForSelector('[data-state=playing]', { timeout: 180000 });
    await L.wait(/rtw: game started/, 90);
    await page.click('[data-action=menu]'); await page.waitForSelector('[data-state=menu]').catch(() => {});
    const hasChoice = !!(await page.$('[data-field=motion-off], [data-field=motion-aim]'));
    await shot(page, 'motionopt-menu', true);
    if (await page.$('[data-action=close-menu]')) await page.click('[data-action=close-menu]').catch(() => {});
    check('motionopt: the host\'s ☰ has no Motion choice (the game\'s option is the switch)', !hasChoice);
    await sleep(4000);
    check('motionopt: option 3 Off: no rtw_motion reaches the module', !first.test(await L.allText()), `data-motion ${await attr('data-motion')}`);
    await motionRow(false);
    const on = await L.wait(/rtw: option 3 = 1 changed in the game's Options menu/, 20);
    check('motionopt: Motion Aim On in the game\'s menu (rtw_option_changed 3 = 1)', !!on, on ?? 'no line');
    const ask = await page.waitForSelector('[data-state=motion-ask]', { timeout: 10000 }).catch(() => null);
    await shot(page, 'motionopt-ask', true);
    check('motionopt: the host asks for the motion sensors (Allow prompt)', !!ask);
    if (ask) await page.click('[data-action=motion-allow]');
    const got = await L.wait(first, 15);
    check('motionopt: after Allow, rtw_motion reaches the module', !!got, `${got ?? 'no line'}; data-motion ${await attr('data-motion')}; asks ${await page.evaluate(() => window.__asks)}`);
    await shot(page, 'motionopt-on', true);
    // a new visit with the option kept On: the host asks at once without a tap (iOS answers only from a tap here, so
    // the prompt again), then the game's menu turns it Off
    await L.reset();
    await page.reload();
    await openGame(page, { stored: true });
    await page.waitForSelector('[data-state=playing]', { timeout: 180000 });
    const t1 = await L.wait(/rtw: option 3 = 1 from the host/, 90);
    const ask2 = await page.waitForSelector('[data-state=motion-ask]', { timeout: 10000 }).catch(() => null);
    check('motionopt: reload with the option On: the host passes it and asks again', !!t1 && !!ask2, `${t1 ?? 'no option line'}; prompt ${!!ask2}`);
    if (ask2) await page.click('[data-action=motion-later]').catch(() => {});
    await motionRow(true);
    const off = await L.wait(/rtw: option 3 = 0 changed in the game's Options menu/, 20);
    await sleep(1000);
    const dm = await attr('data-motion');
    check('motionopt: Motion Aim Off again: the host stops reading the sensors', !!off && dm === 'off', `${off ?? 'no line'}; data-motion ${dm}`);
    writeFileSync(join(outDir, 'log-motionopt.txt'), await L.allText());
  } finally { L.stop(); await ctx.close(); }
}

// F3 + F4 through the host's UI (needs a host with ☰ -> Motion "Off / Motion aim" and Full-screen landscape: RTW_REPO =
// the retrowser checkout that has them; with an older host the phase fails at the menu).
// Phone landscape 844x390 (dpr 3, touch; the screen angle set through CDP as the host's e2e/motion.test.ts does;
// DeviceMotionEvent.requestPermission stubbed the iOS way). The devicemotion events are a simulated phone held
// upright: rotationRate from a device-frame rate, accelerationIncludingGravity = gravity turned by that rotation (so
// the host's label/unit calibration can lock), acceleration 0.
//  1. the ☰ opened by a real touch tap (CDP touch, not page.click) at 90 and 270, in the normal layout and in Full-
//     screen landscape, while the game runs: the menu opens and stays open, the game still there;
//     Full-screen landscape on (bar hidden, Shutter an ordinary extra button), ☰ -> Motion: Motion aim;
//  2. the SAVE continued to the Beach (A every 2 s), the tutorial's "Press Z to aim" passed (Z held + A) until the
//     cart moves;
//  3. a roll about the screen's normal (gravity turns, the aim does not) locks the calibration ("axes: spec (locked)"
//     in the menu); a turn right at 90 deg/s for 1 s about gravity: the module's "[motion] rotation stopped ... yaw +N"
//     within 5 % of the turn (pictures: a quarter turn), left likewise; the same at landscape 270;
//  4. Motion: Off -> the same turn gives no [motion] line;
//  5. a touch tap on the Shutter button (Full-screen landscape) takes exactly one picture ("[snap] photo taken ...,
//     film left N (shutter)", one below), again with a finger held on the stick;
//  6. a fresh page with WebKit's labels (alpha/beta/gamma = rates about x/y/z; Chrome is not WebKit, so the host
//     guesses the spec's): the roll locks "axes: webkit (locked)" and the turn right is still ~+90.
async function motion() {
  const LAND = { width: 844, height: 390 };
  const ctx = await newCtx({ viewport: LAND, deviceScaleFactor: 3, isMobile: true, hasTouch: true });
  // iOS-style permission (the host's e2e/motion.test.ts): the first grant only from a tap; once granted, iOS answers
  // at once for the rest of the page's life (the host asks again without a tap when the game restarts, e.g. a save
  // import)
  await ctx.addInitScript(() => {
    window.__asks = 0;
    let granted = false;
    DeviceMotionEvent.requestPermission = () => {
      window.__asks++;
      const tap = window.event?.type;
      if (!granted && tap !== 'click' && tap !== 'touchend') return Promise.reject(new DOMException('needs a tap', 'NotAllowedError'));
      granted = true;
      return Promise.resolve('granted');
    };
  });
  let page, L, cdp;
  const orient = async (angle) => {
    await cdp.send('Emulation.setDeviceMetricsOverride', {
      width: LAND.width, height: LAND.height, deviceScaleFactor: 3, mobile: true,
      screenOrientation: { type: angle === 90 ? 'landscapePrimary' : 'landscapeSecondary', angle },
    });
    await page.waitForFunction((a) => screen.orientation.angle === a && matchMedia('(orientation: landscape)').matches, angle);
    await sleep(300);
  };
  const angleNow = () => page.evaluate(() => screen.orientation.angle);
  const openMenu = async () => {
    await page.click((await page.$('[data-action=fsl-menu]')) ? '[data-action=fsl-menu]' : '[data-action=menu]');
    await page.waitForSelector('[data-state=menu]');
  };
  const closeMenu = async () => { if (await page.$('[data-action=close-menu]')) await page.click('[data-action=close-menu]', { timeout: 5000 }).catch(() => {}); await sleep(300); };
  const attr = (a) => page.getAttribute('[data-state=playing]', a);
  const choose = async (mode) => {   // ☰ -> Motion: off | aim
    await openMenu();
    const el = await page.$(`[data-field=motion-${mode}]`);
    if (el && !(await el.isChecked())) await el.click();
    await sleep(400);
    const ok = el ? await el.isChecked() : false;
    const diag = (await page.$('[data-field=motion-diag]')) ? await page.textContent('[data-field=motion-diag]') : '';
    await closeMenu();
    return { ok, diag, found: !!el };
  };
  const diagNow = async () => { await openMenu(); const d = (await page.$('[data-field=motion-diag]')) ? await page.textContent('[data-field=motion-diag]') : ''; await closeMenu(); return d; };
  // The simulated phone: `steps` of a constant device-frame rate w (deg/s about x, y, z) for ms each, at the screen
  // angle (gravity along the screen's up: device +x at 90, -x at 270), labels 'spec' (alpha/beta/gamma = z/x/y) or
  // 'webkit' (x/y/z). Returns the seconds the host integrates (time stamps' span + the first sample's interval).
  const phone = (steps, angle, labels = 'spec') => page.evaluate(async ({ steps, angle, labels }) => {
    const up = angle === 90 ? [1, 0, 0] : [-1, 0, 0];
    let g = up.map((v) => v * 9.81);
    const rot = (v, w, dt) => {   // v turned by the rotation -w dt (gravity seen from a turning device), Rodrigues
      const r = w.map((x) => -x * Math.PI / 180 * dt), th = Math.hypot(...r);
      if (th < 1e-12) return v;
      const k = r.map((x) => x / th), c = Math.cos(th), s = Math.sin(th);
      const kv = [k[1] * v[2] - k[2] * v[1], k[2] * v[0] - k[0] * v[2], k[0] * v[1] - k[1] * v[0]];
      const kd = (k[0] * v[0] + k[1] * v[1] + k[2] * v[2]) * (1 - c);
      return [0, 1, 2].map((i) => v[i] * c + kv[i] * s + k[i] * kd);
    };
    let first = -1, last = -1, prev = -1;
    for (const { w, ms } of steps) {
      const end = performance.now() + ms;
      while (performance.now() < end) {
        const now = performance.now();
        if (prev >= 0) g = rot(g, w, (now - prev) / 1000);
        prev = now;
        const rr = labels === 'spec' ? { alpha: w[2], beta: w[0], gamma: w[1] } : { alpha: w[0], beta: w[1], gamma: w[2] };
        const e = new DeviceMotionEvent('devicemotion', {
          rotationRate: rr, acceleration: { x: 0, y: 0, z: 0 },
          accelerationIncludingGravity: { x: g[0], y: g[1], z: g[2] }, interval: 16,
        });
        if (first < 0) first = e.timeStamp;
        last = e.timeStamp;
        dispatchEvent(e);
        await new Promise((ok) => setTimeout(ok, 16));
      }
    }
    return (last - first) / 1000 + 0.016;
  }, { steps, angle, labels });
  // a turn about gravity (yaw > 0: right) at 90 deg/s
  const yawRate = (angle, dps) => [angle === 90 ? -dps : dps, 0, 0];   // w = -dps * up
  const roll = (angle, labels) => phone([{ w: [0, 0, 60], ms: 600 }, { w: [0, 0, -60], ms: 1200 }, { w: [0, 0, 60], ms: 600 }], angle, labels);
  const box = async (sel) => page.locator(sel).first().boundingBox().catch(() => null);
  let touchId = 20;
  const touchPts = new Map();
  const pts = () => [...touchPts].map(([id, [x, y]]) => ({ x, y, id, radiusX: 2, radiusY: 2, force: 1 }));
  const touchStart = async (x, y) => { const id = touchId++; touchPts.set(id, [x, y]); await cdp.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: pts() }); return id; };
  const touchEnd = async (id) => {
    const p = touchPts.get(id); touchPts.delete(id);
    await cdp.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: touchPts.size ? [{ x: p[0], y: p[1], id, radiusX: 2, radiusY: 2, force: 1 }] : [] });
  };
  // the ☰ by a finger (touchStart + touchEnd through CDP: the host acts on pointerup and swallows the click after it)
  const menuTap = async (layout) => {
    const out = [];
    let ok = true;
    for (const angle of [90, 270]) {
      await orient(angle);
      const sel = layout === 'fsl' ? '[data-action=fsl-menu]' : '[data-action=menu]';
      const inLayout = !!(await page.$('.play.fsl')) === (layout === 'fsl');
      const b = await box(sel);
      if (!b || !inLayout) { ok = false; out.push(`${angle}: ${b ? 'wrong layout' : 'no ' + sel}`); continue; }
      const id = await touchStart(b.x + b.width / 2, b.y + b.height / 2);
      await sleep(80);
      await touchEnd(id);
      const opened = await page.waitForSelector('[data-state=menu]', { timeout: 3000 }).then(() => true, () => false);
      await sleep(300);
      const still = !!(await page.$('[data-state=menu]')), playing = !!(await page.$('[data-state=playing]'));
      ok &&= opened && still && playing;
      out.push(`${angle}: ☰ ${Math.round(b.width)}x${Math.round(b.height)} at y ${Math.round(b.y)}, ${opened ? (still ? 'opened, still open' : 'opened then closed') : 'did not open'}${playing ? '' : ', game gone'}`);
      if (still) { await page.click('[data-action=close-menu]'); await page.waitForSelector('[data-state=menu]', { state: 'detached' }).catch(() => {}); }
      await sleep(300);
    }
    await orient(90);
    return { ok, detail: out.join('; ') };
  };
  const short = (line) => (line ?? '').replace(/^.*\[motion\] /, '').replace(/ \(before the game's pitch limits\).*$/, '');
  // one turn about gravity for 1 s: the "[motion] rotation stopped" line that follows it, the yaw it should be
  const turnOnce = async (angle, dps, labels = 'spec', ms = 1000) => {
    await L.reset();
    const secs = await phone([{ w: yawRate(angle, dps), ms }], angle, labels);
    const want = dps * secs;
    const line = await L.wait(/\[motion\] rotation stopped/, 6);
    const m = line && line.match(/yaw ([+-][\d.]+), pitch ([+-][\d.]+)/);
    const yaw = m ? +m[1] : null;
    return { yaw, want, ok: yaw != null && Math.sign(yaw) === Math.sign(want) && Math.abs(yaw - want) <= 0.05 * Math.abs(want),
      line: line ?? 'no "[motion] rotation stopped" line', detail: `${short(line) || 'no [motion] line'} (wanted ${want >= 0 ? '+' : ''}${want.toFixed(1)})` };
  };
  // the game takes the camera away now and then (a message, a Pokemon's scene: "... degrees dropped outside camera
  // control"): then wait, A, and the same turn again
  const turn = async (angle, dps, labels = 'spec') => {
    let r = null;
    for (let i = 0; i < 6 && !(r && r.yaw != null); i++) {
      if (i) {
        const drop = (await L.text()).match(/\[motion\] camera control ended.*?([\d.]+) degrees dropped/);
        console.log(`  motion: no camera turn (${drop ? drop[1] + ' degrees dropped outside camera control' : r.line}); A, then again`);
        await press(page, 'KeyX'); await sleep(2500);
      }
      r = await turnOnce(angle, dps, labels);
      if (i) r.detail += ` (try ${i + 1})`;
    }
    return r;
  };
  // boot (a fresh page), the save, the Beach until the cart moves
  const toBeach = async (tag) => {
    page = await ctx.newPage();
    page.on('pageerror', (e) => errors.push(e.message));
    L = logger(page);
    cdp = await ctx.newCDPSession(page);
    await orient(90);
    await openGame(page, { remember: false });
    await page.waitForSelector('[data-state=playing]', { timeout: 180000 });
    await L.wait(/rtw: game started/, 90);
  };
  const beachRoute = async (tag) => {
    await openMenu();
    await L.reset();
    await page.setInputFiles('[data-field=import-save]', SAVE);
    const st = await L.wait(/rtw: game started.*save from the host/, 90);
    await closeMenu();
    check(`motion ${tag}: the save imported`, !!st, st ?? '');
    const t0 = Date.now();
    while (Date.now() - t0 < 75000) { await press(page, 'KeyX'); await sleep(2000); }
    // the tutorial stops the cart at "Press Z to aim" (the picture still, motion dropped by design) until 3 photos: Z
    // held, A, Z let go, A, until the picture moves again
    let moving = 0;
    for (let i = 0; i < 10; i++) {
      const a = await shot(page, `${tag}-wait-${i}a`);
      await sleep(1000);
      const b = await shot(page, `${tag}-wait-${i}b`);
      moving = pixelDiff(a, b);
      if (moving > 0.05) break;
      console.log(`  motion ${tag}: the picture is still (${(moving * 100).toFixed(1)} % changed): Z held + A`);
      await page.keyboard.down('KeyZ'); await sleep(2000);
      await press(page, 'KeyX'); await sleep(1500);
      await page.keyboard.up('KeyZ'); await sleep(2000);
      await press(page, 'KeyX'); await sleep(2500);
    }
    check(`motion ${tag}: the Beach, the cart moving (camera control)`, moving > 0.05, `${(moving * 100).toFixed(0)} % of pixels changed in 1 s`);
  };
  const photoAfter = async (secs) => { const p = await L.wait(/\[snap\] photo taken/, secs); return p; };
  const lastFilm = async () => { const f = [...(await L.allText()).matchAll(/\[snap\] photo taken.*film left (\d+)/g)]; return f.length ? +f[f.length - 1][1] : 60; };
  try {
    await toBeach('spec');
    // 1. the ☰ by a finger in the normal layout, then the host's menu: Full-screen landscape, Motion aim
    await sleep(3000);   // the game running (the intro)
    const mt1 = await menuTap('bar');
    check('menu: a touch tap on the ☰ opens the menu in landscape 90 and 270 (normal layout, game running)', mt1.ok, mt1.detail);
    await openMenu();
    const fsl = await page.$('[data-field=fs-landscape]');
    if (fsl) await fsl.check();
    await closeMenu();
    const aim = await choose('aim');
    const asks = await page.evaluate(() => window.__asks);
    const fslOn = !!(await page.$('.play.fsl'));
    const x17 = await box('[data-touch=x17]');
    check('motion: Full-screen landscape on from the menu: no bar, no corner zone, Shutter an ordinary extra button on screen',
      fsl && fslOn && !(await page.isVisible('.play .bar')) && !(await page.$('.corner-zone')) && x17 && x17.x >= 0 && x17.y >= 0 &&
      x17.x + x17.width <= LAND.width && x17.y + x17.height <= LAND.height && /Shutter/.test(await page.locator('[data-touch=x17]').first().textContent()),
      fsl ? `fsl ${fslOn}; Shutter ${x17 ? `${Math.round(x17.x)},${Math.round(x17.y)} ${Math.round(x17.width)}x${Math.round(x17.height)}` : 'missing'}` : 'no [data-field=fs-landscape]');
    check('motion: ☰ -> Motion: Motion aim chosen (permission asked from the tap, granted)', aim.ok && asks >= 1 && await attr('data-motion') === 'on',
      aim.found ? `asks ${asks}, data-motion ${await attr('data-motion')}, diag "${aim.diag}"` : 'no [data-field=motion-aim] (host without the Motion choice?)');
    await shot(page, 'motion-fsl', true);
    const mt2 = await menuTap('fsl');
    check('menu: a touch tap on the floating ☰ opens the menu in landscape 90 and 270 (Full-screen landscape, game running)', mt2.ok, mt2.detail);
    // 2. the Beach
    await beachRoute('spec');
    check('motion: no permission banner over the game after the restart', !(await page.$('[data-state=motion-ask]')) && await attr('data-motion') === 'on',
      `data-motion ${await attr('data-motion')}`);
    // 3. calibration, then the turns
    await L.reset();
    await roll(90, 'spec');
    await sleep(1000);
    const d1 = await diagNow();
    check('motion: a roll (gravity turning) locks the calibration on the spec\'s labels, and does not turn the camera',
      /axes: spec \(locked\)/.test(d1) && !/\[motion\]/.test(await L.text()), `diag "${d1}"; ${(await L.text()).match(/\[motion\].*/)?.[0] ?? 'no [motion] line'}`);
    let right = null, before = null, after = null;
    for (let i = 0; i < 6 && !(right && right.yaw != null); i++) {
      if (i) { console.log(`  motion: no camera turn yet (${right.line}); A, then again`); await press(page, 'KeyX'); await sleep(2500); }
      before = await shot(page, `motion-right-${i}-before`);
      right = await turnOnce(90, 90);
      after = await shot(page, `motion-right-${i}-after`);
    }
    const dOn = pixelDiff(before, after);
    check('motion (90): a turn right of the phone (90 deg/s, 1 s) turns the game right by the same angle (+-5 %)', right.ok,
      `${right.detail}; picture change ${(dOn * 100).toFixed(0)} % of pixels`);
    const left = await turn(90, -90);
    check('motion (90): the same turn to the left: the same angle, negative', left.ok, left.detail);
    // landscape-secondary (no screenshot between the turns: a Playwright screenshot puts its own emulated
    // orientation, 90, back over the CDP override)
    await orient(270);
    const a0 = await angleNow();
    const r270 = await turn(270, 90);
    const l270 = await turn(270, -90);
    const a1 = await angleNow();
    await shot(page, 'motion-270', true);
    check('motion (270): the same physical turns, 1 s each: right +, left -, same angle (+-5 %)', a0 === 270 && a1 === 270 && r270.ok && l270.ok,
      `screen angle ${a0} -> ${a1}; right: ${r270.detail}; left: ${l270.detail}`);
    await orient(90);
    // Motion: Off -> the same turn changes nothing
    const off = await choose('off');
    await L.reset();
    const offA = await shot(page, 'motion-off-before');
    await phone([{ w: yawRate(90, 90), ms: 1000 }], 90);
    await sleep(1000);
    const offB = await shot(page, 'motion-off-after');
    const dOff = pixelDiff(offA, offB);
    const tOff = await L.text();
    check('motion off: the same turn changes nothing (no rtw_motion, the view keeps its direction)',
      off.ok && await attr('data-motion') === 'off' && !/\[motion\]/.test(tOff) && dOff < dOn,
      `data-motion ${await attr('data-motion')}; ${tOff.match(/\[motion\].*/)?.[0] ?? 'no [motion] line'}; picture change ${(dOff * 100).toFixed(0)} % (on: ${(dOn * 100).toFixed(0)} %)`);
    // 5. the Shutter: a touch tap on the ordinary button (Full-screen landscape) = one picture
    const shutterShot = async (tag, withStick) => {
      let stickId = null;
      if (withStick) {
        const st = await box('[data-touch=stick]');
        if (st) { stickId = await touchStart(st.x + st.width / 2, st.y + 2); await sleep(300); }   // the stick's top edge: up
      }
      const filmBefore = await lastFilm();
      let photo = null, ignored = '';
      const sb = await box('[data-touch=x17]');
      // a tutorial message holds the camera now and then ("tap ignored ... (no camera control)"): wait and tap again
      for (let i = 0; i < 6 && !photo && sb; i++) {
        if (i) { ignored = (await L.text()).match(/\[snap\] shutter tap ignored.*/)?.[0] ?? ''; console.log(`  shutter ${tag}: ${ignored || 'no photo'}; again in 3 s`); await sleep(3000); }
        await L.reset();
        const id = await touchStart(sb.x + sb.width / 2, sb.y + sb.height / 2);
        await sleep(120);
        await touchEnd(id);
        photo = await photoAfter(6);
      }
      await sleep(300);
      await shot(page, `shutter-${tag}`, true);
      await L.wait(/\[snap\] leave -> idle|\[snap\] shoot -> idle/, 5);
      await sleep(1500);   // nothing more: exactly one picture
      if (stickId != null) await touchEnd(stickId);
      const t = await L.text();
      const photos = (t.match(/\[snap\] photo taken/g) || []).length;
      const seq = [...t.matchAll(/\[snap\] (\w+) -> (\w+) at retrace \d+.*?shutter\)/g)].map((m) => m[2]).join(' ');
      check(`shutter ${tag}: a touch tap on the Shutter button (Full-screen landscape) takes exactly one picture (the [snap] shutter sequence, film - 1)`,
        photo && /\(shutter\)/.test(photo) && photos === 1 && /enter aim shoot/.test(seq) && (!withStick || stickId != null) &&
        +(photo.match(/film left (\d+)/)?.[1]) === filmBefore - 1,
        `${seq || 'no shutter sequence'}; ${photo ? photo.replace(/^.*\[snap\] /, '') : 'no photo'} (before: ${filmBefore}); ${photos} photo line(s)` +
        (withStick ? `; stick ${stickId != null ? 'held' : 'not found'}` : '') + (ignored ? `; earlier: ${ignored.replace(/^.*\[snap\] /, '')}` : ''));
    };
    await shutterShot('tap', false);
    await sleep(2500);
    await shutterShot('tap-stick-held', true);
    await shot(page, 'shutter-after', true);
    writeFileSync(join(outDir, 'log-motion.txt'), await L.allText());
    L.stop(); await page.close();
    // 6. WebKit's labels, a fresh page (a new MotionAim, not locked yet: Chrome is not WebKit, so the spec's guess)
    await toBeach('webkit');
    const aim2 = await choose('aim');
    check('motion webkit: Motion aim chosen again (the stored choice was Off)', aim2.ok && /spec \(guess\)/.test(aim2.diag), `diag "${aim2.diag}"`);
    await beachRoute('webkit');
    await L.reset();
    await roll(90, 'webkit');
    await sleep(1000);
    const d2 = await diagNow();
    // (until the lock the host reads these events the spec's way, so the roll moves the camera a little then: only
    // the lock is asserted, the line shows what the guess did)
    const rollLine = (await L.text()).match(/\[motion\] rotation stopped.*/)?.[0];
    check('motion webkit: a roll locks the calibration on WebKit\'s labels', /axes: webkit \(locked\)/.test(d2),
      `diag "${d2}"; before the lock: ${rollLine ? short(rollLine) : 'no [motion] line'}`);
    let rw = null;
    for (let i = 0; i < 6 && !(rw && rw.yaw != null); i++) {
      if (i) { console.log(`  motion webkit: no camera turn yet (${rw.line}); A, then again`); await press(page, 'KeyX'); await sleep(2500); }
      const b = await shot(page, `webkit-right-${i}-before`);
      rw = await turnOnce(90, 90, 'webkit');
      const a = await shot(page, `webkit-right-${i}-after`);
      rw.detail += `; picture change ${(pixelDiff(b, a) * 100).toFixed(0)} %`;
    }
    check('motion webkit (90): the turn right with WebKit\'s labels turns the game right by the same angle (+-5 %)', rw.ok, rw.detail);
    writeFileSync(join(outDir, 'log-motion-webkit.txt'), await L.allText());
  } finally { L?.stop(); await ctx.close(); }
}
// F6: mouse aim on a desktop (1024x768, no touch). The host's pointer lock: the module calls rtw_pointer_lock(1), the
// host shows [data-field=mouse-catch] ("Click to use the mouse"), a click on it captures the mouse (data-locked 1) and
// rtw_mouse then carries the motion (movementX/Y) and the buttons; Esc (here: document.exitPointerLock, what the
// browser does on Esc) releases it.
async function mouse() {
  const ctx = await newCtx();
  const page = await ctx.newPage();
  page.on('pageerror', (e) => errors.push(e.message));
  const L = logger(page);
  const catcher = async () => page.getAttribute('[data-field=mouse-catch]', 'data-locked', { timeout: 500 }).catch(() => null);
  const lockedNow = () => page.evaluate(() => !!document.pointerLockElement);
  const toOptions = async () => {
    await sleep(3000);
    await press(page, 'Enter', 200); await sleep(5000);
    await press(page, 'Enter', 200); await sleep(2500);
    await press(page, 'ArrowDown'); await sleep(1500);
    await press(page, 'KeyX'); await sleep(3500);
  };
  // the Options menu's Mouse Aim row (7 rows below Screen): A, left (On) or right (Off), A, down to Return, A
  const setMouseAim = async (on) => {
    for (let i = 0; i < 7; i++) { await press(page, 'ArrowDown'); await sleep(700); }
    await press(page, 'KeyX'); await sleep(1200);
    await press(page, on ? 'ArrowLeft' : 'ArrowRight'); await sleep(1200);
    await shot(page, `mouse-option-${on ? 'on' : 'off'}`);
    await press(page, 'KeyX'); await sleep(1200);
    await press(page, 'ArrowDown'); await sleep(1000);
    await press(page, 'KeyX');
  };
  const boot = async (stored) => {
    console.log('mouse: ' + await openGame(page, { stored, remember: true }));
    await page.waitForSelector('[data-state=playing]', { timeout: 180000 });
    return L.wait(/rtw: game started/, 90);
  };
  const photos = async () => ((await L.text()).match(/\[snap\] photo taken.*\(mouse click\)/g) || []).length;
  const click = async (button = 'left', ms = 90) => { await page.mouse.down({ button }); await sleep(ms); await page.mouse.up({ button }); };
  // a move of dx, dy CSS pixels in steps of 10 from where the mouse is (the host sums movementX/Y over a display
  // frame; every move counts while captured, so the position is kept here and the tests move back and forth)
  const pos = { x: 500, y: 380 };
  const move = async (dx, dy) => {
    const n = Math.max(Math.abs(dx), Math.abs(dy)) / 10;
    for (let i = 0; i < n; i++) { pos.x += dx / n; pos.y += dy / n; await page.mouse.move(pos.x, pos.y); await sleep(16); }
  };
  const relock = async () => {
    if (await catcher() === '1') return true;
    await page.waitForSelector('[data-field=mouse-catch]', { timeout: 10000 }).catch(() => {});
    await page.click('[data-field=mouse-catch]').catch(() => {});
    return page.waitForSelector('[data-field=mouse-catch][data-locked="1"]', { timeout: 5000 }).then(() => true, () => false);
  };
  try {
    // 1. option 4 in the game's Options menu: Off, reported; after a reload passed back as 0, then On again
    const st0 = await boot(false);
    const t0 = await L.text();
    check('mouse: the host passes option 4 (Mouse Aim) at start, default On', st0 && /rtw: option 4 = 1 from the host/.test(t0),
      (t0.match(/rtw: option 4.*/) || ['no rtw_option 4 line'])[0]);
    await toOptions();
    await shot(page, 'mouse-options-menu');
    await setMouseAim(false);
    const off = await L.wait(/rtw: option 4 = 0 changed in the game's Options menu/, 20);
    check('mouse: Mouse Aim -> Off in the game\'s menu is reported (rtw_option_changed 4 = 0)', !!off, off ?? 'no line');
    check('mouse: no pointer lock asked in the title and menus', !/pointer lock asked/.test(await L.allText()) && !(await page.$('[data-field=mouse-catch]')));
    await sleep(1500);
    await L.reset();
    await page.reload();
    await boot(true);
    const t1 = await L.text();
    check('mouse: after a reload the host passes the remembered Off (rtw_option 4 = 0)', /rtw: option 4 = 0 from the host/.test(t1),
      (t1.match(/rtw: option 4.*/) || ['no rtw_option 4 line'])[0]);
    await toOptions();
    await setMouseAim(true);
    const on = await L.wait(/rtw: option 4 = 1 changed in the game's Options menu/, 20);
    check('mouse: Mouse Aim -> On again is reported (rtw_option_changed 4 = 1)', !!on, on ?? 'no line');
    await sleep(1500);
    await L.reset();
    await page.reload();
    await boot(true);
    const t2 = await L.text();
    check('mouse: after a second reload the host passes On (rtw_option 4 = 1)', /rtw: option 4 = 1 from the host/.test(t2),
      (t2.match(/rtw: option 4.*/) || ['no rtw_option 4 line'])[0]);

    // 2. the save, the Beach: the module asks for the mouse when the player gets the camera
    await page.click('[data-action=menu]'); await sleep(500);
    await L.reset();
    await page.setInputFiles('[data-field=import-save]', SAVE);
    const st = await L.wait(/rtw: game started.*save from the host/, 90);
    if (await page.$('[data-action=close-menu]')) await page.click('[data-action=close-menu]', { timeout: 5000 }).catch(() => {});
    check('mouse: the save imported', !!st, st ?? '');
    let asked = null;
    const t3 = Date.now();
    while (Date.now() - t3 < 90000 && !asked) {
      await press(page, 'KeyX'); await sleep(2000);
      asked = (await L.text()).match(/.*rtw: pointer lock asked.*/)?.[0] ?? null;
    }
    const shown = await catcher();
    check('mouse: in the course the module asks for the mouse (rtw_pointer_lock 1), the host shows its "click to use the mouse"',
      !!asked && shown === '0', `${asked ?? 'no rtw: pointer lock line'}; mouse-catch data-locked ${shown}`);
    // A through the course's first messages (keyboard; the mouse is not captured yet)
    for (let i = 0; i < 10; i++) { await press(page, 'KeyX'); await sleep(2000); }
    await shot(page, 'mouse-ask', true);
    // 3. the capturing click is the host's
    await L.reset();
    const locked = await relock();
    await sleep(1500);
    const tl = await L.text();
    check('mouse: a click on the picture captures the mouse; that click does not reach the game',
      locked && await lockedNow() && !/\[snap\] photo taken|\[mouse\] (A|click)/.test(tl),
      `data-locked ${await catcher()}; ${tl.match(/\[(snap|mouse)\].*/)?.[0] ?? 'no [snap] / [mouse] line'}`);
    // 4. left clicks: the tutorial's pictures ("Press Z to aim" holds the cart until 3 photos), until the cart moves
    await page.mouse.move(pos.x, pos.y);
    let taken = 0, moving = 0, firstPhoto = null;
    for (let i = 0; i < 14 && moving <= 0.05; i++) {
      await relock();
      await L.reset();
      await click('left');
      const p = await L.wait(/\[snap\] photo taken.*\(mouse click\)/, 5);
      if (p) { taken++; firstPhoto ??= p; }
      await sleep(1500);
      if (taken >= 3) {
        const a = await shot(page, `mouse-cart-${i}a`); await sleep(1000); const b = await shot(page, `mouse-cart-${i}b`);
        moving = pixelDiff(a, b);
        if (moving <= 0.05) { await press(page, 'KeyX'); await sleep(1500); }   // a message after the pictures
      }
    }
    check('mouse: left clicks take pictures (the shutter sequence started by the mouse: "[snap] photo taken ... (mouse click)")',
      taken >= 3, `${taken} picture(s); ${firstPhoto ? firstPhoto.replace(/^.*\[snap\] /, '') : 'no photo'}`);
    check('mouse: the Beach, the cart moving (camera control)', moving > 0.05, `${(moving * 100).toFixed(0)} % of pixels changed in 1 s`);
    // 5. motion turns the view: 400 px right = +32 degrees, 200 px up = +16 (0.08 degree a pixel), the game's
    // scenes take the camera now and then ("... dropped outside camera control"): then A and again
    const turn = async (dx, dy, want, axis) => {
      let r = null;
      for (let i = 0; i < 6 && !(r && r.ok); i++) {
        if (i) { console.log(`  mouse: no turn (${r.line}); A, then again`); await press(page, 'KeyX'); await sleep(2500); }
        await relock();
        await L.reset();
        const b = await shot(page, `mouse-turn-${axis}-${i}-before`);
        await move(dx, dy);
        const line = await L.wait(/\[mouse\] movement stopped/, 6);
        const a = await shot(page, `mouse-turn-${axis}-${i}-after`);
        const m = line && line.match(/yaw ([+-][\d.]+), pitch ([+-][\d.]+)/);
        const got = m ? +m[axis === 'yaw' ? 1 : 2] : null;
        r = { ok: got != null && Math.abs(got - want) <= 0.05 * Math.abs(want) && /0\.00 degrees dropped/.test(line),
          line: line ? line.replace(/^.*\[mouse\] /, '') : 'no "[mouse] movement stopped" line', change: pixelDiff(b, a), got };
      }
      return r;
    };
    const right = await turn(400, 0, 32, 'yaw');
    check('mouse: 400 px to the right turns the view right by 32 degrees (+-5 %)', right.ok,
      `${right.line}; picture change ${(right.change * 100).toFixed(0)} %`);
    const left = await turn(-400, 0, -32, 'yaw');
    check('mouse: 400 px back to the left: -32 degrees (+-5 %)', left.ok, `${left.line}; picture change ${(left.change * 100).toFixed(0)} %`);
    const up = await turn(0, -200, 16, 'pitch');
    check('mouse: 200 px up tilts the view up by 16 degrees (+-5 %)', up.ok, `${up.line}; picture change ${(up.change * 100).toFixed(0)} %`);
    await move(0, 200);   // back level
    await sleep(800);
    // 6. the right button held: zoom (hold), a left click while zoomed: a picture; let go: zoomed out
    let zoomOk = false, zoomPhoto = null, zoomOut = null;
    for (let i = 0; i < 4 && !(zoomOk && zoomPhoto && zoomOut); i++) {
      if (i) { await press(page, 'KeyX'); await sleep(2500); }
      await relock();
      await L.reset();
      await page.mouse.down({ button: 'right' });
      zoomOk = !!(await L.wait(/\[snap\] enter -> aim .*right button/, 5));
      await sleep(600);
      await shot(page, 'mouse-zoomed-aim', true);
      await move(-80, 0);
      await move(80, 0);
      await click('left');
      zoomPhoto = await L.wait(/\[snap\] photo taken.*\(mouse click\)/, 5);
      await sleep(400);
      await page.mouse.up({ button: 'right' });
      zoomOut = await L.wait(/\[snap\] (aim -> leave|leave -> idle) .*right button/, 5);
      await sleep(1000);
    }
    check('mouse: the right button held zooms in; a left click then takes a picture; let go: zoomed out',
      zoomOk && zoomPhoto && zoomOut, `${zoomOk ? 'zoomed' : 'no zoom'}; ${zoomPhoto ? zoomPhoto.replace(/^.*\[snap\] /, '') : 'no photo'}; ${zoomOut ? 'zoomed out' : 'not zoomed out'}`);
    // 7. the middle click: an apple (A while zoomed out)
    let apple = null;
    for (let i = 0; i < 4 && !apple; i++) {
      if (i) { await press(page, 'KeyX'); await sleep(2500); }
      await relock();
      await L.reset();
      await click('middle');
      apple = await L.wait(/\[mouse\] A \(middle click: apple\)/, 4);
    }
    check('mouse: a middle click throws an apple (A while zoomed out)', !!apple, apple ?? (await L.text()).match(/\[mouse\].*/)?.[0] ?? 'no line');
    await sleep(1500);
    // 8. the pause menu gives the mouse back; camera control asks again
    await relock();
    await L.reset();
    await press(page, 'Enter', 150);
    const given = await L.wait(/rtw: pointer lock given back/, 6);
    await sleep(800);
    const lockP = await lockedNow(), catchP = await page.$('[data-field=mouse-catch]');
    await shot(page, 'mouse-pause', true);
    check('mouse: the pause menu gives the mouse back (rtw_pointer_lock 0: not captured, no "click to use the mouse")',
      !!given && !lockP && !catchP, `${given ?? 'no line'}; locked ${lockP}; mouse-catch ${!!catchP}`);
    await press(page, 'Enter', 150);   // Continue
    const again = await L.wait(/rtw: pointer lock asked/, 10);
    check('mouse: back in the course the module asks again', !!again, again ?? 'no line');
    // 9. Esc (the browser releases the lock): the host shows its prompt again; the module does not ask by itself
    await relock();
    await L.reset();
    await page.keyboard.press('Escape');
    await sleep(500);
    let how = 'Escape key';
    if (await lockedNow()) { await page.evaluate(() => document.exitPointerLock()); how = 'document.exitPointerLock (the Escape key does not release it in headless Chrome)'; }
    await sleep(3000);
    const te = await L.text();
    check('mouse: Esc releases the mouse; the host keeps its prompt, the module does not call again',
      !(await lockedNow()) && await catcher() === '0' && !/rtw: pointer lock (asked|given back)/.test(te),
      `${how}; data-locked ${await catcher()}; ${te.match(/rtw: pointer lock.*/)?.[0] ?? 'no rtw_pointer_lock call'}`);
    await L.reset();
    await click('left');   // a click on the prompt captures again: no picture
    await sleep(1500);
    check('mouse: the click that captures it again takes no picture', await lockedNow() && (await photos()) === 0,
      `locked ${await lockedNow()}; ${(await L.text()).match(/\[snap\] photo taken.*/)?.[0] ?? 'no photo'}`);
    writeFileSync(join(outDir, 'log-mouse.txt'), await L.allText());
  } finally { L.stop(); await ctx.close(); }
}

// share of pixels whose colour changed by more than 48 (sum of |dR|+|dG|+|dB|) between two same-size screenshots
function pixelDiff(fa, fb) {
  const a = pngPixels(fa), b = pngPixels(fb);
  if (a.w !== b.w || a.h !== b.h) return 1;
  let n = 0;
  for (let i = 0; i < a.w * a.h; i++) {
    const o = i * a.bpp, p = i * b.bpp;
    if (Math.abs(a.px[o] - b.px[p]) + Math.abs(a.px[o + 1] - b.px[p + 1]) + Math.abs(a.px[o + 2] - b.px[p + 2]) > 48) n++;
  }
  return n / (a.w * a.h);
}

async function wrong() {
  const bad = join(outDir, 'wrong.z64');   // the ROM with one byte changed (local only, deleted at the end)
  const z = readFileSync(join(ROOT, 'pokemonsnap.z64'));
  z[0x1000] ^= 1;
  writeFileSync(bad, z);
  const ctx = await newCtx();
  const page = await ctx.newPage();
  try {
    await openGame(page, { rom: bad, remember: false });
    const warn = await page.waitForSelector('[data-state=wrong-rom]', { timeout: 30000 }).catch(() => null);
    check('wrong ROM: the host warns', !!warn);
    if (warn) await page.click('[data-action=continue-anyway]');
    const e = await page.waitForSelector('.play .error, .error', { timeout: 60000 }).catch(() => null);
    const t = e ? await e.textContent() : '';
    check('wrong ROM: the module refuses with a plain message', /US release of Pokemon Snap/.test(t ?? ''), (t ?? '').slice(0, 160));
    await shot(page, 'wrong-rom', true);
  } finally { await ctx.close(); rmSync(bad, { force: true }); }
}

async function offline() {
  // a persistent-like flow in one context: online once (caches filled, ROM remembered), then everything off
  const ctx = await newCtx();
  const page = await ctx.newPage();
  page.on('pageerror', (e) => errors.push(e.message));
  const L = logger(page);
  try {
    await openGame(page, { remember: true });
    await page.waitForSelector('[data-state=playing]', { timeout: 180000 });
    check('offline: online boot first', !!(await L.wait(/rtw: game started/, 90)));
    await sleep(3000);
    const before = mock.log.length;
    const mlog = mock.log;
    await ctx.setOffline(true);
    await stopHost();
    await L.reset();
    await page.reload();
    console.log('offline: ' + await openGame(page, { stored: true }).catch((e) => 'open failed ' + e.message));
    await page.waitForSelector('[data-state=playing]', { timeout: 120000 });
    check('offline: boot from the caches', !!(await L.wait(/rtw: game started/, 90)));
    await sleep(8000); await shot(page, 'offline');
    check('offline: no request reached the mock', mlog.length === before);
    writeFileSync(join(outDir, 'log-offline.txt'), await L.text());
  } finally { L.stop(); await ctx.close(); }
}

try {
  for (const p of PHASES) {
    console.log(`== ${p}`);
    try { await ({ desk, title, touch, options, menu, beach, motion, motionopt, mouse, wrong, offline })[p](); } catch (e) { check(`${p}: ran`, false, e.stack || String(e)); }
  }
  check('no page errors', errors.length === 0, errors.slice(0, 3).join(' | '));
} finally {
  await browser.close().catch(() => {});
  await stopHost();
}
console.log(results.every(Boolean) ? 'e2e: PASS' : 'e2e: FAIL');
process.exit(results.every(Boolean) ? 0 : 1);
