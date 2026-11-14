// retrowser target only (session S9, docs/port/RTW.md): the host imports (standard 4.9) forwarded to Module.rtw, which
// the host passes to the factory, and the C side's calls into the loop driver (rtw_glue.js, a --pre-js).
addToLibrary({
  rtw_log: (p, n) => Module['rtw']['rtw_log'](p, n),
  rtw_build_id: (p) => Module['rtw']['rtw_build_id'](p),
  rtw_rumble: (port, on) => Module['rtw']['rtw_rumble'](port, on),
  rtw_option_changed: (id, value) => Module['rtw']['rtw_option_changed'](id, value),   // F1: the game's Options menu
  rtw_pointer_lock: (on) => Module['rtw']['rtw_pointer_lock'](on),   // F6: mouse aim

  port_rtw_host_frame: () => Module['rtwLoop'].hostFrame(),
  port_rtw_set_paused: (on) => Module['rtwLoop'].pause(on),
  port_rtw_started: () => Module['rtwLoop'].start(),
  port_rtw_resize: (w, h, dpr) => Module['rtwLoop'].resize(w, h, dpr),
  // files.data pokemonsnap.romfill (the template's fill table, offsets only) copied into the heap; the C side frees it
  port_rtw_romfill_table__deps: ['malloc'],
  port_rtw_romfill_table: (lenp) => {
    const f = Module['files'] && Module['files']['pokemonsnap.romfill'];
    if (!f || !f.length) return 0;
    const p = _malloc(f.length);
    HEAPU8.set(f, p);
    HEAP32[lenp >> 2] = f.length;
    return p;
  },
  // a plain error for the host's page (a wrong ROM): thrown out of rtw_start, which the host reports
  port_rtw_refuse: (p) => { throw new Error(UTF8ToString(p)); },
  port_rtw_refuse__deps: ['$UTF8ToString'],
});
