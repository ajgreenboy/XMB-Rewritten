// Installing panel end to end (native-xmb/packaging/README.md, "Pending Installers"), against the SYNTHETIC installer stand-in
// native-xmb/tests/installer_standin.cpp only. No real repack or game installer is downloaded or run, and Steam is never started.
//
// For each case the real XMB is driven by its own --script tokens: Game > Pending Installers > the entry > "Install (show
// installer)" > Yes. The stand-in then runs as a visible tracked window, the XMB shows the Installing panel, and the panel is
// captured (grab=) in each state. Oracles: the grab's .json (row ids and texts, i.e. exactly what the panel shows), the XMB log
// ("installer:" lines), the stand-in's /LOG file, the registry and the destination folder. A desktop screenshot is taken while
// the installer window is up to prove it is a real, visible window.
//
//   bun work/installer-panel-e2e.ts [--exe C:/dev/out/installer-panel/re-ps3-native.exe]
//       [--standin C:/dev/out/installer-panel-tests/xmb-installer-standin.exe] [--cases success,polite,stubborn,fail,nofiles]
//       [--out C:/dev/out/installer-panel-e2e]
import { copyFileSync, existsSync, mkdirSync, readFileSync, readdirSync, rmSync, writeFileSync } from 'node:fs';
import { createHash } from 'node:crypto';

const arg = (name: string, fallback: string) => { const i = process.argv.indexOf(name); return i >= 0 ? process.argv[i + 1] : fallback; };
const exe = arg('--exe', 'C:/dev/out/installer-panel/re-ps3-native.exe');
const standin = arg('--standin', 'C:/dev/out/installer-panel-tests/xmb-installer-standin.exe');
const flash = arg('--dev-flash', 'C:/dev/rpcs3/bin/dev_flash');
const out = arg('--out', 'C:/dev/out/installer-panel-e2e');
const cases = arg('--cases', 'success,polite,stubborn,fail,nofiles').split(',');
const TITLE = 'Synthetic E2E Game';
let failures = 0;
const check = (label: string, ok: boolean, detail = '') => { console.log(`${ok ? 'PASS' : 'FAIL'}  ${label}${detail ? '  ' + detail : ''}`); if (!ok) failures++; };
const win = (s: string) => s.split('/').join('\\');
const sleep = (ms: number) => new Promise(r => setTimeout(r, ms));

for (const f of [exe, standin, flash]) if (!existsSync(f)) { console.error(`missing ${f}`); process.exit(2); }

type Row = { id: string; title: string; detail: string };
const rowsOf = (png: string): Row[] => existsSync(png + '.json') ? (JSON.parse(readFileSync(png + '.json', 'utf8')).entryRows || []) : [];
const rowValue = (rows: Row[], id: string) => { const r = rows.find(x => x.id === id); return !r ? '' : r.title.includes(': ') ? r.title.slice(r.title.indexOf(': ') + 2) : r.detail; };
const rowDetail = (rows: Row[], id: string) => rows.find(r => r.id === id)?.detail ?? '';
const hasRow = (rows: Row[], id: string) => rows.some(r => r.id === id);
const stateOf = (png: string) => existsSync(png + '.json') ? JSON.parse(readFileSync(png + '.json', 'utf8')) : {};

function seed(dir: string) {
  rmSync(dir, { recursive: true, force: true });
  const library = `${dir}/drive`, games = `${library}/XMB Games`, pending = `${games}/Pending Installers/e2e-synthetic`, archive = `${games}/Download Archives/e2e-synthetic`;
  for (const d of [pending, archive, `${dir}/data`, `${dir}/no-steam`]) mkdirSync(d, { recursive: true });
  const setup = `${pending}/setup.exe`;
  copyFileSync(standin, setup);
  const sha256 = createHash('sha256').update(readFileSync(setup)).digest('hex');
  writeFileSync(`${dir}/data/pending-installers.json`, JSON.stringify({ version: 1, entries: [{
    id: 'e2e-synthetic', title: TITLE, installer: win(setup), archive: win(`${archive}/archive.zip`), destination: '', libraryRoot: win(games), sha256, status: 'pending', error: '' }] }));
  return { library, games, setup, destination: `${games}/${TITLE}`, data: `${dir}/data`, sha256 };
}

async function desktopShot(path: string) {
  const ps = `Add-Type -AssemblyName System.Windows.Forms,System.Drawing;$b=[Windows.Forms.SystemInformation]::VirtualScreen;` +
    `$bmp=New-Object Drawing.Bitmap $b.Width,$b.Height;$g=[Drawing.Graphics]::FromImage($bmp);$g.CopyFromScreen($b.Left,$b.Top,0,0,$bmp.Size);` +
    `$bmp.Save('${win(path)}',[Drawing.Imaging.ImageFormat]::Png);$g.Dispose();$bmp.Dispose()`;
  Bun.spawnSync(['powershell', '-NoProfile', '-Command', ps], { stdout: 'ignore', stderr: 'ignore' });
}

// Where the Pending Installers row sits in the Game column (the run starts there).
async function discoverRow(): Promise<number> {
  const dir = `${out}/discovery`;
  const s = seed(dir);
  const grab = `${dir}/game-column.png`;
  await run(dir, s, ['full', 'pause=4000', `grab=${grab}`, 'quit'], {}, 60000);
  const rows = rowsOf(grab);
  const index = rows.findIndex(r => r.id === 'game:pending-installers');
  if (index < 0) { console.error(`game:pending-installers not in the Game column: ${rows.map(r => r.id).join(', ')}`); process.exit(2); }
  return index;
}

async function run(dir: string, s: ReturnType<typeof seed>, tokens: string[], env: Record<string, string>, timeoutMs: number, during?: () => Promise<void>) {
  const log = `${dir}/xmb.log`;
  const p = Bun.spawn([exe, '--dev-flash', flash, '--data-dir', s.data, '--focus-time', '2000', '--script', tokens.join(',')],
    { stdout: 'ignore', stderr: 'pipe', env: { ...process.env, XMB_SINGLE_INSTANCE: '0', XMB_DIALOG_LOG: '1', XMB_STEAM_ROOT: `${dir}/no-steam`, XMB_STEAM_ARTWORK: '0',
      XMB_GAME_LIBRARY_ROOT: win(s.library), XMB_LOG: log, ...env } });
  const stderr = new Response(p.stderr).text();
  const timer = setTimeout(() => p.kill(), timeoutMs);
  if (during) during().catch(e => console.error('during:', e));
  await p.exited; clearTimeout(timer);
  return (existsSync(log) ? readFileSync(log, 'utf8') : '') + '\n' + await stderr;
}

const registry = (s: ReturnType<typeof seed>) => (JSON.parse(readFileSync(`${s.data}/pending-installers.json`, 'utf8')).entries as any[]).find(e => e.id === 'e2e-synthetic');
const installerLog = (s: ReturnType<typeof seed>) => { const d = `${s.data}/installer-logs`; return existsSync(d) ? readdirSync(d).map(f => readFileSync(`${d}/${f}`, 'utf8')).join('\n') : ''; };
const killStandins = () => { Bun.spawnSync(['taskkill', '/IM', 'setup.exe', '/F', '/T'], { stdout: 'ignore', stderr: 'ignore' }); };

const pendingRow = await discoverRow();
console.log(`Pending Installers row index in the Game column: ${pendingRow}`);

// Game column -> Pending Installers -> the entry -> detail; the first install row (show installer) is the 4th row.
const nav = (dir: string) => ['full', 'pause=4000', ...Array(pendingRow).fill('DOWN'), 'pause=300', 'ENTER', 'pause=1500', `grab=${dir}/01-list.png`, 'ENTER', 'pause=1500',
  `grab=${dir}/02-detail.png`, 'DOWN', 'DOWN', 'DOWN', 'pause=400', `grab=${dir}/03-install-row.png`, 'ENTER', 'pause=800', `grab=${dir}/04-confirm.png`, 'RIGHT', 'pause=300', 'ENTER', 'pause=2500'];

async function startedCase(name: string, mode: string, seconds: number, tail: (dir: string) => string[], timeoutMs: number, shotAfterMs = 0) {
  const dir = `${out}/${name}`;
  const s = seed(dir);
  const tokens = [...nav(dir), ...tail(dir), 'quit'];
  const text = await run(dir, s, tokens, { XMB_STANDIN_MODE: mode, XMB_STANDIN_SECONDS: String(seconds) }, timeoutMs, async () => {
    if (!shotAfterMs) return;
    for (let i = 0; i < 400 && !installerLog(s); i++) await sleep(100);
    await sleep(shotAfterMs);
    await desktopShot(`${dir}/desktop-installer-window.png`);
  });
  killStandins();
  writeFileSync(`${dir}/installer-lines.log`, (text.match(/[^\n]*installer:[^\n]*/g) || []).join('\n') + '\n');
  return { dir, s, text };
}

if (cases.includes('success')) {
  const { dir, s, text } = await startedCase('success', 'success', 10, d => ['grab=' + d + '/05-running-a.png', 'pause=3000', 'grab=' + d + '/06-running-b.png', 'pause=7000', 'grab=' + d + '/07a-success-result-page.png', 'ENTER', 'pause=600', 'grab=' + d + '/07-success.png'], 150000, 1500);
  const confirm = stateOf(`${dir}/04-confirm.png`);
  check('success: a confirm box was raised before anything was launched', text.indexOf('dialog: box show') >= 0 && text.indexOf('dialog: box show') < text.indexOf('installer: started'));
  check('success: the installer started tracked and visible', /installer: started .*\(visible, tracked\)/.test(text), (text.match(/installer: started[^\n]*/) || [''])[0].slice(0, 140));
  check('success: the command line has no /VERYSILENT and passes /DIR /LANG /NORESTART /LOG', !/VERYSILENT/i.test(text) && /destination=/.test(text));
  const a = rowsOf(`${dir}/05-running-a.png`), b = rowsOf(`${dir}/06-running-b.png`), done = rowsOf(`${dir}/07-success.png`);
  check('success: the panel is up with a Close row first', a[0]?.id === 'installer:close', a.map(r => r.id).join(','));
  check('success: the panel shows elapsed, a step from the log, destination and size written', hasRow(a, 'installer:line:Elapsed') && hasRow(a, 'installer:status') && hasRow(a, 'installer:destination'),
    a.map(r => `${r.title}=${r.detail}`).join(' | ').slice(0, 300));
  check('success: the written size grows between two captures', hasRow(b, 'installer:line:Written to destination') &&
    rowValue(a, 'installer:line:Written to destination') !== rowValue(b, 'installer:line:Written to destination'), `${rowValue(a, 'installer:line:Written to destination')} -> ${rowValue(b, 'installer:line:Written to destination')}`);
  check('success: progress appears only because the log carried it', /%/.test(rowValue(b, 'installer:line:Progress')), rowValue(b, 'installer:line:Progress'));
  check('success: after exit 0 with files the panel offers Register Installed Game', hasRow(done, 'installer:register') && /finished/i.test(rowValue(done, 'installer:result')), done.map(r => r.id).join(','));
  const reg = registry(s);
  check('success: the registry says exited-needs-verification and nothing was registered by itself', reg.status === 'helper-exited-needs-verification' && !existsSync(`${s.data}/apps.json`), `${reg.status}`);
  check('success: the destination holds the files the installer wrote', existsSync(`${s.destination}/SyntheticGame.exe`));
  check('success: the installer log is the stand-in\'s own', /Installation process succeeded/.test(installerLog(s)));
}
if (cases.includes('polite')) {
  const { dir, s, text } = await startedCase('polite', 'success', 40, d => ['grab=' + d + '/05-running.png', 'ENTER', 'pause=300', 'grab=' + d + '/06-close-requested.png', 'pause=2500', 'grab=' + d + '/07a-closed-result-page.png', 'ENTER', 'pause=600', 'grab=' + d + '/07-closed.png'], 150000);
  const req = rowsOf(`${dir}/06-close-requested.png`), closed = rowsOf(`${dir}/07-closed.png`);
  check('polite: the close was requested politely (WM_CLOSE to a window)', /installer: polite close processes=\d+ windows=[1-9]/.test(text), (text.match(/installer: polite close[^\n]*/) || [''])[0]);
  check('polite: nothing was force closed', !/force close confirmed/.test(text));
  check('polite: the panel reports the close request or the closed result', /Close requested|has exited/.test(rowValue(req, 'installer:status')) || hasRow(req, 'installer:note') || hasRow(closed, 'installer:result'), rowValue(req, 'installer:status'));
  check('polite: the result says closed and a half-finished install may remain', /closed/i.test(rowValue(closed, 'installer:result')) && /half-finished/.test(rowDetail(closed, 'installer:result')), rowValue(closed, 'installer:result'));
  check('polite: no Register row after a cancel', !hasRow(closed, 'installer:register'));
  const reg = registry(s);
  check('polite: the Pending Installers row is back to pending', reg.status === 'pending' && reg.destination === '', `${reg.status} dest="${reg.destination}"`);
  check('polite: the destination was left in place, not deleted', existsSync(`${s.destination}/data`));
}
if (cases.includes('stubborn')) {
  // Stubborn: ignores WM_CLOSE. Close -> 5 s later the question -> No (declined, still running) -> Close again -> question -> Yes.
  const { dir, s, text } = await startedCase('stubborn', 'stubborn', 90,
    d => ['grab=' + d + '/05-running.png', 'ENTER', 'pause=1500', 'grab=' + d + '/06-close-requested.png', 'pause=5200', 'grab=' + d + '/07-force-question.png',
      'ENTER', 'pause=1500', 'grab=' + d + '/08-declined.png', 'ENTER', 'pause=5800', 'grab=' + d + '/09-force-question-2.png', 'RIGHT', 'pause=300', 'ENTER', 'pause=2500', 'grab=' + d + '/10a-force-closed-result-page.png', 'ENTER', 'pause=600', 'grab=' + d + '/10-force-closed.png'], 180000);
  const declined = rowsOf(`${dir}/08-declined.png`), closed = rowsOf(`${dir}/10-force-closed.png`);
  check('stubborn: three boxes were raised (install, force-close, force-close again) and one answer was No', (text.match(/dialog: box show/g) || []).length === 3 && (text.match(/dialog: box enter choice= no commit= 0/g) || []).length === 1, String((text.match(/dialog: box show/g) || []).length));
  check('stubborn: the first question answered No leaves it running and says so', /still running/i.test(rowValue(declined, 'installer:note')) || /declined/i.test(rowValue(declined, 'installer:note')), rowValue(declined, 'installer:note'));
  check('stubborn: only the confirmed question terminated anything (exactly once)', (text.match(/force close confirmed/g) || []).length === 1, (text.match(/installer: force close confirmed[^\n]*/) || [''])[0]);
  check('stubborn: the panel shows closed after the force close and no Register row', /closed/i.test(rowValue(closed, 'installer:result')) && !hasRow(closed, 'installer:register'), rowValue(closed, 'installer:result'));
  check('stubborn: the result warns about the half-finished install and says nothing was deleted', /half-finished/.test(closed.find(r => r.id === 'installer:result')?.detail ?? ''), rowDetail(closed, 'installer:result').slice(0, 120));
  const reg = registry(s);
  check('stubborn: back to pending, destination untouched', reg.status === 'pending' && existsSync(`${s.destination}/data`), reg.status);
  const left = Bun.spawnSync(['tasklist', '/FI', 'IMAGENAME eq setup.exe'], { stdout: 'pipe' }).stdout.toString();
  check('stubborn: no stand-in process (or its helper) is left running', !/setup\.exe/i.test(left), left.trim().split('\n').slice(-2).join(' '));
}
if (cases.includes('fail')) {
  const { dir, s, text } = await startedCase('fail', 'fail', 6, d => ['grab=' + d + '/05-running.png', 'pause=6500', 'grab=' + d + '/06a-failure-result-page.png', 'ENTER', 'pause=600', 'grab=' + d + '/06-failure.png'], 120000);
  const done = rowsOf(`${dir}/06-failure.png`);
  check('fail: a non-zero exit is reported with its code', /exit code 2/.test(rowValue(done, 'installer:result')), rowValue(done, 'installer:result'));
  check('fail: no Register row, no success claim', !hasRow(done, 'installer:register') && !/finished/i.test(rowValue(done, 'installer:result')));
  check('fail: the log path is shown', /installer-logs/.test(rowValue(done, 'installer:log')), rowValue(done, 'installer:log'));
  const reg = registry(s);
  check('fail: the registry records nonzero with the log path', reg.status === 'nonzero' && /installer-logs/.test(reg.error), `${reg.status}`);
  check('fail: the installer\'s own message is in its log', /Synthetic failure/.test(installerLog(s)));
}
if (cases.includes('nofiles')) {
  const { dir, s } = await startedCase('nofiles', 'nofiles', 3, d => ['pause=5000', 'grab=' + d + '/05a-nofiles-result-page.png', 'ENTER', 'pause=600', 'grab=' + d + '/05-nofiles.png'], 120000);
  const done = rowsOf(`${dir}/05-nofiles.png`);
  check('nofiles: exit 0 with an empty destination is not claimed as installed', /not verified/i.test(rowValue(done, 'installer:result')) && !/^Installer finished$/.test(rowValue(done, 'installer:result')), rowValue(done, 'installer:result'));
  const reg = registry(s);
  check('nofiles: never registered by itself', !existsSync(`${s.data}/apps.json`), reg.status);
}
console.log(failures ? `\n${failures} FAILED` : '\nall passed');
process.exit(failures ? 1 : 0);
