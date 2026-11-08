// A headless retrowser host in node for the retrowser build (session S9, docs/port/RTW.md): the standard's start-up
// sequence (factory with wasmBinary / files / rtw imports, rtw_rom_buffer + the ROM in .z64 order, the stored save at
// rtw_save_ptr + rtw_save_load, rtw_start), then N retraces with scripted pad input, called directly
// (Module.rtwManual: no pacing, deterministic). No canvas: the renderer stays off, as in port-test.
//   node port/rtw/node_host.mjs BUILD_DIR ROM.z64 --frames N [--tap EVERY MASK] [--save FILE] [--out DIR]
// Writes DIR/log.txt and DIR/tasks.txt (the [task] [ovl] [audio] [save] lines) like port/tools/node_run.js, so a run
// of the template build can be compared with the unstripped link and with port-test of the harness build.
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { join, resolve } from 'node:path';
import vm from 'node:vm';

const args = process.argv.slice(2);
const opt = (n, d) => { const i = args.indexOf('--' + n); return i >= 0 ? args[i + 1] : d; };
const dir = resolve(args[0]), romPath = resolve(args[1]);
const frames = +opt('frames', 600), out = opt('out', null), saveIn = opt('save', null);
const ti = args.indexOf('--tap');
const tap = ti >= 0 ? [+args[ti + 1], Number(args[ti + 2])] : null;
const man = JSON.parse(readFileSync(join(dir, 'retrowser.json'), 'utf8'));
const lines = [];
const log = (s) => { lines.push(String(s)); };
globalThis.self = globalThis;
self.location = { href: 'file:///rtw/' };
vm.runInThisContext(readFileSync(join(dir, man.files.js.path), 'utf8'), { filename: man.files.js.path });
const files = {};
for (const f of man.files.data || []) files[f.path] = new Uint8Array(readFileSync(join(dir, f.path)));
let M;
const mem = () => M.HEAPU8;
const rtw = {
  rtw_log: (p, n) => { log(new TextDecoder().decode(mem().subarray(p, p + n))); return 0; },
  rtw_build_id: (p) => { mem().fill(0xAB, p, p + 32); return 0; },
  rtw_rumble: (port, on) => { log(`[host] rumble ${port} ${on}`); return 0; },
};
M = await self[man.files.js.factory]({ wasmBinary: readFileSync(join(dir, man.files.wasm.path)), rtw, files,
  print: log, printErr: log, rtwManual: true });
const rom = readFileSync(romPath);
const p = M._rtw_rom_buffer(rom.length);
M.HEAPU8.set(rom, p);
if (saveIn) {
  const sv = readFileSync(saveIn), sp = M._rtw_save_ptr();
  M.HEAPU8.set(sv.subarray(0, M._rtw_save_size()), sp);
  M._rtw_save_load(sp, Math.min(sv.length, M._rtw_save_size()));
}
M._port_rtw_test_log(1);   // the [task] lines, as port-test prints them
let err = null;
try { M._rtw_start(); } catch (e) { err = e; log('[host] rtw_start threw: ' + e.message); }
const t0 = performance.now();
if (!err) for (let f = 1; f <= frames; f++) {
  const pad = tap && tap[0] > 0 && f % tap[0] < 3 ? tap[1] : 0;
  M._rtw_input(0, pad, 0, 0);
  M._port_rtw_retrace();
}
log(`[host] ${frames} retraces in ${(performance.now() - t0).toFixed(0)} ms; save generation ${M._rtw_save_generation()}`);
if (out) {
  mkdirSync(out, { recursive: true });
  writeFileSync(join(out, 'log.txt'), lines.join('\n') + '\n');
  writeFileSync(join(out, 'tasks.txt'), lines.filter((l) => /^\[(task|ovl|audio|save)\]/.test(l)).join('\n') + '\n');
}
console.log(lines.filter((l) => /^rtw:|\[host\]/.test(l)).join('\n'));
process.exit(err ? 1 : 0);
