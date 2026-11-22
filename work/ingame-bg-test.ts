// In-game XMB backdrop test (audit/INGAME-BG-CAPTURE.md): launches the repo stand-in game (native-xmb/tests/overlay_standin.cpp,
// built with -DXMB_OVERLAY_BUILD_TESTS=ON or by cl) as the tracked title, opens the in-game XMB with a scripted PS tap, and samples
// the screen pixels BEHIND the XMB columns for ~1 s after the stand-in loses focus. The stand-in logs every frame it presents, so
// each sampled pixel can be matched to the game frame it came from: it must be the dimmed LAST GAME FRAME (colour and animation
// phase), never black, the desktop or another window.
//
//   bun work/ingame-bg-test.ts --exe C:/dev/out/ingame-bg/re-ps3-native.exe --standin C:/dev/out/ingame-bg-run2/standin.exe
//        [--variant none|minimize|pause|black|all] [--label after] [--tap 1|0] [--out C:/dev/out/ingame-bg-captures]
//
// Read-only on the screen; it only ever starts the XMB exe and stand-in it was given, and ends them by the exact PID it started.
// It never starts Steam or any real game: the XMB gets an explicit scratch --steam-root. Other XMB windows on the desktop would
// corrupt the samples, so it waits (up to --wait-minutes) until none is running.
import { cpSync, existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';

const arg = (name: string, fallback: string) => { const i = process.argv.indexOf(name); return i >= 0 ? process.argv[i + 1] : fallback; };
const exe = arg('--exe', 'C:/dev/out/ingame-bg/re-ps3-native.exe');
const standin = arg('--standin', 'C:/dev/out/ingame-bg-run2/standin.exe');
const label = arg('--label', 'after');
const tap = arg('--tap', '1');                      // XMB_INGAME_TAP: pins the new path in a scripted run (the old exe ignores it)
const outRoot = arg('--out', 'C:/dev/out/ingame-bg-captures');
const waitMinutes = Number(arg('--wait-minutes', '10'));
// --display N runs the XMB in --kiosk mode on Qt screen N (and the stand-in and sampler on that monitor): a desktop where other agents' XMB windows are not.
const display = arg('--display', '');
const device = arg('--device', '');                 // the sampler's screen DeviceName for that display, e.g. \.DISPLAY2
const at = arg('--at', '');                         // a virtual-desktop point on that monitor for the stand-in, e.g. -960,500
const variantArg = arg('--variant', 'all');
const flash = 'C:/dev/rpcs3/bin/dev_flash';
const dataSeed = arg('--data-seed', 'C:/dev/out/ingame-bg-run/data');
const here = import.meta.dir.replace(/\\/g, '/');
const sampler = `${here}/ingame-bg-sampler.ps1`;
const foreground = `${here}/ingame-bg-foreground.ps1`;
const win = (s: string) => s.split('/').join(String.fromCharCode(92));
const rep = (t: string, n: number) => Array(n).fill(t);
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));
let failures = 0;
const check = (name: string, ok: boolean, detail = '') => { console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? '  ' + detail : ''}`); if (!ok) failures++; };

function otherXmbProcesses(): string[] {
  const r = Bun.spawnSync(['powershell', '-NoProfile', '-Command', "Get-Process re-ps3-native -ErrorAction SilentlyContinue | ForEach-Object { $_.Id }"]);
  return new TextDecoder().decode(r.stdout).split(/\s+/).filter(Boolean);
}
async function waitForQuietDesktop() {
  const t0 = Date.now();
  for (;;) {
    const others = otherXmbProcesses();
    if (!others.length) return;
    if (Date.now() - t0 > waitMinutes * 60000) throw new Error(`other XMB instance(s) still running after ${waitMinutes} min: ${others.join(',')}`);
    console.log(`waiting: other XMB process(es) ${others.join(',')} may be in front; retrying in 10 s`);
    await sleep(10000);
  }
}

type Frame = { t: number; r: number; g: number; b: number; bar: number };
type Sample = { t: number; fg: string; bar: number; px: number[][] };

function parseFrames(path: string) {
  const frames: Frame[] = []; const inactive: number[] = [];
  for (const line of readFileSync(path, 'utf8').split('\n')) {
    if (line.startsWith('# inactive ')) { inactive.push(Number(line.slice(11))); continue; }
    const p = line.trim().split(' ').map(Number);
    if (p.length === 5 && p.every((n) => Number.isFinite(n))) frames.push({ t: p[0], r: p[1], g: p[2], b: p[3], bar: p[4] });
  }
  return { frames, inactive };
}
function parseSamples(path: string): Sample[] {
  return readFileSync(path, 'utf8').split(/\r?\n/).filter(Boolean).map((l) => {
    const c = l.split(',');
    const px: number[][] = [];
    for (let i = 3; i + 2 < c.length; i += 3) px.push([Number(c[i]), Number(c[i + 1]), Number(c[i + 2])]);
    return { t: Number(c[0]), fg: c[1], bar: Number(c[2]), px };
  });
}
const DIM = 0.2;   // 1 - int(255 * 0.8) / 255: what the in-game XMB leaves of the game
const TOL = 7;     // per channel, 8-bit
function dist(a: number[], b: number[]) { return Math.max(Math.abs(a[0] - b[0]), Math.abs(a[1] - b[1]), Math.abs(a[2] - b[2])); }
function expected(f: Frame, x: number, barW = 60) {   // what the point at screen x shows of frame f, dimmed
  const onBar = x >= f.bar && x < f.bar + barW;
  const src = onBar ? [255, 217, 26] : [f.r, f.g, f.b];
  return src.map((v) => v * DIM);
}

async function runVariant(variant: string): Promise<string | void> {
  if (!display) await waitForQuietDesktop(); else console.log(`other XMB processes on this machine: ${otherXmbProcesses().join(",") || "none"} (running on display ${display}, so they are not in front of the samples)`);
  const dir = `${outRoot}/${label}-${variant}`;
  rmSync(dir, { recursive: true, force: true });
  mkdirSync(`${dir}/data`, { recursive: true });
  mkdirSync(`${dir}/steamroot`, { recursive: true });   // explicit scratch Steam root: the XMB must never look at the real one
  for (const f of ['preferences.json', 'profiles.json', 'settings.ini']) if (existsSync(`${dataSeed}/${f}`)) cpSync(`${dataSeed}/${f}`, `${dir}/data/${f}`);
  const stubLog = `${dir}/stub.log`, frameLog = `${dir}/frames.log`;
  writeFileSync(`${dir}/data/apps.json`, JSON.stringify({ version: 1, apps: [{ id: 'bg-stub', title: 'BG Stub', kind: 'game', category: 'game', executable: win(standin),
    arguments: ['--borderless', ...(at ? ['--at', at] : []), '--seconds', '60', '--blur', variant, '--log', win(stubLog), '--frame-log', win(frameLog)] }] }, null, 2));
  const script = ['full', ...rep('wait', 10), 'ENTER', ...rep('wait', 14), 'ps', ...rep('wait', 12), 'quit'];
  const env: Record<string, string | undefined> = { ...process.env, XMB_SINGLE_INSTANCE: '0', XMB_STEAM_ARTWORK: '0', XMB_PS_LOG: '1', XMB_LOG: `${dir}/xmb.log`,
    XMB_NO_GAMEBOOT: '1', XMB_INGAME_TAP: tap };
  delete env.QT_QPA_PLATFORM; delete env.QT_PLUGIN_PATH;
  const sam = Bun.spawn(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', win(sampler), '-Dir', win(`${dir}/shots`), '-Ms', '36000', ...(device ? ['-Device', device] : [])], { stdout: 'ignore', stderr: 'ignore' });
  const xmb = Bun.spawn([exe, '--dev-flash', flash, '--data-dir', `${dir}/data`, '--no-intro', '--steam-root', `${dir}/steamroot`, ...(display ? ['--kiosk', '--display', display] : []), '--script', script.join(',')],
    { stdout: 'ignore', stderr: 'ignore', env });
  // The XMB launches the stand-in, and a title started by a background process cannot take the foreground: make sure the XMB we started is in front first.
  for (const ms of [1200, 2200, 3000]) setTimeout(() => { Bun.spawn(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', win(foreground), '-ProcessId', String(xmb.pid)], { stdout: 'ignore', stderr: 'ignore' }); }, ms);
  const killer = setTimeout(() => xmb.kill(), 60000);
  let enterAt = 0;
  const poll = setInterval(() => { if (!enterAt && existsSync(`${dir}/xmb.log`) && /ingame: enter reason/.test(readFileSync(`${dir}/xmb.log`, 'utf8'))) enterAt = Date.now(); }, 25);
  await xmb.exited; clearTimeout(killer); clearInterval(poll);
  await sam.exited;
  // The stand-in the XMB launched, by the pid it logged.
  const stub = existsSync(stubLog) ? readFileSync(stubLog, 'utf8') : '';
  const pid = /started pid=(\d+)/.exec(stub)?.[1];
  if (pid) Bun.spawnSync(['taskkill', '/PID', pid, '/F'], { stdout: 'ignore', stderr: 'ignore' });

  console.log(`\n== ${label} / ${variant}  (exe ${exe}, XMB_INGAME_TAP=${tap})`);
  if (!existsSync(frameLog) || !existsSync(`${dir}/shots/samples.csv`)) { check(`${variant}: stand-in frames and screen samples were recorded`, false); return; }
  const { frames, inactive } = parseFrames(frameLog);
  const samples = parseSamples(`${dir}/shots/samples.csv`);
  const xmbLog = existsSync(`${dir}/xmb.log`) ? readFileSync(`${dir}/xmb.log`, 'utf8') : '';
  for (const l of xmbLog.split('\n')) if (/ingame: (enter|overlay|tap|latched|using|frame tap|window|frozen)/.test(l)) console.log('   xmb: ' + l.trim().slice(0, 260));
  // T0: the stand-in's focus loss that belongs to the in-game XMB being raised (within 700 ms of the enter line). A focus loss long before
  // it means another application took the foreground first (another agent's window): the run is void, not failed.
  const t0 = inactive.find((i) => Math.abs(i - enterAt) < 700) ?? 0;
  if (!t0) { console.log(`   VOID: the stand-in lost focus at ${inactive.map((i) => i - enterAt).join(',')} ms relative to the in-game XMB enter; another window took the foreground (fg samples: ${[...new Set(samples.map((s) => s.fg))].join(' | ')})`); return 'void'; }
  check(`${variant}: the stand-in lost focus to the in-game XMB`, true, `enter-to-focus-loss ${t0 - enterAt} ms`);
  const xmbFront = samples.filter((s) => s.px.length >= 6 && s.t >= t0 + 150 && s.t <= t0 + 1700 && /Native XMB/.test(s.fg));
  check(`${variant}: the XMB is the foreground window in the sampling window`, xmbFront.length > 30, `${xmbFront.length} samples; foreground titles: ${[...new Set(samples.filter((s) => s.t >= t0 + 150 && s.t <= t0 + 1700).map((s) => s.fg))].join(' | ')}`);
  // Game frames the stand-in really presented (blank ones logged as 0 0 0 are not frames of the game).
  const good = frames.filter((f) => f.r + f.g + f.b > 0);
  const lastGood = good.filter((f) => f.t <= t0).slice(-1)[0];          // the frame on screen when focus was lost
  const xs = [60, 1860, 60, 1860, 300, 1600];                           // x of each sampled point (the sampler's default list)
  const circ = (a: number, b: number) => { const d = Math.abs(a - b) % 1920; return Math.min(d, 1920 - d); };
  // A sample is explained by a game frame when the bar sits where that frame had it (the exact clock: 0.25 px per ms) and the field
  // colour at every point the bar does not cover is that frame's, dimmed.
  const explains = (s: Sample, f: Frame) => {
    if (s.bar < 0 || circ(s.bar, f.bar) > 10) return false;
    return s.px.every((c, p) => (xs[p] >= f.bar - 6 && xs[p] < f.bar + 66) || dist(c, expected(f, xs[p])) <= TOL);
  };
  const candidates = (s: Sample) => variant === 'none' ? good.filter((f) => f.t >= t0 - 300 && f.t <= s.t + 60) : good.filter((f) => f.t >= t0 - 300 && f.t <= t0 + 80);
  let explained = 0; const rows: string[] = []; let firstMove = -1; let liveAges: number[] = [], bars: number[] = [];
  for (const s of xmbFront) {
    const hit = candidates(s).filter((f) => explains(s, f)).sort((a, b) => circ(s.bar, a.bar) - circ(s.bar, b.bar))[0];
    if (hit) explained++;
    if (bars.length && firstMove < 0 && circ(s.bar, bars[0]) > 25) firstMove = s.t - t0;
    bars.push(s.bar);
    if (hit && firstMove >= 0) liveAges.push(s.t - hit.t);
    rows.push(`${s.t - t0}ms bar=${s.bar} p0=${s.px[0]} frame=${hit ? `${hit.t - t0}ms bar=${hit.bar} ${hit.r},${hit.g},${hit.b}` : 'NONE'}`);
  }
  writeFileSync(`${dir}/match.txt`, rows.join('\n'));
  const barMin = Math.min(...bars), barMax = Math.max(...bars);
  console.log(`   samples ${xmbFront.length}, explained by a game frame ${explained}; bar x ${barMin}..${barMax}; ${variant === 'none' ? `first backdrop movement at t0+${firstMove} ms` : ''}`);
  check(`${variant}: every sampled pixel behind the XMB is a dimmed game frame (colour and bar phase), not black/desktop/another window`, xmbFront.length > 30 && explained >= xmbFront.length - (variant === 'none' ? 1 : 0), `${explained}/${xmbFront.length}${variant === 'none' ? ' (one sample may straddle the frozen-to-live switch: the strip and the points are read a few ms apart)' : ''}`);
  if (variant === 'none') {
    // A title that keeps running: frozen on the entry frame first, then live once frames are proven to keep arriving (~300 ms).
    const stale = liveAges.length ? liveAges.reduce((a, b) => a + b, 0) / liveAges.length : -1;
    check(`${variant}: the backdrop goes live while the title keeps drawing`, firstMove >= 150 && firstMove <= 1200 && barMax - barMin > 100, `first movement t0+${firstMove} ms, mean frame age ${stale.toFixed(0)} ms, bar swept ${barMax - barMin} px`);
  } else if (lastGood) {
    // The title blanked, paused or minimised: the backdrop is the frame that was on screen at focus loss, and it stays.
    const still = bars[0] >= 0 && bars.every((b) => circ(b, bars[0]) <= 3);
    const expectNow = (s: Sample) => (lastGood.bar + 0.25 * (s.t - lastGood.t)) % 1920;
    const late = xmbFront.filter((s) => s.t >= t0 + 700);
    const notNow = late.every((s) => circ(s.bar, expectNow(s)) > 60);   // had the game gone on, the bar would be 175+ px further along
    check(`${variant}: the backdrop is the last game frame, frozen (not the moving game)`, still && notNow, `still=${still} bar=${bars[0]} lastGood bar=${lastGood.bar} at t0${lastGood.t - t0 >= 0 ? '+' : ''}${lastGood.t - t0} ms`);
  }
  return 'ok';
}

mkdirSync(outRoot, { recursive: true });
const variants = variantArg === 'all' ? ['none', 'minimize', 'pause', 'black'] : variantArg.split(',');
for (const v of variants) {
  let result: string | void = 'void';
  for (let attempt = 1; attempt <= 6 && result === 'void'; attempt++) { if (attempt > 1) console.log(`   retrying ${v} (attempt ${attempt})`); result = await runVariant(v); }
  check(`${v}: a clean run was obtained (no other window took the foreground)`, result !== 'void');
}
console.log(failures ? `\n${failures} check(s) FAILED` : '\nall checks passed');
process.exit(failures ? 1 : 0);
