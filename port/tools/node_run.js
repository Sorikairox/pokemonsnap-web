// Run the linked wasm module in node (sessions S2-S3a):
//   node port/tools/node_run.js <module.js> <rom> [--out DIR] [main args: --frames N --bench --load-all]
// Loads the module, hands the player's ROM to port_set_rom (the bytes stay in this process), runs main().
// --out DIR (gitignored port_work/...): writes DIR/log.txt (everything), DIR/tasks.txt (the deterministic part:
// [task] [ovl] [stub] [os] [icall] lines, compared between runs), DIR/dl_hist.txt (the first display list's opcode
// histogram). Nothing is written without --out.
const path = require('path');
const fs = require('fs');
const argv = process.argv.slice(2);
const [js, rom] = argv;
let rest = argv.slice(2), out = null;
const oi = rest.indexOf('--out');
if (oi >= 0) { out = rest[oi + 1]; rest.splice(oi, 2); }
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
  const rc = m.callMain(rest);
  log(`[node_run] main returned ${rc}`);
  if (out) {
    fs.mkdirSync(out, { recursive: true });
    fs.writeFileSync(path.join(out, 'log.txt'), lines.join('\n') + '\n');
    const det = lines.filter(l => /^\[(task|ovl|stub|os|icall|mem|frame|sp|pi|boot)\]/.test(l));
    fs.writeFileSync(path.join(out, 'tasks.txt'), det.join('\n') + '\n');
    fs.writeFileSync(path.join(out, 'dl_hist.txt'), lines.filter(l => l.startsWith('[dlhist]')).join('\n') + '\n');
    console.error(`[node_run] wrote ${out}/{log,tasks,dl_hist}.txt`);
  }
});
