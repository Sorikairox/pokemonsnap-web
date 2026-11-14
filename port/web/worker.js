// Pokémon Snap port: local test worker (session S3c). The retrowser model: the module runs in a worker and draws on
// an OffscreenCanvas given as Module.canvas; the host calls port_vi_retrace() once per display frame.
// The page (index.html) hands over the canvas and the run configuration (from the harness, port/web/run.mjs):
//   cfg.frames  number of retraces to run            cfg.shots  [frame, ...]: canvas PNG after that retrace
//   cfg.tap     [every, mask]: press mask for 3 retraces every `every` (same rule as node's --tap)
//   cfg.input   [[frame, mask, length, x, y], ...]: press mask (and hold the stick at x, y) from retrace `frame`
//               for `length` retraces
//   cfg.motion  (F3) [[frame, yaw, pitch, length], ...]: port_motion_add(yaw, pitch) before each retrace from `frame`
//               for `length` (default 1) retraces, as the retrowser host's rtw_motion before rtw_input
//   cfg.mouse   (F6) [[frame, dx, dy, buttons, length], ...]: from `frame` for `length` (default 1) retraces, dx / dy
//               pixels and the buttons held; port_mouse_add(dx, dy, buttons) before the retrace when there is motion
//               or the buttons changed (the retrowser host's rtw_mouse)
//   cfg.tapEnd  no taps from this retrace on (0: never stop)
//   cfg.logops  [frame, ...]: log every display-list command of the tasks run during that retrace ([gfx] lines)
//   cfg.pace    'fast' (as fast as the worker runs, one event-loop turn per retrace: deterministic screenshots) or
//               'raf' (one retrace per requestAnimationFrame: the display's rate)
//   cfg.readback  record the read-back journal (every byte GPU write-backs changed, S6) and POST it to /readback at
//               the end: node replays it (port/tools/node_run.js --readback) to follow this run's game state
//   cfg.audio   (S7) drain the PCM the virtual AI played after every retrace (rtw_audio_drain, as the retrowser host
//               does) and POST it to /audio at the end: the harness writes OUT/audio.wav
//   cfg.play    (S7) also send each drained block to the page, which plays it (Web Audio; off by default, and
//               headless Chrome runs muted)
//   cfg.options (F1) [[id, value], ...]: port_option_set before main (the retrowser host's rtw_option)
//   cfg.save    (S8) boot with the FlashRAM image the harness serves at /save; after every retrace that changed the
//               flash the image is POSTed to /save?gen=N (the harness keeps it as OUT/flash.fla)
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
    if (cfg.save) {
      const sv = new Uint8Array(await (await fetch('/save')).arrayBuffer());
      const q = M._malloc(sv.length);
      M.HEAPU8.set(sv, q);
      M._port_save_load(q, sv.length);
      M._free(q);
    }
    const saveDump = async () => {
      const q = M._port_flash_image() >>> 0;
      await fetch(`/save?gen=${M._port_flash_generation() >>> 0}`, { method: 'POST', body: M.HEAPU8.slice(q, q + M._rtw_save_size()) });
    };
    for (const [id, v] of cfg.options || []) M._port_option_set(id, v);   // F1: as rtw_option at start
    if (cfg.readback) M._port_rb_record(1);
    const rc = M.callMain(cfg.args || []);
    print(`[web] main returned ${rc}; ${cfg.frames} retraces, pace ${cfg.pace}`);
    const shots = new Set(cfg.shots || []);
    const pcm = [];   // S7: Int16Array blocks drained per retrace
    let mouseB = 0;   // F6: the buttons last sent
    let pcmFrames = 0;
    const t0 = performance.now();
    let tl = t0;
    for (let f = 1; f <= cfg.frames; f++) {
      const [mask, sx, sy] = padAt(cfg, f);
      let my = 0, mp = 0;   // F3: motion aim
      for (const [at, y, p, len] of cfg.motion || []) if (f >= at && f < at + (len || 1)) { my += y || 0; mp += p || 0; }
      if (my || mp) M._port_motion_add(my, mp);
      let mx = 0, mdy = 0, mb = 0;   // F6: mouse aim
      for (const [at, dx, dy, b, len] of cfg.mouse || []) if (f >= at && f < at + (len || 1)) { mx += dx || 0; mdy += dy || 0; mb |= b || 0; }
      if (mx || mdy || mb !== mouseB) { M._port_mouse_add(mx, mdy, mb); mouseB = mb; }
      M._port_set_pad(0, mask, sx, sy);
      const lo = (cfg.logops || []).includes(f);
      if (lo) M._port_gfx_set_log_ops(1);
      M._port_vi_retrace();
      if (lo) M._port_gfx_set_log_ops(0);
      if (cfg.audio || cfg.play) {   // S7: the retrowser host's pull (rtw_audio_drain / rtw_audio_buffer)
        const n = M._rtw_audio_drain() >>> 0;
        if (n) {
          const p = M._rtw_audio_buffer() >>> 0;
          const blk = new Int16Array(M.HEAPU8.buffer.slice(p, p + 4 * n));
          if (cfg.audio) { pcm.push(blk); pcmFrames += n; }
          if (cfg.play) postMessage({ pcm: blk, rate: M._rtw_audio_rate() });
        }
      }
      if (M._port_save_poll()) await saveDump();
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
    if (cfg.audio) {
      const rate = M._rtw_audio_rate(), h = new DataView(new ArrayBuffer(44)), w = (o, t) => [...t].forEach((c, i) => h.setUint8(o + i, c.charCodeAt(0)));
      w(0, 'RIFF'); h.setUint32(4, 36 + pcmFrames * 4, true); w(8, 'WAVEfmt '); h.setUint32(16, 16, true); h.setUint16(20, 1, true);
      h.setUint16(22, 2, true); h.setUint32(24, rate, true); h.setUint32(28, rate * 4, true); h.setUint16(32, 4, true);
      h.setUint16(34, 16, true); w(36, 'data'); h.setUint32(40, pcmFrames * 4, true);
      await fetch('/audio', { method: 'POST', body: new Blob([h, ...pcm]) });
      print(`[web] audio: ${pcmFrames} stereo frames at ${rate} Hz, PCM hash ${(M._port_audio_pcm_hash() >>> 0).toString(16)}`);
    }
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
