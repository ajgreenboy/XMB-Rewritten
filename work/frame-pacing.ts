// Frame pacing measurement (audit/FRAME-PACING.md).
//
//   bun work/frame-pacing.ts --exe <exe> [--size 3840x2160] [--swap 1|4|0] [--pacing 1|0] [--interval 100]
//                            [--gpu] [--rounds 3] [--keep] [--json]
//
// Runs the XMB once, live clock, XMB_FRAME_TRACE=<file> (the binary-ish dump the View writes at exit), scrolls the
// categories (LEFT/RIGHT) and a column (UP/DOWN) with one key every --interval ms (100 = a held d-pad, 250 = single
// presses), and reports over the moving window:
//   swap gap      swap-to-swap interval: p50/p95/p99/max and the count that differ from the median by > 2 ms
//   step          the animation step (dt) each frame advanced by: same statistics
//   content error content clock minus swap (display) clock, detrended: how far each frame's motion is from where it
//                 should be at the moment it is shown. Its p95 / max is the judder in milliseconds of motion.
//   cpu / gpu     paint CPU ms and, with --gpu (glFinish at the end of paintGL), CPU+GPU ms.
// --swap N sets XMB_SWAP_INTERVAL: 4 on a 239 Hz panel presents at ~60 Hz, standing in for the TV.
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { join } from 'node:path';

const argv = process.argv.slice(2);
const opt = (k: string, d: string) => { const i = argv.indexOf(k); return i >= 0 ? argv[i + 1] : d; };
const flag = (k: string) => argv.includes(k);
const exe = opt('--exe', ''); const size = opt('--size', '1920x1080'); const swap = opt('--swap', '1');
const pacing = opt('--pacing', '1'); const interval = Number(opt('--interval', '100')); const rounds = Number(opt('--rounds', '3'));
if (!exe || !existsSync(exe)) { console.error('--exe required'); process.exit(2); }

const seq: string[] = [];
for (let r = 0; r < rounds; r++) for (const k of ['RIGHT', 'RIGHT', 'RIGHT', 'RIGHT', 'RIGHT', 'RIGHT', 'LEFT', 'LEFT', 'LEFT', 'LEFT', 'LEFT', 'LEFT', 'DOWN', 'DOWN', 'DOWN', 'DOWN', 'UP', 'UP', 'UP', 'UP']) seq.push(k);
const warm = Math.ceil(Number(opt('--warm', '9000')) / interval);
const tokens = [`size=${size}`, ...Array(warm).fill('wait'), ...seq, ...Array(Math.ceil(1500 / interval)).fill('wait'), 'quit'];

const work = mkdtempSync(join('C:/dev/out', 'pace-'));
mkdirSync(join(work, 'data')); mkdirSync(join(work, 'tmp'));
const trace = join(work, 'frame.trace'), logPath = join(work, 'log.txt');
const env: Record<string, string | undefined> = {
  ...process.env, TEMP: join(work, 'tmp'), TMP: join(work, 'tmp'),
  XMB_SINGLE_INSTANCE: '0', XMB_STEAM_ARTWORK: '0', XMB_STEAM_ACTIVITY: '0', XMB_POWER_DRY_RUN: '1', XMB_AUDIO_DRY_RUN: '1',
  XMB_SCRIPT_FRAMELESS: '1', XMB_LOG: logPath, XMB_FRAME_TRACE: opt('--slow','')? '1' : trace, XMB_FRAME_TRACE_MS: opt('--slow','20'), XMB_SWAP_INTERVAL: swap, XMB_PACING: pacing,
  XMB_SCRIPT_INTERVAL_MS: String(interval), XMB_PACE_GPU: flag('--gpu') ? '1' : '0',
};
const run = spawnSync(exe, ['--dev-flash', 'C:/dev/rpcs3/bin/dev_flash', '--data-dir', join(work, 'data'), '--vsh-modules', 'C:/dev/xmb/fw493/vsh_module', '--script', tokens.join(',')], { env, timeout: 240000, encoding: 'utf8' });
if (run.error || run.status !== 0) { console.error('run failed', run.error?.message ?? run.status); process.exit(2); }

if (opt('--slow', '')) { const L = readFileSync(logPath, 'utf8').split(/\r?\n/); console.log(L.filter((l) => /frame-trace: SLOW|prewarm|script: /.test(l)).join('\n')); process.exit(0); }
type P = { at: number; cpu: number; moving: boolean; dt: number; gpu: number };
const swaps: number[] = []; const paints: P[] = [];
for (const line of readFileSync(trace, 'utf8').split(/\r?\n/)) {
  let m;
  if ((m = /frame swap:\s*(\d+)/.exec(line))) swaps.push(+m[1]);
  else if ((m = /frame paint:\s*(\d+)\s+(\d+)\s+([01])\s+([01])/.exec(line))) paints.push({ at: +m[1], cpu: +m[2], moving: m[4] === '1', dt: 0, gpu: 0 });
  else if ((m = /frame pace:\s*(\d+)\s+(\d+)\s+(\d+)/.exec(line))) { const p = paints[paints.length - 1]; if (p) { p.dt = +m[2]; p.gpu = +m[3]; } }
}
const q = (a: number[], p: number) => { const s = [...a].sort((x, y) => x - y); return s.length ? s[Math.max(0, Math.ceil(p * s.length) - 1)] : NaN; };
const st = (a: number[]) => { const med = q(a, .5); return { n: a.length, p50: med, p95: q(a, .95), p99: q(a, .99), max: Math.max(...a), over2ms: a.filter((x) => Math.abs(x - med) > 2).length }; };

// Moving window: first to last moving paint, one frame of margin each side.
const mi = paints.map((p, i) => (p.moving ? i : -1)).filter((i) => i >= 0);
if (!mi.length) { console.error('no moving frames in trace'); process.exit(2); }
const a = Math.max(0, mi[0] - 1), b = Math.min(paints.length - 1, mi[mi.length - 1] + 1);
const win = paints.slice(a, b + 1);
const t0 = win[0].at, t1 = win[win.length - 1].at;
const sw = swaps.filter((s) => s >= t0 && s <= t1 + 50e6);
const gaps = sw.slice(1).map((s, i) => (s - sw[i]) / 1e6);
const steps = win.map((p) => p.dt / 1e6).slice(1);
// Content error: cumulative step vs the swap that displayed each paint.
let si = 0, cum = 0; const err: number[] = [];
for (const p of win) {
  while (si < sw.length && sw[si] <= p.at + p.cpu) si++;
  if (si >= sw.length) break;
  cum += p.dt / 1e6; err.push(cum - (sw[si] - t0) / 1e6);
}
const med = q(err, .5); const dev = err.map((e) => Math.abs(e - med));
// Drift-free: remove the linear trend by a 31-frame running median so a refresh estimate error is not judder.
const detr = err.map((e, i) => { const w = err.slice(Math.max(0, i - 15), i + 16); return Math.abs(e - q(w, .5)); });
const cpu = win.map((p) => p.cpu / 1e6), gpu = win.filter((p) => p.gpu > 0).map((p) => p.gpu / 1e6);
const fmt = (o: any) => `n=${o.n} p50=${o.p50.toFixed(2)} p95=${o.p95.toFixed(2)} p99=${o.p99.toFixed(2)} max=${o.max.toFixed(2)} >2ms-from-median=${o.over2ms}`;
const out = {
  exe, size, swap, pacing, interval, windowMs: (t1 - t0) / 1e6, frames: win.length,
  swapGapMs: st(gaps), stepMs: st(steps),
  contentErrorMs: { p50abs: q(detr, .5), p95: q(detr, .95), p99: q(detr, .99), max: Math.max(...detr), over2ms: detr.filter((x) => x > 2).length },
  cpuMs: st(cpu), ...(gpu.length ? { gpuMs: st(gpu) } : {}),
};
if (flag('--json')) console.log(JSON.stringify(out));
else {
  console.log(`size=${size} swap=${swap} pacing=${pacing} interval=${interval} window=${out.windowMs.toFixed(0)} ms frames=${win.length}`);
  console.log(`  swap gap : ${fmt(out.swapGapMs)}`);
  console.log(`  step dt  : ${fmt(out.stepMs)}`);
  console.log(`  content error vs display (detrended |ms|): p50=${out.contentErrorMs.p50abs.toFixed(2)} p95=${out.contentErrorMs.p95.toFixed(2)} p99=${out.contentErrorMs.p99.toFixed(2)} max=${out.contentErrorMs.max.toFixed(2)} >2ms=${out.contentErrorMs.over2ms}`);
  console.log(`  paint cpu: ${fmt(out.cpuMs)}`);
  if (gpu.length) console.log(`  gpu       : ${fmt(out.gpuMs!)}`);
}
if (!flag('--keep')) { try { rmSync(work, { recursive: true, force: true }); } catch { /* scratch */ } } else console.log('kept', work);
