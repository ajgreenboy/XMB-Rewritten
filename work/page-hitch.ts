// First-open hitch measurement for pages opened with Cross (audit/FRAME-PACING.md, "First-open page hitch").
//
//   bun work/page-hitch.ts --exe <exe> [--size 1920x1080] [--slow-ms 8] [--warm-s 40] [--pages a,b] [--json out.json]
//                          [--compare before.json] [--lock] [--keep] [--list]
//
// One live-clock session (XMB_FRAME_TRACE=1, no pinned clock, power/audio dry-run, fresh data dir seeded with one
// Jellyfin registration so the TV column has a sign-in page): after the idle prewarm has finished it walks the
// categories, opens each page once with Cross, scrolls it a little, backs out with Circle, and moves on. Every page is
// bracketed by two `pause=` script markers (the script logs them), so each slow frame of the log belongs to the page
// that was opening. Reported per page: frames over --slow-ms (paint CPU), the worst paint CPU, the longest key handler
// (the Cross press itself, which runs outside paintGL and is labelled "key handler" when over 0.5 ms), and the labels
// of the worst frame (glyph tiles rasterised, first image decode, scene, glass icons...).
//
// --lock holds C:/dev/out/gate.lock (the machine-wide mutex the gate uses) for the run only, so other agents' gates and
// captures cannot corrupt the frame times. Frame times belong to the machine: compare runs from one PC.
import { spawnSync } from 'node:child_process';
import { copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, rmdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

const argv = process.argv.slice(2);
const opt = (k: string, d: string) => { const i = argv.indexOf(k); return i >= 0 ? argv[i + 1] : d; };
const flag = (k: string) => argv.includes(k);
const envExtra = Object.fromEntries(argv.flatMap((a, i) => (a === '--env' ? [argv[i + 1].split('=')] : [])) as [string, string][]);
const exe = opt('--exe', ''), size = opt('--size', '1920x1080'), slow = Number(opt('--slow-ms', '8'));
const warmS = Number(opt('--warm-s', '40')), only = opt('--pages', '').split(',').filter(Boolean);

// ---- navigation model -------------------------------------------------------------------------------------------
// Categories left to right; the XMB starts on Game (index 6). Each category remembers its own row.
const CATS = ['user', 'settings', 'photo', 'music', 'video', 'tv', 'game', 'network', 'psn', 'friends'];
let curCat = 6;
const rows: number[] = CATS.map(() => 0);
const w = (n: number) => Array(n).fill('wait');
function goto(cat: number, row: number): string[] {
  const t: string[] = [];
  while (curCat < cat) { t.push('RIGHT', 'wait'); curCat++; }
  while (curCat > cat) { t.push('LEFT', 'wait'); curCat--; }
  while (rows[cat] < row) { t.push('DOWN'); rows[cat]++; }
  while (rows[cat] > row) { t.push('UP'); rows[cat]--; }
  if (t.length) t.push('wait');
  return t;
}
type Page = { name: string; cat: number; row: number; inner?: string[]; back?: number; before?: string[]; after?: string[] };
// `inner` runs inside the page before it is closed (rows scroll); `before` runs on the parent before the marker.
const S = (n: string, row: number, extra: Partial<Page> = {}): Page => ({ name: `settings/${n}`, cat: 1, row, ...extra });
const PAGES: Page[] = [
  S('system', 5, { inner: ['DOWN', 'wait', 'DOWN', 'wait', 'UP', 'wait'] }),
  S('theme', 6), S('date', 7), S('power', 8), S('device', 9), S('display', 11), S('sound', 12), S('security', 13),
  S('remote', 14), S('network', 15), S('game-folder', 1), S('video', 2), S('music', 3), S('chat', 4),
  { name: 'photo/library', cat: 2, row: 3, back: 8 }, { name: 'photo/playlists', cat: 2, row: 2 }, { name: 'photo/settings', cat: 2, row: 4 },
  { name: 'music/library', cat: 3, row: 2, back: 8 }, { name: 'music/playlists', cat: 3, row: 1 }, { name: 'music/spotify', cat: 3, row: 3 },
  { name: 'video/continue', cat: 4, row: 3 }, { name: 'video/library', cat: 4, row: 4, back: 8 },
  { name: 'tv/jellyfin-setup', cat: 5, row: 0 },
  { name: 'game/steam-hub', cat: 6, row: 10 }, { name: 'game/saved-data-ps3', cat: 6, row: 14 }, 
  { name: 'game/download-hub', cat: 6, row: 17 }, { name: 'game/manage-hub', cat: 6, row: 18 }, { name: 'game/store-message', cat: 6, row: 15 },
  { name: 'network/tailscale', cat: 7, row: 5 }, { name: 'network/homeassistant', cat: 7, row: 7 },
  { name: 'network/weather', cat: 7, row: 8 }, { name: 'network/wifi', cat: 7, row: 10 },
  { name: 'friends/messages', cat: 9, row: 4 }, { name: 'friends/players-message', cat: 9, row: 2 }, { name: 'friends/discord-setup', cat: 9, row: 7 },
  { name: 'users/new-user', cat: 0, row: 5 }, { name: 'users/steam-accounts', cat: 0, row: 7 },
  { name: 'options-panel', cat: 6, row: 10, inner: ['TRIANGLE', 'wait', 'wait', 'wait', 'TRIANGLE'], back: 0 },
];
// Nested surfaces on the System Settings page: System Name opens the on-screen keyboard, System Language a choice list.
const NESTED = ['settings/system/osk', 'settings/system/choice-list'];

const plan: string[] = [];
const marks: { name: string; k: number }[] = [];
const add = (...t: string[]) => plan.push(...t);
function measured(name: string, tokens: string[]) { const k = marks.length; marks.push({ name, k }); add(`pause=${10 + k}`, ...tokens, `pause=${100 + k}`); }
for (const p of PAGES) {
  if (only.length && !only.some((o) => p.name.includes(o))) continue;
  add(...goto(p.cat, p.row));
  measured(p.name, ['ENTER', ...w(4), ...(p.inner ?? ['DOWN', 'wait', 'UP', 'wait']), ...Array(p.back ?? 1).fill('ESC'), ...w(2)]);
  if (p.name === 'settings/system' && (!only.length || only.some((o) => 'settings/system/osk'.includes(o) || 'settings/system'.includes(o)))) {
    // Re-enter System Settings and exercise the two nested surfaces (the page itself is already measured above).
    add('ENTER', ...w(3), 'DOWN', ...w(2));
    measured(NESTED[0], ['ENTER', ...w(6), 'ESC', ...w(3)]);
    add('DOWN', 'DOWN', 'DOWN', 'DOWN', 'DOWN', 'DOWN', 'DOWN', ...w(2));
    measured(NESTED[1], ['ENTER', ...w(5), 'ESC', ...w(4)]);
    add('ESC', ...w(3));
  }
}
if (flag('--list')) { console.log(marks.map((m) => m.name).join('\n')); process.exit(0); }
if (!exe || !existsSync(exe)) { console.error('--exe required'); process.exit(2); }
const tokens = [`size=${size}`, ...w(Math.ceil(warmS / 0.3)), ...plan, ...w(3), 'quit'];

// ---- run -----------------------------------------------------------------------------------------------------------
const lockDir = 'C:/dev/out/gate.lock';
const sleep = (ms: number) => Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, ms);
let locked = false;
if (flag('--lock')) {
  for (;;) { try { mkdirSync(lockDir); locked = true; break; } catch { sleep(5000); } }
}
let logText = '';
const work = mkdtempSync(join('C:/dev/out', 'pagehitch-'));
try {
  mkdirSync(join(work, 'data')); mkdirSync(join(work, 'tmp'));
  copyFileSync(join(import.meta.dir, 'page-hitch-apps.json'), join(work, 'data', 'apps.json'));
  const logPath = join(work, 'log.txt');
  const env = { ...process.env, TEMP: join(work, 'tmp'), TMP: join(work, 'tmp'), XMB_SINGLE_INSTANCE: '0', XMB_STEAM_ARTWORK: '0',
    XMB_STEAM_ACTIVITY: '0', XMB_POWER_DRY_RUN: '1', XMB_AUDIO_DRY_RUN: '1', XMB_SCRIPT_FRAMELESS: '1', XMB_LOG: logPath,
    XMB_FRAME_TRACE: '1', XMB_FRAME_TRACE_MS: String(slow), XMB_SCRIPT_INTERVAL_MS: '300', ...envExtra };
  const run = spawnSync(exe, ['--dev-flash', 'C:/dev/rpcs3/bin/dev_flash', '--data-dir', join(work, 'data'), '--vsh-modules', 'C:/dev/xmb/fw493/vsh_module', '--script', tokens.join(',')], { env, timeout: 1500000, encoding: 'utf8' });
  if (run.error || run.status !== 0) console.error('run did not exit cleanly (', run.error?.message ?? run.status, '); using the log as far as it got');
  logText = readFileSync(logPath, 'utf8');
} finally {
  if (locked) { try { rmdirSync(lockDir); } catch { /* already gone */ } }
  if (!flag('--keep')) { try { rmSync(work, { recursive: true, force: true }); } catch { /* scratch */ } } else console.log('kept', work);
}
if (!logText) process.exit(2);

// ---- parse ---------------------------------------------------------------------------------------------------------
const L = logText.split(/\r?\n/);
const SLOWRE = /frame-trace: SLOW t=\d+ ms cpu=([\d.]+) ms gap=([\d.]+) ms rebuild=\d \[(.*)\]$/;
type Row = { name: string; focus: string; misses: string[]; frames: number; over: number; worstCpu: number; worstLabels: string; keyMs: number; opened: string; first: boolean; glyphTiles: number };
const res: Row[] = [];
let firstMark = -1;
const markLine = (n: number) => L.findIndex((l) => l.trimEnd().endsWith(`script: pause ${n}`));
for (const m of marks) {
  const a = markLine(10 + m.k), b = markLine(100 + m.k);
  if (a < 0 || b < 0) { res.push({ name: m.name, focus: '', misses: [], frames: 0, over: 0, worstCpu: NaN, worstLabels: 'marker missing', keyMs: 0, opened: '', first: false, glyphTiles: 0 }); continue; }
  if (firstMark < 0) firstMark = a;
  let over = 0, worst = 0, worstLabels = '', key = 0, glyph = 0; const opened: string[] = []; const focus: string[] = []; const misses: string[] = [];
  for (let i = a; i < b; i++) {
    const s = SLOWRE.exec(L[i]);
    if (s) {
      const cpu = +s[1], labels = s[3];
      if (/prewarm stage|column prewarm|page prewarm/.test(labels) && !/opened/.test(labels)) { /* idle prewarm slice, not a user frame */ }
      if (cpu >= slow) over++;
      if (cpu > worst) { worst = cpu; worstLabels = labels; }
      const km = /key handler ([\d.]+) ms/.exec(labels); if (km) key = Math.max(key, +km[1]);
      const gm = /(\d+) glyph tiles rasterised/.exec(labels); if (gm) glyph += +gm[1];
    }
    const gm2 = /frame-trace: glyph miss px=([0-9]+) tiles=([0-9]+) text=(.*)$/.exec(L[i]); if (gm2) misses.push(`${gm2[1]}px x${gm2[2]} "${gm2[3]}"`);
    const f = /frame-trace: focus (.*)$/.exec(L[i]); if (f) focus.push(f[1]);
    const o = /frame-trace: surface (.*opened.*)$/.exec(L[i]); if (o) opened.push(o[1]);
  }
  res.push({ name: m.name, misses, focus: focus.join(' > '), frames: 0, over, worstCpu: worst, worstLabels, keyMs: key, opened: opened.join(' + ').slice(0, 70), first: opened.some((o) => /FIRST/.test(o)), glyphTiles: glyph });
}
const warmDone = L.map((l, i) => (/prewarm: page text done/.test(l) ? i : -1)).filter((i) => i >= 0);
const lastWarm = warmDone.length ? Math.max(...warmDone) : -1;
console.log(`size=${size} slow>=${slow} ms; prewarm finished at log line ${lastWarm}, first page marker at ${firstMark}${lastWarm > firstMark ? '  WARNING: prewarm still running during measurement' : ''}`);
if (lastWarm >= 0) console.log(L[lastWarm]);
const atlasPages = L.filter((l) => /text atlas page \d+ created/.test(l)).length;
const styles = L.filter((l) => /frame-trace: text style/.test(l)).map((l) => l.replace(/^.*text style /, ''));
console.log(`atlas page notes in log: ${atlasPages}; distinct text styles drawn: ${styles.length}`);
if (flag('--styles')) console.log(styles.join('\n'));
const before: Record<string, Row> = flag('--compare') ? Object.fromEntries((JSON.parse(readFileSync(opt('--compare', ''), 'utf8')).rows as Row[]).map((r) => [r.name, r])) : {};
const pad = (s: string, n: number) => (s + ' '.repeat(n)).slice(0, n);
const sorted = [...res].sort((x, y) => (y.worstCpu || 0) - (x.worstCpu || 0));
console.log(pad('page', 28) + pad('over', 5) + pad('worst ms', 9) + pad('key ms', 8) + pad('tiles', 6) + (flag('--compare') ? pad('before', 14) : '') + 'worst-frame labels / opened');
for (const r of sorted) {
  const b = before[r.name];
  console.log(pad(r.name, 28) + pad(String(r.over), 5) + pad(Number.isNaN(r.worstCpu) ? '-' : r.worstCpu.toFixed(1), 9) + pad(r.keyMs ? r.keyMs.toFixed(1) : '', 8) + pad(String(r.glyphTiles), 6)
    + (flag('--compare') ? pad(b ? `${b.over}/${b.worstCpu.toFixed(1)}` : '', 14) : '') + `${r.worstLabels.slice(0, 90)} || ${r.opened} || ${r.focus.slice(-60)}`);
}
if (flag('--misses')) for (const r of res) if (r.misses.length) console.log(`MISS ${r.name}: ${r.misses.join(' ; ')}`);
if (opt('--json', '')) writeFileSync(opt('--json', ''), JSON.stringify({ exe, size, slow, rows: res }, null, 1));
