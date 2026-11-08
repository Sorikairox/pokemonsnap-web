// Objective checks of a WAV the port's virtual AI played (session S7; node, no packages):
//   node port/audio/tools/wav_report.mjs FILE.wav [--png OUT.png] [--from S] [--to S] [--cols N] [--tempo S0,S1 [--bpm LO,HI]] [--tuning S0,S1]
// Prints per-second level (RMS / peak dBFS), clipped samples, the dominant frequency of each second, and with
// --tempo the onset-strength autocorrelation tempo (BPM) of that time range. --png writes a spectrogram
// (log frequency 40 Hz..16 kHz bottom to top, time left to right, dB grey levels) of [from, to). WAVs and pictures
// made from the game's audio are ROM-derived: keep them in port_work/.
import { readFileSync, writeFileSync } from 'node:fs';
import { deflateSync } from 'node:zlib';

const args = process.argv.slice(2);
const opt = (n, d) => { const i = args.indexOf('--' + n); return i >= 0 ? args[i + 1] : d; };
const buf = readFileSync(args[0]);
let off = 12, rate = 0, ch = 0, data = null;
while (off + 8 <= buf.length) {
  const id = buf.toString('ascii', off, off + 4), len = buf.readUInt32LE(off + 4);
  if (id === 'fmt ') { ch = buf.readUInt16LE(off + 10); rate = buf.readUInt32LE(off + 12); }
  if (id === 'data') data = buf.subarray(off + 8, off + 8 + len);
  off += 8 + len + (len & 1);
}
const n = data.length / (2 * ch);
const mono = new Float32Array(n);
let clipped = 0, peakAll = 0, sumAll = 0;
for (let i = 0; i < n; i++) {
  let m = 0;
  for (let c = 0; c < ch; c++) {
    const s = data.readInt16LE(2 * (i * ch + c));
    if (s >= 32767 || s <= -32768) clipped++;
    peakAll = Math.max(peakAll, Math.abs(s));
    m += s;
  }
  mono[i] = m / ch / 32768;
  sumAll += mono[i] * mono[i];
}
const db = (x) => (x > 0 ? 20 * Math.log10(x) : -Infinity).toFixed(1);
console.log(`${args[0]}: ${n} frames, ${ch} ch, ${rate} Hz, ${(n / rate).toFixed(2)} s; peak ${db(peakAll / 32768)} dBFS, ` +
  `RMS ${db(Math.sqrt(sumAll / n))} dBFS, ${clipped} clipped samples`);

// FFT (radix 2, in place)
function fft(re, im) {
  const N = re.length;
  for (let i = 1, j = 0; i < N; i++) {
    let bit = N >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { [re[i], re[j]] = [re[j], re[i]]; [im[i], im[j]] = [im[j], im[i]]; }
  }
  for (let len = 2; len <= N; len <<= 1) {
    const a = -2 * Math.PI / len, wr = Math.cos(a), wi = Math.sin(a);
    for (let i = 0; i < N; i += len) {
      let cr = 1, ci = 0;
      for (let j = 0; j < len / 2; j++) {
        const ur = re[i + j], ui = im[i + j], k = i + j + len / 2;
        const vr = re[k] * cr - im[k] * ci, vi = re[k] * ci + im[k] * cr;
        re[i + j] = ur + vr; im[i + j] = ui + vi; re[k] = ur - vr; im[k] = ui - vi;
        const t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t;
      }
    }
  }
}
const W = 4096;
const hann = Float32Array.from({ length: W }, (_, i) => 0.5 - 0.5 * Math.cos(2 * Math.PI * i / W));
function spectrum(at) {
  const re = new Float32Array(W), im = new Float32Array(W);
  for (let i = 0; i < W; i++) re[i] = (mono[at + i] || 0) * hann[i];
  fft(re, im);
  const mag = new Float32Array(W / 2);
  for (let k = 0; k < W / 2; k++) mag[k] = Math.hypot(re[k], im[k]);
  return mag;
}

// per second: level, peak, dominant frequency (largest bin of the averaged spectrum above 40 Hz)
for (let s = 0; s * rate < n; s++) {
  const a = s * rate, b = Math.min(n, a + rate);
  let pk = 0, sum = 0;
  for (let i = a; i < b; i++) { pk = Math.max(pk, Math.abs(mono[i])); sum += mono[i] * mono[i]; }
  const avg = new Float32Array(W / 2);
  for (let at = a; at + W <= b; at += W / 2) { const m = spectrum(at); for (let k = 0; k < W / 2; k++) avg[k] += m[k]; }
  let best = 0;
  const k0 = Math.ceil(40 * W / rate);
  for (let k = k0; k < W / 2; k++) if (avg[k] > avg[best]) best = k;
  console.log(`  ${String(s).padStart(4)} s: RMS ${db(Math.sqrt(sum / (b - a))).padStart(6)} dBFS, peak ${db(pk).padStart(6)}, ` +
    `dominant ${(best * rate / W).toFixed(1)} Hz`);
}

const t0 = +opt('from', 0), t1 = +opt('to', n / rate);
if (opt('tempo')) {
  // onset strength: positive spectral flux per 512-sample hop, autocorrelated over 40..240 BPM
  const [a, b] = opt('tempo').split(',').map(Number);
  const hop = 512, frames = [];
  let prev = null;
  for (let at = Math.floor(a * rate); at + W <= Math.min(n, b * rate); at += hop) {
    const m = spectrum(at);
    let f = 0;
    if (prev) for (let k = 1; k < W / 2; k++) f += Math.max(0, Math.log1p(1000 * m[k]) - Math.log1p(1000 * prev[k]));
    frames.push(f);
    prev = m;
  }
  const mean = frames.reduce((x, y) => x + y, 0) / frames.length;
  const o = frames.map((x) => x - mean), fps = rate / hop;
  const res = [];
  const [blo, bhi] = (opt('bpm', '40,240')).split(',').map(Number);
  for (let bpm = blo; bpm <= bhi; bpm += 0.5) {
    const lag = 60 * fps / bpm, l0 = Math.floor(lag), fr = lag - l0;
    let c = 0;
    for (let i = 0; i + l0 + 1 < o.length; i++) c += o[i] * (o[i + l0] * (1 - fr) + o[i + l0 + 1] * fr);
    res.push([bpm, c]);
  }
  res.sort((x, y) => y[1] - x[1]);
  console.log(`tempo ${a}..${b} s: best ${res.slice(0, 5).map(([bpm, c]) => `${bpm} BPM (${c.toFixed(1)})`).join(', ')}`);
}

if (opt('tuning')) {
  // tuning: the strongest spectral peaks (parabolic interpolation, 100 Hz..4 kHz) of each 4096-sample window,
  // their offset from the nearest equal-tempered note (A4 = 440 Hz), histogram in 10-cent bins. A pitch error of
  // the resampler (anything but whole semitones) moves the histogram's centre away from 0.
  const [a, b] = opt('tuning').split(',').map(Number);
  const hist = new Array(10).fill(0);
  let sum = 0, cnt = 0;
  for (let at = Math.floor(a * rate); at + W <= Math.min(n, b * rate); at += W) {
    const m = spectrum(at);
    const peaks = [];
    for (let k = Math.ceil(100 * W / rate); k < 4000 * W / rate; k++)
      if (m[k] > m[k - 1] && m[k] >= m[k + 1]) peaks.push(k);
    peaks.sort((x, y) => m[y] - m[x]);
    for (const k of peaks.slice(0, 3)) {
      const l = Math.log(m[k - 1] + 1e-12), c = Math.log(m[k] + 1e-12), r = Math.log(m[k + 1] + 1e-12);
      const d = 0.5 * (l - r) / (l - 2 * c + r), f = (k + d) * rate / W;
      let cents = 1200 * Math.log2(f / 440);
      cents -= 100 * Math.round(cents / 100);
      hist[Math.min(9, Math.floor((cents + 50) / 10))]++;
      sum += cents; cnt++;
    }
  }
  console.log(`tuning ${a}..${b} s: ${cnt} peaks, mean offset ${(sum / cnt).toFixed(1)} cents; histogram -50..+50 by 10: ${hist.join(' ')}`);
}

const png = opt('png');
if (png) {
  const cols = +opt('cols', 1200), rows = 300, img = Buffer.alloc((cols * 3 + 1) * rows);
  const fmin = 40, fmax = 16000;
  for (let x = 0; x < cols; x++) {
    const at = Math.floor((t0 + (t1 - t0) * x / cols) * rate);
    const m = spectrum(at);
    for (let y = 0; y < rows; y++) {
      const f = fmin * Math.pow(fmax / fmin, (rows - 1 - y) / (rows - 1)), k = Math.min(W / 2 - 1, Math.round(f * W / rate));
      const v = Math.max(0, Math.min(255, Math.round((20 * Math.log10(m[k] + 1e-9) + 20) * 255 / 80)));
      const o = y * (cols * 3 + 1) + 1 + 3 * x;
      img[o] = v; img[o + 1] = v; img[o + 2] = Math.min(255, v + 40 * (y % 50 === 0));
    }
  }
  const crc = (b) => { let c = ~0; for (const x of b) { c ^= x; for (let k = 0; k < 8; k++) c = (c >>> 1) ^ (0xEDB88320 & -(c & 1)); } return ~c >>> 0; };
  const chunk = (t, d) => { const l = Buffer.alloc(4); l.writeUInt32BE(d.length); const td = Buffer.concat([Buffer.from(t), d]); const c = Buffer.alloc(4); c.writeUInt32BE(crc(td)); return Buffer.concat([l, td, c]); };
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(cols, 0); ihdr.writeUInt32BE(rows, 4); ihdr[8] = 8; ihdr[9] = 2;
  writeFileSync(png, Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr), chunk('IDAT', deflateSync(img)), chunk('IEND', Buffer.alloc(0))]));
  console.log(`spectrogram ${t0}..${t1} s -> ${png} (${cols}x${rows}; blue lines every 50 rows: ` +
    `${[0, 50, 100, 150, 200, 250].map((y) => Math.round(fmin * Math.pow(fmax / fmin, (rows - 1 - y) / (rows - 1)))).join(', ')} Hz)`);
}
