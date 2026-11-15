import { mkdirSync, mkdtempSync, readFileSync, existsSync } from 'node:fs';
import { join } from 'node:path';

// Emulates a Windows display-scale (device pixel ratio) on a 100% PC and grabs one frame, so the high-DPI path can
// be compared with the DPR 1 frame. QT_SCALE_FACTOR multiplies every screen's ratio, so a LOGICAL window of
// WxH renders at (W*dpr)x(H*dpr) physical pixels, exactly what a 4K TV at 300% gives (logical 1280x720).
//
//   bun work/dpi-grab.ts --exe <exe> --dpr 3 --size 1280x720 --out C:/dev/out/dpi/dpr3.png [--tokens wait,RIGHT,...]
//   bun work/dpi-grab.ts --exe <exe> --dpr 3 --full --out ...     (full = fullscreen on the primary screen)
//
// Prints the grab line from the app's log (frame size) and the window state sidecar fields.

const a = process.argv.slice(2);
const opt = (n: string, d = '') => { const i = a.indexOf('--' + n); return i < 0 ? d : (a[i + 1] ?? ''); };
const flag = (n: string) => a.includes('--' + n);
const exe = opt('exe'); const out = opt('out');
if (!exe || !out) { console.error('usage: --exe <exe> --out <png> [--dpr N] [--size WxH | --full] [--tokens a,b] [--env K=V,...]'); process.exit(2); }
const dpr = opt('dpr', '1');
const size = opt('size', '1920x1080');
const tokens = (opt('tokens', 'wait,wait,wait')).split(',').filter(Boolean);
const dir = mkdtempSync(join('C:/dev/out', 'dpi-run-'));
mkdirSync(join(dir, 'data'), { recursive: true }); mkdirSync(join(dir, 'tmp'), { recursive: true });
const log = join(dir, 'x.log');
const script = [flag('full') ? 'full' : `size=${size}`, ...tokens, `grab=${out}`, 'quit'].join(',');
const env: Record<string, string> = {
  ...process.env as Record<string, string>,
  TEMP: join(dir, 'tmp'), TMP: join(dir, 'tmp'), XMB_SINGLE_INSTANCE: '0', XMB_STEAM_ARTWORK: '0', XMB_STEAM_ACTIVITY: '0',
  XMB_POWER_DRY_RUN: '1', XMB_AUDIO_DRY_RUN: '1', XMB_LOG: log, XMB_STEAM_ROOT: join(dir, 'no-steam'),
  QT_ENABLE_HIGHDPI_SCALING: '1',
};
mkdirSync(env.XMB_STEAM_ROOT, { recursive: true });
if (dpr !== '1') env.QT_SCALE_FACTOR = dpr; else delete env.QT_SCALE_FACTOR;
for (const kv of opt('env').split(',').filter(Boolean)) { const [k, v] = kv.split('='); env[k] = v; }
const args = ['--dev-flash', 'C:/dev/rpcs3/bin/dev_flash', '--data-dir', join(dir, 'data'), '--vsh-modules', 'C:/dev/xmb/fw493/vsh_module',
  ...(flag('live') ? [] : ['--render-clock', '30', '--background-clock', '2026-10-25T12:32:00', '--ui-clock', '2026-10-25T12:32:00', '--focus-time', '2000']),
  '--script', script, ...opt('appargs').split(' ').filter(Boolean)];
const proc = Bun.spawn([exe, ...args], { stdout: 'ignore', stderr: 'pipe', env });
const err = new Response(proc.stderr).text();
const t = setTimeout(() => proc.kill(), 120000);
const code = await proc.exited; clearTimeout(t); await err;
const text = existsSync(log) ? readFileSync(log, 'utf8') : '';
for (const l of text.split('\n')) if (/script: grab|kiosk:|dpi:|native frames|native display/i.test(l)) console.log(l.trim());
console.log(`exit ${code}  dir ${dir}`);
