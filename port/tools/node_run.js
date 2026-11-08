// Run the linked wasm module in node (sessions S2-S3a):
//   node port/tools/node_run.js <module.js> <rom> [--out DIR] [main args: --frames N --bench --load-all]
// Loads the module, hands the player's ROM to port_set_rom (the bytes stay in this process), runs main().
// --out DIR (gitignored port_work/...): writes DIR/log.txt (everything), DIR/tasks.txt (the deterministic part:
// [task] [ovl] [stub] [os] [icall] lines, compared between runs), DIR/dl_hist.txt (the first display list's opcode
// histogram). Nothing is written without --out.
// --readback FILE (S6): replay a browser run's read-back journal (port/web/run.mjs writes OUT/readback.bin): the GPU
// write-backs node cannot compute are written at the same points, so node follows that run's game state (same build,
// same --frames/--tap/--input).
// --wav FILE (S7): capture every stereo frame the virtual AI played and write it as a 16-bit WAV at the AI's rate
// (ROM-derived: port_work/ only).
const path = require('path');
const fs = require('fs');
const argv = process.argv.slice(2);
const [js, rom] = argv;
let rest = argv.slice(2), out = null;
const oi = rest.indexOf('--out');
if (oi >= 0) { out = rest[oi + 1]; rest.splice(oi, 2); }
let wav = null;
const wi = rest.indexOf('--wav');
if (wi >= 0) { wav = rest[wi + 1]; rest.splice(wi, 2); }
let readback = null;
const ri = rest.indexOf('--readback');
if (ri >= 0) { readback = rest[ri + 1]; rest.splice(ri, 2); }
if (!js || !rom) { console.error('usage: node node_run.js <module.js> <rom> [--out DIR] [args]'); process.exit(2); }
const lines = [];
const log = s => { lines.push(s); console.error(s); };
const factory = require(path.resolve(js));
factory({ print: log, printErr: log }).then(m => {
  const data = fs.readFileSync(rom);
  const p = m._malloc(data.length);
  m.HEAPU8.set(data, p);
  if (m._port_set_rom(p, data.length) !== 0) { console.error('port_set_rom failed'); process.exit(1); }
  m._free(p);
  if (readback) {
    const j = fs.readFileSync(readback);
    const q = m._malloc(j.length);
    m.HEAPU8.set(j, q);
    if (m._port_rb_load(q, j.length) !== 0) { console.error('port_rb_load failed'); process.exit(1); }
    m._free(q);
  }
  if (wav) m._port_audio_capture(1);
  const rc = m.callMain(rest);
  log(`[node_run] main returned ${rc}`);
  if (wav) {
    const n = m._port_audio_capture_frames() >>> 0, q = m._port_audio_capture_ptr() >>> 0, rate = m._rtw_audio_rate();
    const h = Buffer.alloc(44);
    h.write('RIFF', 0); h.writeUInt32LE(36 + n * 4, 4); h.write('WAVEfmt ', 8); h.writeUInt32LE(16, 16);
    h.writeUInt16LE(1, 20); h.writeUInt16LE(2, 22); h.writeUInt32LE(rate, 24); h.writeUInt32LE(rate * 4, 28);
    h.writeUInt16LE(4, 32); h.writeUInt16LE(16, 34); h.write('data', 36); h.writeUInt32LE(n * 4, 40);
    fs.mkdirSync(path.dirname(wav), { recursive: true });
    fs.writeFileSync(wav, Buffer.concat([h, Buffer.from(m.HEAPU8.subarray(q, q + n * 4))]));
    log(`[node_run] wrote ${wav}: ${n} stereo frames at ${rate} Hz, PCM hash ${(m._port_audio_pcm_hash() >>> 0).toString(16)}`);
  }
  if (out) {
    fs.mkdirSync(out, { recursive: true });
    fs.writeFileSync(path.join(out, 'log.txt'), lines.join('\n') + '\n');
    const det = lines.filter(l => /^\[(task|ovl|stub|os|icall|mem|frame|sp|pi|boot)\]/.test(l));
    fs.writeFileSync(path.join(out, 'tasks.txt'), det.join('\n') + '\n');
    fs.writeFileSync(path.join(out, 'dl_hist.txt'), lines.filter(l => l.startsWith('[dlhist]')).join('\n') + '\n');
    console.error(`[node_run] wrote ${out}/{log,tasks,dl_hist}.txt`);
  }
});
