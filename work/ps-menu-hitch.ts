// Frame-time measurement for the first-use overlays (audit/PS-MENU-HITCH.md).
//
//   bun work/ps-menu-hitch.ts --exe <exe> [--size 3840x2160] [--opens 3]
//                             [--surface ps|ps-f10|options|message|osk|choice]
//                             [--slow-ms 20] [--threshold 30] [--assert] [--warmup-s 6] [--keep]
//
// Runs the XMB once in a scripted, live-clock run (no pinned clocks: the point is real frame time) with
// XMB_FRAME_TRACE=1, opens the chosen surface --opens times, and reports every frame from the first open on whose
// CPU time or swap gap passed --slow-ms, with the labels the build attached (a first open, a first decode, glyph
// tiles rasterised, an index build). With --assert it exits 1 when any such frame passes --threshold ms of CPU, so
// it can guard a regression where it is stable. Frame times belong to the machine: compare runs on one PC.
// XMB_OVERLAY_PREWARM=0 and XMB_PS_BLOOM_CACHE=0 in the environment give the build's old behaviour for an A/B.
//
// Scripted input only (the `pshold` and `F10` tokens stand in for the PS button); nothing here touches the live
// system: power and audio run dry, and no game or account row is pressed.
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { join } from 'node:path';

type Args = { slowMs: number; exe: string; size: string; opens: number; surface: string; threshold: number; assert: boolean; warmupS: number; keep: boolean; json: boolean };
function parse(): Args {
  const a: Args = { slowMs: 20, exe: '', size: '3840x2160', opens: 3, surface: 'ps', threshold: 30, assert: false, warmupS: 6, keep: false, json: false };
  const v = process.argv.slice(2);
  for (let i = 0; i < v.length; i++) {
    const k = v[i];
    if (k === '--exe') a.exe = v[++i];
    else if (k === '--size') a.size = v[++i];
    else if (k === '--opens') a.opens = Number(v[++i]);
    else if (k === '--surface') a.surface = v[++i];
    else if (k === '--slow-ms') a.slowMs = Number(v[++i]);
    else if (k === '--threshold') a.threshold = Number(v[++i]);
    else if (k === '--warmup-s') a.warmupS = Number(v[++i]);
    else if (k === '--assert') a.assert = true;
    else if (k === '--keep') a.keep = true;
    else if (k === '--json') a.json = true;
    else { console.error(`unknown argument ${k}`); process.exit(2); }
  }
  if (!a.exe || !existsSync(a.exe)) { console.error('--exe <re-ps3-native.exe> is required'); process.exit(2); }
  if (!Number.isFinite(a.opens) || a.opens < 1) { console.error('--opens must be >= 1'); process.exit(2); }
  return a;
}
const args = parse();

const wait = (n: number) => Array(n).fill('wait');
// One token every 300 ms. `pshold` presses PS: the watcher fires its hold at 1000 ms and the menu's staged
// entrance (black at 1052, text 1279..1326, bloom to 1840) runs from the press. `ps` is a tap, which closes it.
// Settings > System Settings is five LEFT and five DOWN from the start; its rows are Automatic Update, System
// Name (opens the on-screen keyboard), System Language (a choice list), ... Circle backs out of both without
// writing anything.
const settingsPage = ['LEFT', 'LEFT', 'LEFT', 'LEFT', 'LEFT', 'wait', 'DOWN', 'DOWN', 'DOWN', 'DOWN', 'DOWN', 'wait', 'ENTER', 'wait', 'wait'];
function surfaceTokens(surface: string): { prefix: string[]; open: string[] } {
  switch (surface) {
    case 'ps': return { prefix: [], open: ['pshold', ...wait(9), 'ps', ...wait(4)] };
    case 'ps-f10': return { prefix: [], open: ['F10', ...wait(8), 'ESC', ...wait(1), 'ESC', ...wait(4)] };
    // Triangle on the first root item opens its options panel; Triangle again closes it.
    case 'options': return { prefix: [], open: ['TRIANGLE', ...wait(5), 'TRIANGLE', ...wait(4)] };
    // "Turn Off the Controller" explains itself on a message page (nothing is switched off).
    case 'message': return { prefix: [], open: ['F10', ...wait(3), 'DOWN', 'ENTER', ...wait(5), 'ESC', ...wait(3), 'ESC', ...wait(2)] };
    case 'osk': return { prefix: [...settingsPage, 'DOWN', ...wait(2)], open: ['ENTER', ...wait(6), 'ESC', ...wait(4)] };
    case 'choice': return { prefix: [...settingsPage, 'DOWN', 'DOWN', 'DOWN', 'DOWN', 'DOWN', 'DOWN', 'DOWN', ...wait(2)], open: ['ENTER', ...wait(5), 'ESC', ...wait(4)] };
    default: console.error(`unknown --surface ${surface}`); process.exit(2);
  }
}
const plan = surfaceTokens(args.surface);
const tokens = [`size=${args.size}`, ...wait(Math.ceil(args.warmupS / 0.3)), ...plan.prefix];
for (let i = 0; i < args.opens; i++) tokens.push(...plan.open);
tokens.push('quit');

const work = mkdtempSync(join('C:/dev/out', 'pshitch-'));
mkdirSync(join(work, 'data'));
mkdirSync(join(work, 'tmp'));
const logPath = join(work, 'log.txt');
const env = {
  ...process.env,
  TEMP: join(work, 'tmp'), TMP: join(work, 'tmp'),
  XMB_SINGLE_INSTANCE: '0', XMB_STEAM_ARTWORK: '0', XMB_STEAM_ACTIVITY: '0',
  XMB_POWER_DRY_RUN: '1', XMB_AUDIO_DRY_RUN: '1', XMB_SCRIPT_FRAMELESS: '1',
  XMB_LOG: logPath, XMB_FRAME_TRACE: '1', XMB_FRAME_TRACE_MS: String(args.slowMs),
};
const run = spawnSync(args.exe, ['--dev-flash', 'C:/dev/rpcs3/bin/dev_flash', '--data-dir', join(work, 'data'),
  '--vsh-modules', 'C:/dev/xmb/fw493/vsh_module', '--script', tokens.join(',')], { env, timeout: 180000, encoding: 'utf8' });
if (run.error || run.status !== 0) { console.error('run failed', run.error?.message ?? run.status); process.exit(2); }

const lines = readFileSync(logPath, 'utf8').split(/\r?\n/);
type Slow = { line: number; t: number; cpu: number; gap: number; rebuild: number; labels: string[] };
const slow: Slow[] = [];
const seconds: string[] = [];
lines.forEach((line, i) => {
  const m = /frame-trace: SLOW t=(\d+) ms cpu=([\d.]+) ms gap=([\d.]+) ms rebuild=(\d) \[(.*)\]$/.exec(line);
  if (m) slow.push({ line: i, t: +m[1], cpu: +m[2], gap: +m[3], rebuild: +m[4], labels: m[5] ? m[5].split('; ') : [] });
  else if (line.includes('frame-trace: 1s')) seconds.push(line.replace(/^.*frame-trace: /, ''));
});
// The window starts at the first surface the script opens (the View logs every open and close at the start of that
// frame, and the slow-frame line for it comes after), so start-up, the window resize and the idle prewarm stay out
// of the numbers.
const firstOpenLine = lines.findIndex((l) => /frame-trace: surface .* opened/.test(l));
if (firstOpenLine < 0) { console.error('no surface was opened in this run (see the log)'); process.exit(2); }
const after = slow.filter((s) => s.line >= firstOpenLine);
const worst = after.reduce((m, s) => Math.max(m, s.cpu), 0);
const out = {
  exe: args.exe, size: args.size, surface: args.surface, opens: args.opens,
  slowFramesAfterWarmup: after.length, worstCpuMs: worst,
  worstGapMs: after.reduce((m, s) => Math.max(m, s.gap), 0),
  frames: after,
  perSecond: seconds,
};
if (args.json) console.log(JSON.stringify(out, null, 1));
else {
  console.log(`surface=${args.surface} size=${args.size} opens=${args.opens} exe=${args.exe}`);
  console.log(`slow frames (>=${args.slowMs} ms cpu or gap) from the first open on: ${after.length}; worst cpu ${worst.toFixed(1)} ms, worst gap ${out.worstGapMs.toFixed(1)} ms`);
  for (const s of after) console.log(`  t=${s.t} cpu=${s.cpu.toFixed(1)} gap=${s.gap.toFixed(1)} rebuild=${s.rebuild} [${s.labels.join('; ')}]`);
  const surfaceSeconds = seconds.filter((x) => /worst-cpu=\S+ ms/.test(x));
  console.log(`per-second worst cpu (all ${surfaceSeconds.length} seconds): ` +
    surfaceSeconds.map((x) => /worst-cpu=([\d.]+)/.exec(x)![1]).join(' '));
}
if (!args.keep) { try { rmSync(work, { recursive: true, force: true }); } catch { /* scratch only */ } }
else console.log(`kept ${work}`);
if (args.assert && worst > args.threshold) {
  console.error(`FAIL: worst frame ${worst.toFixed(1)} ms is over ${args.threshold} ms`);
  process.exit(1);
}
