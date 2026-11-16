// In-game live overlay end to end, without a real game (audit/INGAME-OVERLAY-DESIGN.md sections 7-8, milestone 1).
// The stand-in is native-xmb/tests/overlay_standin.cpp (a borderless or windowed flip-model D3D11 window of our own, never shipped), launched
// by the XMB itself as an imported app. A scripted PS tap opens the in-game XMB; while it is up a desktop probe captures the screen twice.
// It briefly takes the primary monitor (about 25 s per case). Nothing touches any other application, and no Steam is started.
//   bun work/overlay-live-test.ts [--exe C:/dev/out/overlay/re-ps3-native.exe] [--standin C:/dev/out/overlay-standin/overlay-standin.exe] [--cases live,frozen,windowed]
import { cpSync, existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';

const arg = (name: string, fallback: string) => { const i = process.argv.indexOf(name); return i >= 0 ? process.argv[i + 1] : fallback; };
const exe = arg('--exe', 'C:/dev/out/overlay/re-ps3-native.exe');
const standin = arg('--standin', 'C:/dev/out/overlay-standin/overlay-standin.exe');
const cases = arg('--cases', 'live,frozen,windowed').split(',');
const sizeArg = arg('--size', '');   // e.g. 3840x2160: a frameless XMB window of that size (cost measurement only)
const out = 'C:/dev/out/overlay-live';
const flash = 'C:/dev/rpcs3/bin/dev_flash';
const seed = 'C:/dev/xmb/out/native-reference/v2/game';
const probeScript = new URL('./overlay-desktop-probe.ps1', import.meta.url).pathname.replace(/^\/(\w):/, '$1:');
let failures = 0;
const check = (label: string, ok: boolean, detail = '') => { console.log(`${ok ? 'PASS' : 'FAIL'}  ${label}${detail ? '  ' + detail : ''}`); if (!ok) failures++; };
const win = (s: string) => s.split('/').join(String.fromCharCode(92));
const sleep = (ms: number) => new Promise(r => setTimeout(r, ms));

async function probe(): Promise<{ width: number; height: number; meanLuma: number; regionPixels: number; changed: number }> {
  const p = Bun.spawn(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', win(probeScript)], { stdout: 'pipe', stderr: 'pipe' });
  const text = await new Response(p.stdout).text(); await p.exited;
  return JSON.parse(text.trim().split('\n').pop()!);
}
function killStandin(logPath: string) {
  if (!existsSync(logPath)) return;
  for (const m of readFileSync(logPath, 'utf8').matchAll(/started pid=(\d+)/g)) Bun.spawnSync(['taskkill', '/PID', m[1], '/F'], { stdout: 'ignore', stderr: 'ignore' });
}

async function baseline(): Promise<number> {
  const log = `${out}/baseline.log`; rmSync(log, { force: true }); mkdirSync(out, { recursive: true });
  const p = Bun.spawn([standin, '--borderless', '--seconds', '12', '--log', win(log)], { stdout: 'ignore', stderr: 'ignore' });
  await sleep(2500);
  const r = await probe(); const r2 = await probe();
  killStandin(log); await p.exited;
  return (r.meanLuma + r2.meanLuma) / 2;
}

async function runCase(name: string, standinArgs: string[], extraEnv: Record<string, string>) {
  const dir = `${out}/${name}`;
  rmSync(dir, { recursive: true, force: true });
  mkdirSync(`${dir}/data`, { recursive: true }); mkdirSync(`${dir}/no-steam`, { recursive: true });
  for (const f of ['preferences.json', 'profiles.json', 'settings.ini']) cpSync(`${seed}/${f}`, `${dir}/data/${f}`);
  const standinLog = `${dir}/standin.log`;
  writeFileSync(`${dir}/data/apps.json`, JSON.stringify({ version: 1, apps: [{ id: 'v2-stub', title: 'Gate Stub', kind: 'game', category: 'game',
    executable: win(standin), arguments: [...standinArgs, '--seconds', '60', '--log', win(standinLog)] }] }, null, 2));
  const grab = `${dir}/end.png`;
  const script = ['full', ...Array(10).fill('wait'), 'ENTER', 'wait', 'pause=3000', ...(sizeArg ? [`size=${sizeArg}`, 'pause=1500'] : []),
    'ps', 'pause=14000', 'ESC', 'pause=4000', 'wait', `grab=${grab}`, 'quit'];
  const log = `${dir}/xmb.log`;
  const p = Bun.spawn([exe, '--dev-flash', flash, '--data-dir', `${dir}/data`, '--focus-time', '2000', '--script', script.join(',')],
    { stdout: 'ignore', stderr: 'pipe', env: { ...process.env, XMB_SINGLE_INSTANCE: '0', XMB_STEAM_ROOT: `${dir}/no-steam`, XMB_NO_GAMEBOOT: '1',
      XMB_PS_LOG: '1', XMB_LOG: log, XMB_OVERLAY_COST: '1', ...(sizeArg ? { XMB_SCRIPT_FRAMELESS: '1' } : {}), ...extraEnv } });
  const stderr = new Response(p.stderr).text();
  const timer = setTimeout(() => p.kill(), 120000);
  // Wait for the in-game decision line, give the overlay 2 s of frames, then probe the desktop.
  let measured: Awaited<ReturnType<typeof probe>> | undefined;
  const t0 = Date.now();
  while (Date.now() - t0 < 90000) {
    if (existsSync(log) && /ingame: overlay=/.test(readFileSync(log, 'utf8'))) { await sleep(2500); measured = await probe(); break; }
    await sleep(250);
  }
  await p.exited; clearTimeout(timer);
  const err = await stderr;
  killStandin(standinLog);
  const text = existsSync(log) ? readFileSync(log, 'utf8') : '';
  const sl = existsSync(standinLog) ? readFileSync(standinLog, 'utf8').split('\n').filter(l => l.startsWith('t=')) : [];
  const side = existsSync(`${grab}.json`) ? JSON.parse(readFileSync(`${grab}.json`, 'utf8')) : {};
  const fg = sl.map(l => /foreground=(\d)/.exec(l)?.[1]);
  return { text, err, measured, standinFg: fg, side };
}

const lumaBase = await baseline();
console.log(`baseline stand-in mean luma ${lumaBase}`);

if (cases.includes('live')) {
  const r = await runCase('live', ['--borderless'], {});
  check('live: overlay chosen by default for a borderless title', /ingame: overlay=live reason=borderless title/.test(r.text), (r.text.match(/ingame: overlay=[^\n]*/) || [''])[0]);
  check('live: no blocking capture on the live entry path', /capture=failed[^\n]*not captured \(live overlay\)/.test(r.text) && !/capture=ok/.test(r.text));
  check('live: enter line reports the live overlay as the backdrop', /ingame: enter [^\n]*backdrop=live overlay/.test(r.text));
  const m = r.measured;
  check('live: the game keeps running behind the menu (changed pixels in the corner regions)', !!m && m.changed > m.regionPixels * 0.02, m ? `changed ${m.changed}/${m.regionPixels}` : 'no probe');
  check('live: the screen is dimmed (mean luma well below the bare stand-in)', !!m && m.meanLuma < lumaBase * 0.6, m ? `${m.meanLuma} vs ${lumaBase}` : '');
  check('live: overlay stopped and the owner restored on return', /ingame: DirectComposition live overlay stopped/.test(r.text));
  check('live: focus returned to the game (stand-in reports foreground=1 again after the menu)', /10+1/.test(r.standinFg.join('')), `foreground series ${r.standinFg.join('')}`);
  console.log(((r.text+r.err).match(/ingame: overlay bridge[^\n]*/g) || ['(no bridge cost line)']).join('\n'));
}
if (cases.includes('frozen')) {
  const r = await runCase('frozen', ['--borderless'], { XMB_INGAME_LIVE: '0' });
  check('frozen: XMB_INGAME_LIVE=0 forces the old path and says why', /ingame: overlay=frozen reason=XMB_INGAME_LIVE=0/.test(r.text));
  check('frozen: the frame was captured', /capture=ok/.test(r.text));
  const m = r.measured;
  check('frozen: the game is covered by a still (almost nothing changes behind the menu)', !!m && m.changed < m.regionPixels * 0.01, m ? `changed ${m.changed}/${m.regionPixels}` : 'no probe');
  check('frozen: no overlay was created', !/DirectComposition live overlay started|overlay=live/.test(r.text));
  check('frozen: focus returned to the game', /10+1/.test(r.standinFg.join('')), `foreground series ${r.standinFg.join('')}`);
}
if (cases.includes('windowed')) {
  const r = await runCase('windowed', ['--windowed', '--size', '1280x720'], {});
  check('windowed: a plain window also takes the live overlay', /ingame: overlay=live reason=windowed title/.test(r.text), (r.text.match(/ingame: overlay=[^\n]*/) || [''])[0]);
  check('windowed: focus returned to the game', /10+1/.test(r.standinFg.join('')), `foreground series ${r.standinFg.join('')}`);
}
console.log(failures ? `\n${failures} FAILED` : '\nall passed');
process.exit(failures ? 1 : 0);
