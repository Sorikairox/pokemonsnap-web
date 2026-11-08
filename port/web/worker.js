// Pokémon Snap port: local test worker (session S3c). The retrowser model: the module runs in a worker and draws on
// an OffscreenCanvas given as Module.canvas; the host calls port_vi_retrace() once per display frame.
// The page (index.html) hands over the canvas and the run configuration (from the harness, port/web/run.mjs):
//   cfg.frames  number of retraces to run            cfg.shots  [frame, ...]: canvas PNG after that retrace
//   cfg.tap     [every, mask]: press mask for 3 retraces every `every` (same rule as node's --tap)
//   cfg.input   [[frame, mask, length, x, y], ...]: press mask (and hold the stick at x, y) from retrace `frame`
//               for `length` retraces
//   cfg.tapEnd  no taps from this retrace on (0: never stop)
//   cfg.logops  [frame, ...]: log every display-list command of the tasks run during that retrace ([gfx] lines)
//   cfg.pace    'fast' (as fast as the worker runs, one event-loop turn per retrace: deterministic screenshots) or
//               'raf' (one retrace per requestAnimationFrame: the display's rate)
//   cfg.readback  record the read-back journal (every byte GPU write-backs changed, S6) and POST it to /readback at
//               the end: node replays it (port/tools/node_run.js --readback) to follow this run's game state
// The ROM is fetched from /rom, which the harness serves from the player's own file (never from the repo).
// Results go back to the harness by POST: /shot?name=fNNNNN.png, /log, /done.
importScripts('pokemonsnap.js');

const lines = [];
const print = (t) => { lines.push(t); };
const flushLog = async () => {
  if (!lines.length) return;
  const body = lines.splice(0).join('\n') + '\n';
  await fetch('/log', { method: 'POST', body });
};
const tick = () => new Promise((r) => setTimeout(r, 0));
const raf = () => new Promise((r) => (self.requestAnimationFrame ? self.requestAnimationFrame(() => r()) : setTimeout(r, 16)));

function padAt(cfg, f) {   // same rules as port/boot/main.c
  let mask = 0, x = 0, y = 0;
  if (cfg.tap && cfg.tap[0] > 0 && f % cfg.tap[0] < 3 && !(cfg.tapEnd && f >= cfg.tapEnd)) mask |= cfg.tap[1];
  for (const [at, m, len, sx, sy] of cfg.input || [])
    if (f >= at && f < at + (len || 3)) { mask |= m; if (sx || sy) { x = sx || 0; y = sy || 0; } }
  return [mask, x, y];
}

onmessage = async (e) => {
  const { canvas, cfg } = e.data;
  try {
    const rom = new Uint8Array(await (await fetch('/rom')).arrayBuffer());
    const M = await createPokemonSnap({ canvas, print, printErr: print });
    const p = M._malloc(rom.length);
    M.HEAPU8.set(rom, p);
    if (M._port_set_rom(p, rom.length) !== 0) throw new Error('port_set_rom failed');
    M._free(p);
    if (cfg.readback) M._port_rb_record(1);
    const rc = M.callMain(cfg.args || []);
    print(`[web] main returned ${rc}; ${cfg.frames} retraces, pace ${cfg.pace}`);
    const shots = new Set(cfg.shots || []);
    const t0 = performance.now();
    let tl = t0;
    for (let f = 1; f <= cfg.frames; f++) {
      const [mask, sx, sy] = padAt(cfg, f);
      M._port_set_pad(0, mask, sx, sy);
      const lo = (cfg.logops || []).includes(f);
      if (lo) M._port_gfx_set_log_ops(1);
      M._port_vi_retrace();
      if (lo) M._port_gfx_set_log_ops(0);
      if (shots.has(f)) {
        M._port_gfx_log_stats();
        const blob = await canvas.convertToBlob({ type: 'image/png' });
        await fetch(`/shot?name=f${String(f).padStart(5, '0')}.png`, { method: 'POST', body: blob });
      }
      if (f % 60 === 0) {
        const t = performance.now();
        print(`[web] retrace ${f}: ${((t - tl) / 60).toFixed(2)} ms per retrace (wall, incl. GPU waits)`);
        tl = t;
        await flushLog();
      }
      if (cfg.pace === 'raf') await raf(); else await tick();
    }
    M._port_gfx_log_stats();
    if (cfg.readback) {
      const n = M._port_rb_journal_size() >>> 0, p = M._port_rb_journal() >>> 0;
      await fetch('/readback', { method: 'POST', body: M.HEAPU8.slice(p, p + n) });
      print(`[web] read-back journal: ${n} bytes over ${M._port_rb_point_count() >>> 0} points`);
    }
    print(`[web] done: ${cfg.frames} retraces in ${(performance.now() - t0).toFixed(0)} ms`);
  } catch (err) {
    print('[web] EXCEPTION ' + err + '\n' + (err && err.stack));
  }
  await flushLog();
  await fetch('/done', { method: 'POST' });
};
