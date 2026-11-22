// Launch cover end to end (audit/GAME-LAUNCH-FADE.md), without a real game. The XMB itself launches the stand-in
// native-xmb/tests/launch_fade_standin.cpp (an imported game app of our own, never shipped) through the real game boot,
// and the stand-in records, from its very first instant, which window is on top over the monitor centre and that pixel.
// The oracles are two logs on one clock (milliseconds since the Unix epoch): the XMB's "launch-cover:" lines and the
// stand-in's "started/probe/shown" lines. It takes the XMB's monitor for about 15 s per case. Nothing else is started.
//
//   bun work/launch-fade-e2e.ts [--exe C:/dev/out/launch-fade/re-ps3-native.exe]
//       [--standin C:/dev/out/launch-fade/xmb-launch-fade-standin.exe] [--cases after,fullscreen,never,exit,press]
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';

const arg = (name: string, fallback: string) => { const i = process.argv.indexOf(name); return i >= 0 ? process.argv[i + 1] : fallback; };
const exe = arg('--exe', 'C:/dev/out/launch-fade/re-ps3-native.exe');
const standin = arg('--standin', 'C:/dev/out/launch-fade/xmb-launch-fade-standin.exe');
const flash = arg('--dev-flash', 'C:/dev/rpcs3/bin/dev_flash');
const cases = arg('--cases', 'after,fullscreen,never,exit,press').split(',');
const out = arg('--out', 'C:/dev/out/launch-fade-e2e');
const COVER = 'XMB launch cover', STANDIN = 'Launch Fade Standin';
let failures = 0;
const check = (label: string, ok: boolean, detail = '') => { console.log(`${ok ? 'PASS' : 'FAIL'}  ${label}${detail ? '  ' + detail : ''}`); if (!ok) failures++; };
const win = (s: string) => s.split('/').join(String.fromCharCode(92));
const sleep = (ms: number) => new Promise(r => setTimeout(r, ms));

for (const f of [exe, standin]) if (!existsSync(f)) { console.error(`missing ${f}`); process.exit(2); }

type Probe = { at: number; top: string; pixel: number[]; fg: number };
function standinLog(path: string) {
  const text = existsSync(path) ? readFileSync(path, 'utf8') : '';
  const num = (re: RegExp) => { const m = re.exec(text); return m ? Number(m[1]) : NaN; };
  const probes: Probe[] = [...text.matchAll(/probe at=(\d+) top="([^"]*)" toppid=\d+ pixel=(-?\d+),(-?\d+),(-?\d+) foreground=(\d)/g)]
    .map(m => ({ at: Number(m[1]), top: m[2], pixel: [Number(m[3]), Number(m[4]), Number(m[5])], fg: Number(m[6]) }));
  return { text, probes, started: num(/started pid=\d+ at=(\d+)/), shown: num(/shown at=(\d+)/), fullscreen: num(/fullscreen at=(\d+)/),
    exited: num(/exit at=(\d+)/), pids: [...text.matchAll(/started pid=(\d+)/g)].map(m => m[1]) };
}
function coverLog(text: string) {
  const at = (re: RegExp) => { const m = re.exec(text); return m ? Number(m[1]) : NaN; };
  const line = (re: RegExp) => (re.exec(text) || [''])[0];
  return {
    begin: line(/launch-cover: begin [^\n]*/), beginAt: at(/launch-cover: begin [^\n]* at=(\d+)/),
    opaqueAt: at(/launch-cover: opaque=1; starting the title at=(\d+)/),
    seen: line(/launch-cover: window seen [^\n]*/),
    fade: line(/launch-cover: fade-out [^\n]*/), fadeAt: at(/launch-cover: fade-out [^\n]* at=(\d+)/),
    done: line(/launch-cover: done [^\n]*/), doneAt: at(/launch-cover: done [^\n]* at=(\d+)/),
    handoff: line(/launch-cover: foreground handed [^\n]*/),
  };
}
const black = (p: number[]) => p.every(v => v >= 0 && v <= 8);
const fill = (p: number[]) => Math.abs(p[0] - 0) <= 12 && Math.abs(p[1] - 200) <= 12 && Math.abs(p[2] - 255) <= 12;
const between = (p: number[]) => p[1] > 20 && p[1] < 185 && p[2] > 25 && p[2] < 235;

function seedData(dir: string, standinArgs: string[], logPath: string) {
  rmSync(dir, { recursive: true, force: true });
  mkdirSync(`${dir}/data`, { recursive: true }); mkdirSync(`${dir}/no-steam`, { recursive: true });
  writeFileSync(`${dir}/data/preferences.json`, JSON.stringify({ version: 1, values: { 'display/fullscreen': true } }, null, 2));
  writeFileSync(`${dir}/data/apps.json`, JSON.stringify({ version: 1, apps: [{ id: 'launch-fade', title: 'Launch Fade Probe', kind: 'game', category: 'game',
    executable: win(standin), arguments: [...standinArgs, '--log', win(logPath)] }] }, null, 2));
}

async function runXmb(dir: string, script: string[], env: Record<string, string>, timeoutMs: number) {
  const log = `${dir}/xmb.log`;
  const p = Bun.spawn([exe, '--dev-flash', flash, '--data-dir', `${dir}/data`, '--focus-time', '2000', '--script', script.join(',')],
    { stdout: 'ignore', stderr: 'pipe', env: { ...process.env, XMB_SINGLE_INSTANCE: '0', XMB_STEAM_ROOT: `${dir}/no-steam`, XMB_STEAM_ARTWORK: '0', XMB_LOG: log, ...env } });
  const stderr = new Response(p.stderr).text();
  const timer = setTimeout(() => p.kill(), timeoutMs);
  await p.exited; clearTimeout(timer);
  const text = (existsSync(log) ? readFileSync(log, 'utf8') : '') + '\n' + await stderr;
  return text;
}

// Where the probe row sits in the Game column (the run starts there; the custom row's place depends on the column).
async function discoverRow(): Promise<number> {
  const dir = `${out}/discovery`;
  seedData(dir, ['--never-show', '--max-seconds', '1'], `${dir}/unused.log`);
  const dump = `${dir}/entries.json`;
  await runXmb(dir, ['full', ...Array(22).fill('wait'), 'quit'], { XMB_DUMP_ENTRIES: dump }, 90000);
  const json = JSON.parse(readFileSync(dump, 'utf8'));
  const game = (json.categories || []).find((c: any) => c.id === 'game');
  const index = (game?.entries || []).findIndex((e: any) => e.id === 'custom:launch-fade');
  if (index < 0) { console.error(`custom:launch-fade not in the Game column: ${(game?.entries || []).map((e: any) => e.id).join(', ')}`); process.exit(2); }
  return index;
}

async function runCase(name: string, row: number, standinArgs: string[], after: string[], env: Record<string, string> = {}) {
  const dir = `${out}/${name}`;
  const slog = `${dir}/standin.log`;
  seedData(dir, [...standinArgs, '--max-seconds', '25'], slog);
  const script = ['full', ...Array(12).fill('wait'), ...Array(row).fill('DOWN'), 'wait', 'ENTER', ...after, 'quit'];
  const text = await runXmb(dir, script, env, 120000);
  const s = standinLog(slog);
  for (const pid of s.pids) Bun.spawnSync(['taskkill', '/PID', pid, '/F'], { stdout: 'ignore', stderr: 'ignore' });
  writeFileSync(`${dir}/cover.log`, (text.match(/[^\n]*(launch-cover|gameboot):[^\n]*/g) || []).join('\n') + '\n');
  return { c: coverLog(text), s, text };
}

function commonChecks(name: string, r: Awaited<ReturnType<typeof runCase>>) {
  const { c, s } = r;
  check(`${name}: the cover came up right after the game boot, already black`, /alreadyBlack=1/.test(c.begin), c.begin.slice(0, 160));
  check(`${name}: opaque before the title was started (cover log <= stand-in start)`, c.opaqueAt <= s.started, `opaque ${c.opaqueAt} started ${s.started} (${s.started - c.opaqueAt} ms)`);
  const first = s.probes[0];
  check(`${name}: the stand-in's first look saw the cover on top`, !!first && first.top === COVER, first ? `top="${first.top}"` : 'no probe');
  check(`${name}: ... and a black screen`, !!first && black(first.pixel), first ? `pixel ${first.pixel}` : '');
}

const row = await discoverRow();
console.log(`probe row index in the Game column: ${row}`);

if (cases.includes('after')) {
  const r = await runCase('after', row, ['--show-after-ms', '1500', '--windowed', '1280x720'], ['pause=9000']);
  const { c, s } = r;
  commonChecks('after', r);
  check('after: the window was seen and the cover faded once it was stable', /reason=window-stable/.test(c.fade), c.fade.slice(0, 160));
  check('after: the fade began at least the settle time after the window showed', c.fadeAt - s.shown >= 450, `${c.fadeAt - s.shown} ms after shown`);
  check('after: the window was windowed', /mode=windowed/.test(c.fade));
  const held = s.probes.filter(p => p.at > s.shown + 100 && p.at < c.fadeAt - 30);
  check('after: the cover stayed on top of the new window until the fade', held.length > 3 && held.every(p => p.top === COVER), `${held.length} probes, tops ${[...new Set(held.map(p => p.top))].join('|')}`);
  // The centre of the monitor is inside the 1280x720 window at (80,80) on the XMB's monitor (1920x1080 or larger).
  const fading = s.probes.filter(p => p.at > c.fadeAt + 20 && p.at < c.doneAt - 20);
  check('after: intermediate pixels while fading (a fade, not a cut)', fading.some(p => between(p.pixel)), fading.map(p => p.pixel.join('/')).join(' '));
  const afterDone = s.probes.filter(p => p.at > c.doneAt + 150);
  check('after: then the stand-in is on top with its own colour', afterDone.length > 3 && afterDone.every(p => p.top === STANDIN && fill(p.pixel)),
    afterDone.slice(0, 3).map(p => `${p.top}:${p.pixel}`).join(' '));
  check('after: the stand-in holds the foreground after the hand-off', afterDone.length > 0 && afterDone[afterDone.length - 1].fg === 1, c.handoff);
  check('after: fade length ~400 ms', c.doneAt - c.fadeAt >= 350 && c.doneAt - c.fadeAt <= 900, `${c.doneAt - c.fadeAt} ms`);
}
if (cases.includes('fullscreen')) {
  const r = await runCase('fullscreen', row, ['--show-after-ms', '600', '--fullscreen-after-ms', '300'], ['pause=9000']);
  const { c, s } = r;
  commonChecks('fullscreen', r);
  check('fullscreen: the switch to fullscreen restarted the settle clock', c.fadeAt - s.fullscreen >= 450, `${c.fadeAt - s.fullscreen} ms after the switch`);
  check('fullscreen: settled as a borderless monitor-sized window', /reason=window-stable[^\n]*mode=borderless/.test(c.fade), c.fade.slice(0, 160));
  const held = s.probes.filter(p => p.at > s.shown + 50 && p.at < c.fadeAt - 30);
  check('fullscreen: the windowed-then-fullscreen switch happened behind the cover', held.length > 3 && held.every(p => p.top === COVER), `${held.length} probes`);
  const afterDone = s.probes.filter(p => p.at > c.doneAt + 150);
  check('fullscreen: then the stand-in fills the screen', afterDone.length > 3 && afterDone.every(p => p.top === STANDIN && fill(p.pixel)));
}
if (cases.includes('never')) {
  const r = await runCase('never', row, ['--never-show'], ['pause=10000'], { XMB_LAUNCH_COVER_TIMEOUT_MS: '4000' });
  const { c, s } = r;
  commonChecks('never', r);
  check('never: no window -> the no-window timeout', /reason=no-window-timeout/.test(c.fade), c.fade.slice(0, 160));
  check('never: the timeout ran from the moment the cover was opaque (~4 s)', c.fadeAt - c.beginAt >= 3900 && c.fadeAt - c.beginAt <= 5000, `${c.fadeAt - c.beginAt} ms`);
  check('never: the cover is gone afterwards', !!c.done && s.probes.filter(p => p.at > c.doneAt + 150).every(p => p.top !== COVER));
}
if (cases.includes('exit')) {
  const r = await runCase('exit', row, ['--never-show', '--exit-after-ms', '1200'], ['pause=7000']);
  const { c, s } = r;
  commonChecks('exit', r);
  check('exit: the early exit removed the cover at once', /reason=process-exited/.test(c.fade), c.fade.slice(0, 160));
  check('exit: within half a second of the exit', c.doneAt - s.exited <= 600, `${c.doneAt - s.exited} ms`);
}
if (cases.includes('press')) {
  // ENTER, the 2.8 s game boot, then a press while the cover holds (the stand-in never shows a window).
  const r = await runCase('press', row, ['--never-show'], ['pause=4800', 'ESC', 'pause=3000']);
  const { c, s } = r;
  commonChecks('press', r);
  check('press: a press while black fades the cover out', /reason=press/.test(c.fade) && /launch-cover: press \(key\)/.test(r.text), c.fade.slice(0, 160));
  check('press: the cover is gone well before the no-window timeout', c.doneAt - c.opaqueAt < 8000, `${c.doneAt - c.opaqueAt} ms`);
  void s;
}
console.log(failures ? `\n${failures} FAILED` : '\nall passed');
process.exit(failures ? 1 : 0);
