// Contact sheet of harness screenshots (session S10a): node only (zlib), no packages.
//   node port/web/sheet.mjs OUT.png [--cols 4] [--scale 2] A.png B.png ...
// Reads 8-bit RGB/RGBA non-interlaced PNGs (what the harness writes), shrinks each by --scale (box filter), tiles them
// left to right, top to bottom, with a 2-pixel grey gap, and writes one RGB PNG. Inputs and output are ROM-derived
// pictures: keep them in port_work/.
import { readFileSync, writeFileSync } from 'node:fs';
import { inflateSync, deflateSync } from 'node:zlib';

const args = process.argv.slice(2);
const take = (n, d) => { const i = args.indexOf(n); if (i < 0) return d; const v = args[i + 1]; args.splice(i, 2); return v; };
const cols = +take('--cols', 4), scale = +take('--scale', 2);
const [out, ...ins] = args;
if (!out || !ins.length) { console.error('usage: node port/web/sheet.mjs OUT.png [--cols 4] [--scale 2] IN.png...'); process.exit(2); }

function decode(buf) {
  let o = 8, w, h, ct, idat = [];
  while (o < buf.length) {
    const len = buf.readUInt32BE(o), type = buf.toString('latin1', o + 4, o + 8), d = buf.subarray(o + 8, o + 8 + len);
    if (type === 'IHDR') { w = d.readUInt32BE(0); h = d.readUInt32BE(4); ct = d[9]; if (d[8] !== 8 || d[12]) throw new Error('unsupported PNG'); }
    else if (type === 'IDAT') idat.push(d);
    o += 12 + len;
  }
  const bpp = ct === 6 ? 4 : ct === 2 ? 3 : 0;
  if (!bpp) throw new Error('unsupported colour type ' + ct);
  const raw = inflateSync(Buffer.concat(idat)), stride = w * bpp, px = Buffer.alloc(h * stride);
  for (let y = 0; y < h; y++) {
    const f = raw[y * (stride + 1)], src = raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1));
    for (let x = 0; x < stride; x++) {
      const a = x >= bpp ? px[y * stride + x - bpp] : 0, b = y ? px[(y - 1) * stride + x] : 0;
      const c = x >= bpp && y ? px[(y - 1) * stride + x - bpp] : 0;
      let v = src[x];
      if (f === 1) v += a; else if (f === 2) v += b; else if (f === 3) v += (a + b) >> 1;
      else if (f === 4) { const p = a + b - c, pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c); v += pa <= pb && pa <= pc ? a : pb <= pc ? b : c; }
      px[y * stride + x] = v & 255;
    }
  }
  return { w, h, bpp, px };
}

const imgs = ins.map((f) => decode(readFileSync(f)));
const tw = Math.max(...imgs.map((i) => Math.floor(i.w / scale))), th = Math.max(...imgs.map((i) => Math.floor(i.h / scale)));
const gap = 2, rows = Math.ceil(imgs.length / cols), W = cols * (tw + gap), H = rows * (th + gap);
const sheet = Buffer.alloc(W * H * 3, 96);
imgs.forEach((im, k) => {
  const ox = (k % cols) * (tw + gap), oy = Math.floor(k / cols) * (th + gap);
  for (let y = 0; y < Math.floor(im.h / scale); y++) for (let x = 0; x < Math.floor(im.w / scale); x++) {
    for (let c = 0; c < 3; c++) {
      let s = 0;
      for (let dy = 0; dy < scale; dy++) for (let dx = 0; dx < scale; dx++) s += im.px[((y * scale + dy) * im.w + x * scale + dx) * im.bpp + c];
      sheet[((oy + y) * W + ox + x) * 3 + c] = Math.round(s / (scale * scale));
    }
  }
});
const crcT = new Int32Array(256).map((_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; return c; });
const crc = (b) => { let c = -1; for (const x of b) c = crcT[(c ^ x) & 255] ^ (c >>> 8); return (c ^ -1) >>> 0; };
const chunk = (t, d) => { const l = Buffer.alloc(4); l.writeUInt32BE(d.length); const td = Buffer.concat([Buffer.from(t, 'latin1'), d]); const c = Buffer.alloc(4); c.writeUInt32BE(crc(td)); return Buffer.concat([l, td, c]); };
const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(W, 0); ihdr.writeUInt32BE(H, 4); ihdr[8] = 8; ihdr[9] = 2;
const raw = Buffer.alloc(H * (W * 3 + 1));
for (let y = 0; y < H; y++) sheet.copy(raw, y * (W * 3 + 1) + 1, y * W * 3, (y + 1) * W * 3);
writeFileSync(out, Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr), chunk('IDAT', deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]));
console.log(`${out}: ${imgs.length} pictures, ${cols} x ${rows}, ${W}x${H}`);
