// Smoke run of the linked wasm module in node (session S2): node port/tools/node_run.js <module.js> <rom> [main args]
// Loads the module, hands the player's ROM to port_set_rom (the bytes stay in this process), runs main().
// Nothing is written to disk.
const path = require('path');
const fs = require('fs');
const [js, rom, ...args] = process.argv.slice(2);
if (!js || !rom) { console.error('usage: node node_run.js <module.js> <rom>'); process.exit(2); }
const factory = require(path.resolve(js));
factory({ print: s => console.log(s), printErr: s => console.error(s) }).then(m => {
  const data = fs.readFileSync(rom);
  const p = m._malloc(data.length);
  m.HEAPU8.set(data, p);
  if (m._port_set_rom(p, data.length) !== 0) { console.error('port_set_rom failed'); process.exit(1); }
  m._free(p);
  const rc = m.callMain(args);
  console.error(`[node_run] main returned ${rc}`);
});
