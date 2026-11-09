// retrowser target only (session S9, docs/port/RTW.md): the loop driver of the self-running module, an Emscripten
// --pre-js. One port_rtw_retrace() is one NTSC field (60 Hz). Pacing (guide section 3, traps 13 and 14):
//  - the host calls rtw_input once per display frame; the port-0 call (rtw.c -> hostFrame) starts a retrace when one
//    is due (up to half a field early), in a microtask, so never inside the export the host called;
//  - one timer at most is outstanding. After every retrace it is armed for half a field after the next one is due:
//    while the host ticks at the display rate a display frame starts that retrace first and the timer is cleared
//    unfired (no wake-up); it fires only to catch up (a 30 Hz host, a slow frame) or when no display frames come
//    (100 ms without one: the module then steps at 60 Hz on that timer alone, like a module without host frames);
//  - paused (rtw_pause(1)): no timer at all; the clock restarts on resume.
// Module.rtwManual (tests only: port/rtw/node_host.mjs): no pacing, the test calls _port_rtw_retrace itself.
// Every 10 s a "rtw: pacing" log line gives retraces, display frames and timer wake-ups per second.
(function () {
  'use strict';
  const FIELD_MS = 1000 / 60;
  let next = 0;            // performance.now() at which the next retrace is due (0: start the clock)
  let lastHost = -1e9;     // performance.now() of the last display frame
  let timer = 0, queued = false, paused = false, running = false;
  let want = null;         // canvas size asked by rtw_resize, applied before the next retrace
  let view = null;         // the last rtw_resize [w, h, dpr]; sharp: the present mode the canvas was sized for (F2)
  let sharp = 0;
  const st = { retraces: 0, host: 0, timerFired: 0, timerRan: 0, late: 0, t0: 0 };
  const log = (s) => { try { (Module['print'] || console.log)(s); } catch (e) { /* */ } };

  function arm() {
    if (timer) { clearTimeout(timer); timer = 0; }
    if (paused || !running || Module['rtwManual']) return;
    const now = performance.now();
    const ticking = now - lastHost < 100;
    timer = setTimeout(onTimer, Math.max(0, (ticking ? next + FIELD_MS / 2 : next) - now));
  }
  function onTimer() {
    timer = 0;
    st.timerFired++;
    if (paused || !running) return;
    const now = performance.now();
    const ticking = now - lastHost < 100;
    if (ticking ? now + 1 >= next + FIELD_MS / 2 : now + 1 >= next) { st.timerRan++; retrace(now); }
    else arm();
  }
  function retrace(now) {
    if (!next) next = now;
    if (now - next > FIELD_MS) st.late++;
    next = Math.max(next + FIELD_MS, now - FIELD_MS);   // catch up at most one field, then follow the display
    // F2: option 2 "Pixels" (or the Options menu's live preview) changed: size the canvas for the new mode
    const sh = Module['_port_display_sharp'] ? Module['_port_display_sharp']() : 0;
    if (sh !== sharp) { sharp = sh; if (view) want = fit(view[0], view[1], view[2]); }
    if (want) {
      const c = Module['canvas'];
      if (c && (c.width !== want[0] || c.height !== want[1])) { c.width = want[0]; c.height = want[1]; log(`rtw: canvas ${c.width}x${c.height}`); }
      want = null;
    }
    Module['_port_rtw_retrace']();
    st.retraces++;
    if (st.retraces % 600 === 0) {
      const s = (now - st.t0) / 1000;
      log(`rtw: pacing ${st.retraces} retraces: ${(600 / s).toFixed(1)} retraces/s, ${(st.host / s).toFixed(1)} display ` +
        `frames/s, timer wake-ups ${(st.timerFired / s).toFixed(1)}/s (${st.timerRan} ran a retrace), ${st.late} late`);
      st.host = st.timerFired = st.timerRan = st.late = 0;
      st.t0 = now;
    }
    arm();
  }
  // Smooth (option 2 = 0): 4:3 like the console's picture (the host fits it with object-fit: contain), at least 320x240
  // and at most 1280x960 (as before F2). Sharp: the view's own shape and size in device pixels (at most 4096 a side),
  // so the host shows the canvas 1:1 and the browser adds no smoothing of its own; vi_frame letterboxes the 4:3
  // picture inside it.
  function fit(w, h, dpr) {
    const k = Math.max(1, dpr | 0) / 100;
    if (sharp) {
      const s = Math.min(1, 4096 / Math.max(w * k, h * k));
      return [Math.max(320, Math.round(w * k * s)), Math.max(240, Math.round(h * k * s))];
    }
    let cw = Math.min(w * k, h * k * 4 / 3);
    cw = Math.max(320, Math.min(1280, Math.round(cw / 4) * 4));
    return [cw, cw * 3 / 4];
  }
  Module['rtwLoop'] = {
    start() {
      running = true;
      st.t0 = performance.now();
      if (!Module['canvas'] || !Module['canvas'].width || Module['canvas'].width === 300) want = want || [640, 480];
      arm();
    },
    hostFrame() {
      const now = performance.now();
      st.host++;
      lastHost = now;
      if (paused || queued || !running || Module['rtwManual']) return;
      if (!next) next = now;
      if (now + FIELD_MS / 2 >= next) {
        queued = true;
        queueMicrotask(() => { queued = false; if (!paused) retrace(performance.now()); });
      }
    },
    pause(on) {
      paused = !!on;
      next = 0;
      if (paused) { if (timer) { clearTimeout(timer); timer = 0; } }
      else arm();
      log(`rtw: ${paused ? 'paused' : 'resumed'} at retrace ${st.retraces}`);
    },
    // the canvas follows the view (CSS px x devicePixelRatio); render-only (the N64 images stay at 320x240)
    resize(w, h, dpr) {
      view = [w, h, dpr];
      want = fit(w, h, dpr);
    },
  };
})();
