// port/gfx test worker: the retrowser model (the module runs in a worker, Module.canvas is an OffscreenCanvas).
importScripts('gfx_test.js');
const log = [];
onmessage = async (e) => {
  const canvas = e.data.canvas;
  const print = (t) => { log.push(t); postMessage({ type: 'log', text: t }); };
  let M, ptr;
  try {
    M = await createGfxTest({ canvas, print, printErr: print });
    ptr = M._gfx_test_run();
  } catch (err) {
    print('EXCEPTION ' + err + '\n' + (err && err.stack));
    return;
  }
  // the canvas as presented (GL) and the frame as RDRAM holds it after the write-back, for each test frame
  const grab = async (p) => {
    const rdram = M.HEAPU8.slice(p, p + 320 * 240 * 4);
    const shown = await canvas.convertToBlob({ type: 'image/png' });
    const oc = new OffscreenCanvas(320, 240);
    oc.getContext('2d').putImageData(new ImageData(new Uint8ClampedArray(rdram.buffer), 320, 240), 0, 0);
    return { shown, rdram: await oc.convertToBlob({ type: 'image/png' }) };
  };
  const f1 = await grab(ptr);
  let f2;
  try { f2 = await grab(M._gfx_test_run2()); } catch (err) { print('EXCEPTION ' + err + '\n' + (err && err.stack)); return; }
  postMessage({ type: 'done', shown: f1.shown, rdram: f1.rdram, shown2: f2.shown, rdram2: f2.rdram });
};
