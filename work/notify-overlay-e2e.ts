// In-game notification overlay end to end (audit/INFO-BAR.md "Toast"): the XMB launches a stand-in title of our own (the XMB's
// own exe in --zf-text-sink mode, a small window that does nothing), a scripted NOTIFY= raises a notification while the title runs,
// and the NotifyOverlay window must appear over it with the XMB toast's geometry. The same notification is also raised inside
// the XMB and grabbed, and the two toasts are composed side by side. It takes the primary monitor for about 40 s. No real game,
// no Steam, nothing else is started.
//
//   bun work/notify-overlay-e2e.ts [--exe C:/dev/out/notify-overlay/re-ps3-native.exe] [--out C:/dev/out/notify-overlay/e2e]
import { cpSync, existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';

const arg = (name: string, fallback: string) => { const i = process.argv.indexOf(name); return i >= 0 ? process.argv[i + 1] : fallback; };
const exe = arg('--exe', 'C:/dev/out/notify-overlay/re-ps3-native.exe');
const out = arg('--out', 'C:/dev/out/notify-overlay/e2e');
const flash = arg('--dev-flash', 'C:/dev/rpcs3/bin/dev_flash');
const seed = arg('--seed', 'C:/dev/xmb/out/native-reference/v2/game');   // read-only data seed (the gate's game state)
const tools = `${import.meta.dir}/notify-overlay-tools.ps1`.split('/').join('\\');
const win = (s: string) => s.split('/').join('\\');
const sleep = (ms: number) => new Promise(r => setTimeout(r, ms));
let failures = 0;
const check = (label: string, ok: boolean, detail = '') => { console.log(`${ok ? 'PASS' : 'FAIL'}  ${label}${detail ? '  ' + detail : ''}`); if (!ok) failures++; };
const NOTE = 'Signed out.|Spotify';

function ps(args: string[]) {
  const r = Bun.spawnSync(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', tools, ...args], { stdout: 'pipe', stderr: 'pipe' });
  if (r.exitCode !== 0) throw new Error(`tools ${args.join(' ')}: ${r.stderr.toString()}`);
}
function seedDir(name: string, withGame: boolean) {
  const dir = `${out}/${name}`;
  rmSync(dir, { recursive: true, force: true });
  mkdirSync(`${dir}/data`, { recursive: true }); mkdirSync(`${dir}/no-steam`, { recursive: true });
  for (const f of ['preferences.json', 'profiles.json', 'settings.ini']) cpSync(`${seed}/${f}`, `${dir}/data/${f}`);
  if (withGame) writeFileSync(`${dir}/data/apps.json`, JSON.stringify({ version: 1, apps: [{ id: 'v2-stub', title: 'Gate Stub', kind: 'game', category: 'game',
    executable: win(exe), arguments: ['--zf-text-sink', win(`${dir}/sink.log`), '40', '--data-dir', win(`${dir}/sink-data`)] }] }, null, 2));
  return dir;
}
function spawnXmb(dir: string, script: string[]) {
  const p = Bun.spawn([exe, '--dev-flash', flash, '--data-dir', `${dir}/data`, '--focus-time', '2000', '--script', script.join(',')],
    { stdout: 'ignore', stderr: 'pipe', env: { ...process.env, XMB_SINGLE_INSTANCE: '0', XMB_STEAM_ROOT: `${dir}/no-steam`, XMB_NO_GAMEBOOT: '1', XMB_NOTIFY_LOG: '1', XMB_LOG: `${dir}/xmb.log` } });
  const stderr = new Response(p.stderr).text();
  const timer = setTimeout(() => p.kill(), 150000);
  return { p, done: async () => { await p.exited; clearTimeout(timer); await stderr; return existsSync(`${dir}/xmb.log`) ? readFileSync(`${dir}/xmb.log`, 'utf8') : ''; } };
}
const logText = (dir: string) => existsSync(`${dir}/xmb.log`) ? readFileSync(`${dir}/xmb.log`, 'utf8') : '';
async function waitFor(dir: string, re: RegExp, ms: number) {
  for (let t = 0; t < ms; t += 100) { if (re.test(logText(dir))) return true; await sleep(100); }
  return false;
}

if (!existsSync(exe)) { console.error(`missing ${exe}`); process.exit(2); }
mkdirSync(out, { recursive: true });

// 1. The XMB's own toast, grabbed from its framebuffer.
{
  const dir = seedDir('xmb', false);
  const grab = `${dir}/xmb-toast.png`;
  const run = spawnXmb(dir, ['full', ...Array(10).fill('wait'), `NOTIFY=${NOTE}`, 'wait', 'wait', 'wait', `grab=${grab}`, 'wait', 'quit']);
  await run.done();
  check('xmb: the toast frame was grabbed', existsSync(grab));
}

// 2. The in-game overlay over a stand-in title.
{
  const dir = seedDir('game', true);
  const run = spawnXmb(dir, ['full', ...Array(10).fill('wait'), 'ENTER', 'wait', 'pause=3000', `NOTIFY=${NOTE}`, 'pause=12000', 'quit']);
  const raised = await waitFor(dir, /script: notify/, 60000);
  check('game: the notification was raised while the title ran', raised);
  await sleep(1000);   // past the 200 ms fade-in, inside the 5.1 s life
  ps(['-Mode', 'shot', '-Out', win(`${dir}/overlay.png`)]);
  const shot = JSON.parse(readFileSync(`${dir}/overlay.png.json`, 'utf8').replace(/^\uFEFF/, ''));
  const windows = ([] as string[]).concat(shot.topmost || []).map(l => { const [h, cls, rect, ex, title] = l.split('|'); const [x, y, w, hh] = rect.split(',').map(Number); return { h: Number(h), cls, x, y, w, hh, ex: Number(ex), title }; });
  const overlay = windows.find(w => w.cls.startsWith('Qt') && (w.ex & 0x20) && (w.ex & 0x08000000) && w.hh >= 100 && w.hh <= 110 && w.w <= 700);
  check('game: an overlay window exists (topmost, click-through, no-activate, toast-sized)', !!overlay, windows.map(w => `${w.cls}:${w.w}x${w.hh}@${w.x},${w.y}`).join(' '));
  if (overlay) {
    const s = shot.width / 1920;
    check('game: its height is the 107 px toast box (scaled)', Math.abs(overlay.hh - 107 * s) <= 2, `${overlay.hh} px`);
    check('game: its right edge is the toast right edge (1818 design px)', Math.abs(overlay.x + overlay.w - 1818 * s) <= 2, `${overlay.x + overlay.w} px`);
    check('game: its top is the toast top (82 design px)', Math.abs(overlay.y - 82 * s) <= 2, `${overlay.y} px`);
    check('game: it is not the foreground window (no focus stolen)', overlay.h !== shot.foreground);
    check('game: it is a layered window carrying WS_EX_LAYERED', (overlay.ex & 0x80000) !== 0);
  }
  await sleep(6500);   // 2100 + 3000 ms life has passed
  ps(['-Mode', 'shot', '-Out', win(`${dir}/after.png`)]);
  const after = JSON.parse(readFileSync(`${dir}/after.png.json`, 'utf8').replace(/^\uFEFF/, ''));
  const still = ([] as string[]).concat(after.topmost || []).some(l => { const [, cls, rect, ex] = l.split('|'); const hh = Number(rect.split(',')[3]); return cls.startsWith('Qt') && (Number(ex) & 0x20) && (Number(ex) & 0x08000000) && hh >= 100 && hh <= 110; });
  check('game: the overlay is gone after the notification expired (auto-hide)', !still);
  await run.done();
  const sink = existsSync(`${dir}/sink.log`) ? readFileSync(`${dir}/sink.log`, 'utf8') : '';
  const ready = /"e":"ready","pid":(\d+)/.exec(sink);
  if (ready) Bun.spawnSync(['taskkill', '/PID', ready[1], '/F'], { stdout: 'ignore', stderr: 'ignore' });   // the stand-in this run started, by pid
}

// 3. Side by side.
ps(['-Mode', 'compose', '-Xmb', win(`${out}/xmb/xmb-toast.png`), '-Game', win(`${out}/game/overlay.png`), '-Out', win(`${out}/side-by-side.png`)]);
check('side-by-side written', existsSync(`${out}/side-by-side.png`), `${out}/side-by-side.png`);
console.log(failures ? `\n${failures} FAILED` : '\nall passed');
process.exit(failures ? 1 : 0);
