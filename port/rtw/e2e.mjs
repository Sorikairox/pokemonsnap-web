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
//                the Snap extra button (F1) is shown and reaches the pad as bit 16;
//         options  (F1) the host passes option 1 at start, the game's Options menu changes it (rtw_option_changed),
//                the host remembers it across a reload; the Snap key (Digit1) reaches the pad;
//         wrong  a ROM with one byte changed: the host's wrong-ROM warning, then the module's plain refusal;
//         offline  every server stopped, the network off: reload, boot from the caches and the remembered ROM.
// Screenshots and the stored save go to the out dir (game pictures: local only, never commit). Every server and
// Chrome it starts is stopped at the end. Prints PASS / FAIL lines.
import { createRequire } from 'node:module';
import { execFileSync } from 'node:child_process';
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
    for (const line of cur.slice(k)) seen.push(line);
    prev = cur;
  };
  const timer = setInterval(() => { poll().catch(() => {}); }, 200);
  return {
    async text() { await poll(); return seen.join('\n'); },
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
    const m = pace && pace.match(/([\d.]+) retraces\/s, ([\d.]+) display frames\/s, timer wake-ups ([\d.]+)\/s/);
    check('pacing: ~60 retraces/s on display frames, few timer wake-ups', m && Math.abs(+m[1] - 60) < 3 && +m[3] < 10, pace ?? 'no pacing line');
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
      // F1: the manifest's extra button "Snap" (controls.extra bit 16) is on the layout, inside the view, and reaches
      // pad 0 as bit 16
      const sb = await page.locator('[data-touch=x16]').first().boundingBox().catch(() => null);
      const label = sb ? (await page.locator('[data-touch=x16]').first().textContent()) : '';
      check(`touch ${name}: the Snap button is shown`, sb && sb.width >= 20 && sb.height >= 20 && sb.x >= 0 && sb.y >= 0 &&
        sb.x + sb.width <= w && sb.y + sb.height <= h && /Snap/.test(label), sb ? `${label} ${Math.round(sb.x)},${Math.round(sb.y)} ${Math.round(sb.width)}x${Math.round(sb.height)}` : 'no [data-touch=x16]');
      if (sb) {
        const cdp = await ctx.newCDPSession(page);
        const pt = [{ x: sb.x + sb.width / 2, y: sb.y + sb.height / 2, id: 8, radiusX: 2, radiusY: 2, force: 1 }];
        for (let i = 0; i < 3 && !/pad0 buttons 10000/.test(await L.text()); i++) {
          await cdp.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: pt });
          await sleep(150);
          if (!i) await shot(page, `touch-${name}-Snap-down`, true);
          await cdp.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
          await sleep(800);
        }
        check(`touch ${name}: the Snap button reached pad 0 (bit 16)`, /pad0 buttons 10000/.test(await L.text()));
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
    await shot(page, `options-${tag}`);
  };
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
    await press(page, 'ArrowDown'); await sleep(1000);    // Return
    await press(page, 'KeyX');
    const ch = await L.wait(/rtw: option 1 = 0 changed in the game's Options menu/, 20);
    check('options: the game\'s menu reports the change (rtw_option_changed 1 = 0)', !!ch, ch ?? 'no line');
    await sleep(2000);
    await L.reset();
    await page.reload();
    console.log('options reload: ' + await openGame(page, { stored: true }));
    await page.waitForSelector('[data-state=playing]', { timeout: 180000 });
    await L.wait(/rtw: game started/, 90);
    const t1 = await L.text();
    check('options: after a reload the host passes the remembered value (rtw_option 1 = 0)', /rtw: option 1 = 0 from the host/.test(t1),
      (t1.match(/rtw: option.*/) || ['no rtw_option line'])[0]);
    await toOptions('menu-reload');   // the row shows Off
    writeFileSync(join(outDir, 'log-options.txt'), await L.text());
  } finally { L.stop(); await ctx.close(); }
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
    try { await ({ desk, title, touch, options, wrong, offline })[p](); } catch (e) { check(`${p}: ran`, false, e.stack || String(e)); }
  }
  check('no page errors', errors.length === 0, errors.slice(0, 3).join(' | '));
} finally {
  await browser.close().catch(() => {});
  await stopHost();
}
console.log(results.every(Boolean) ? 'e2e: PASS' : 'e2e: FAIL');
process.exit(results.every(Boolean) ? 0 : 1);
