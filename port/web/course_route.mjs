// Scripted course routes for the browser harness (session S10a). Prints the run.mjs arguments for one course:
//   node port/web/course_route.mjs COURSE [--frames N] [--every N] [--no-play] [--play-end F] [--then INPUTS]
//   node port/web/run.mjs --out port_work/DIR --save SAVE.fla $(node port/web/course_route.mjs 3) [--audio]
// COURSE: 0 beach, 1 tunnel, 2 volcano, 3 river, 4 cave, 5 valley, 6 rainbow (include/enums.h; the lab's course list
// order, oaks_lab 8A8A50.c). Needs a save with that course open (port/save/snap_save.py --write ... --courses 6).
// Boot -> Continue -> lab: A every 120 until retrace 1000 (title, Continue, Oak's greeting, "Go to Course"); the
// course list is up by then: stick down COURSE times (every 40 retraces), A (course), A ("Let's go!"). In the course
// (unless --no-play): A every 60 retraces (zoomed out: throws an apple; zoomed in: takes a photo), Z held 400 of every
// 1200 retraces (zoom in: photos), B (pester ball) every 900, C-down (flute) every 1500. The rail ends the course by
// itself; the camera check that follows is not navigated (A taps continue into it) unless --play-end F stops the
// in-course inputs at retrace F and --then appends run.mjs --input entries (F:MASK[:LEN[:X[:Y]]],...).
const args = process.argv.slice(2);
const opt = (n, d) => { const i = args.indexOf('--' + n); return i >= 0 ? +args[i + 1] : d; };
const course = +args[0];
if (!(course >= 0 && course <= 6)) { console.error('usage: course_route.mjs COURSE(0..6) [--frames N] [--every N] [--no-play]'); process.exit(2); }
const frames = opt('frames', 14000), every = opt('every', 300), playEnd = opt('play-end', frames);
const A = 0x8000, B = 0x4000, Z = 0x2000, CD = 0x0004;
const inp = [];
let f = 1010;
for (let j = 0; j < course; j++, f += 40) inp.push(`${f}:0:3:0:-80`);
f += 60; inp.push(`${f}:${A}:3`);
f += 120; inp.push(`${f}:${A}:3`);
const start = f + 300;
if (!args.includes('--no-play')) {
  for (let t = start; t < playEnd; t += 60) inp.push(`${t}:${A}:3`);
  for (let t = start + 600; t < playEnd; t += 1200) inp.push(`${t}:${Z}:${Math.min(400, playEnd - t)}`);
  for (let t = start + 450; t < playEnd; t += 900) inp.push(`${t}:${B}:3`);
  for (let t = start + 750; t < playEnd; t += 1500) inp.push(`${t}:${CD}:3`);
}
const ti = args.indexOf('--then');
if (ti >= 0) inp.push(args[ti + 1]);
console.log(['--frames', frames, '--every', every, '--tap', 120, A, '--tap-end', 1000, '--input', inp.join(',')].join(' '));
