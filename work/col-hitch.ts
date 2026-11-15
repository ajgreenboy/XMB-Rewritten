// First-visit column hitch measurement (audit/COLUMN-HITCH.md).
const SLOWRE = /frame-trace: SLOW t=\d+ ms cpu=([\d.]+) ms gap=([\d.]+) ms rebuild=\d \[(.*)\]$/;
//   bun work/col-hitch.ts --exe <exe> [--size 3840x2160] [--slow-ms 8] [--warm-s 12] [--cats 12] [--interval 300] [--runs 1]
// Live clock, XMB_FRAME_TRACE=1. After the warm-up (idle prewarm window) it scrolls RIGHT through --cats categories,
// pausing on each to press DOWN x2 / UP x2 (the column's items), reports every frame over --slow-ms from the first key on.
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { join } from 'node:path';
const argv = process.argv.slice(2);
const opt = (k: string, d: string) => { const i = argv.indexOf(k); return i >= 0 ? argv[i + 1] : d; };
const exe = opt('--exe', ''), size = opt('--size', '3840x2160'), slow = Number(opt('--slow-ms', '8'));
const warmS = Number(opt('--warm-s', '20')), cats = Number(opt('--cats', '12')), interval = Number(opt('--interval', '300'));
const runs = Number(opt('--runs', '1')), verbose = argv.includes('--verbose');
if (!exe || !existsSync(exe)) { console.error('--exe required'); process.exit(2); }
const w = (n: number) => Array(n).fill('wait');
const tokens = [`size=${size}`, ...w(Math.ceil(warmS * 1000 / interval)), 'wait'];
tokens.push('MARK');
for (let c = 0; c < cats; c++) tokens.push(opt('--dir', 'RIGHT'), ...w(2), 'DOWN', 'DOWN', 'UP', 'UP', ...w(1));
tokens.push(...w(3), 'quit');
const results: any[] = [];
for (let r = 0; r < runs; r++) {
  const work = mkdtempSync(join('C:/dev/out', 'colhitch-'));
  mkdirSync(join(work, 'data')); mkdirSync(join(work, 'tmp'));
  const logPath = join(work, 'log.txt');
  const env = { ...process.env, TEMP: join(work, 'tmp'), TMP: join(work, 'tmp'), XMB_SINGLE_INSTANCE: '0', XMB_STEAM_ARTWORK: '0',
    XMB_STEAM_ACTIVITY: '0', XMB_POWER_DRY_RUN: '1', XMB_AUDIO_DRY_RUN: '1', XMB_SCRIPT_FRAMELESS: '1', XMB_LOG: logPath,
    XMB_FRAME_TRACE: '1', XMB_FRAME_TRACE_MS: String(slow), XMB_SCRIPT_INTERVAL_MS: String(interval) };
  const toks = tokens.filter((t) => t !== 'MARK');
  const run = spawnSync(exe, ['--dev-flash', 'C:/dev/rpcs3/bin/dev_flash', '--data-dir', join(work, 'data'), '--vsh-modules', 'C:/dev/xmb/fw493/vsh_module', '--script', toks.join(',')], { env, timeout: 240000, encoding: 'utf8' });
  if (run.error || run.status !== 0) { console.error('run failed', run.error?.message ?? run.status); process.exit(2); }
  const L = readFileSync(logPath, 'utf8').split(/\r?\n/);
  // window: from the first key token logged after warm-up
  const last = (re: RegExp) => L.map((l, i) => (re.test(l) ? i : -1)).filter((i) => i >= 0).pop() ?? -1;
  const warmStart = last(/prewarm: stage 9 done/); const colDone = last(/prewarm: column text done/);
  let start = colDone >= 0 ? colDone : warmStart;
  if (start < 0) start = 0;
  const frames0: { line: number; cpu: number; gap: number; labels: string }[] = [];
  L.forEach((l, i) => { const m = SLOWRE.exec(l); if (m) frames0.push({ line: i, cpu: +m[1], gap: +m[2], labels: m[3] }); });
  const frames = frames0.filter((f) => f.line >= start && !/prewarm stage/.test(f.labels));
  const over = frames.filter((f) => f.cpu >= slow);
  const worst = Math.max(0, ...frames.map((f) => f.cpu));
  results.push({ start, over: over.length, worst, frames });
  const warmFrames = frames0.filter((f) => f.line >= warmStart && f.line < start && f.cpu >= slow);
  console.log(`warm phase (stage 9 .. column text done): frames cpu>=${slow}ms: ${warmFrames.length}, worst ${Math.max(0, ...warmFrames.map((f) => f.cpu)).toFixed(1)} ms; log: ${L.filter((l) => /prewarm: column/.test(l)).join(" / ")}`);
  console.log(`run ${r + 1} ${size}: start-line ${start}; frames cpu>=${slow}ms: ${over.length}; worst cpu ${worst.toFixed(1)} ms; worst gap ${Math.max(0, ...frames.map((f) => f.gap)).toFixed(1)}`);
  if (verbose || runs === 1) for (const f of frames) console.log(`  cpu=${f.cpu.toFixed(1)} gap=${f.gap.toFixed(1)} [${f.labels}]`);
  try { rmSync(work, { recursive: true, force: true }); } catch {}
}
