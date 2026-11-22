// End-of-queue behaviour of the player, driven through the real XMB with short local clips
// (native-xmb/audit/PLAYER-POLISH-2026-11-21.md, item 6). No server and no pad: the XMB opens a local library folder,
// plays it, and the oracles are the XMB's own "media queue:" trace lines plus the grab sidecar (mediaOpen, category, depth).
//
//   bun work/video-end-e2e.ts [--exe C:/dev/out/video-end/re-ps3-native.exe] [--ffmpeg <ffmpeg.exe>] [--out C:/dev/out/video-end-captures]
//       [--cases single,queue,repeat-one,stay,music]
//
// The clips are generated with ffmpeg (--ffmpeg, or ffmpeg on PATH) into <out>/fixtures; when no ffmpeg is found the
// script exits 2 rather than pass vacuously. It takes the XMB window for about 15 s per case.
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';

const arg = (name: string, fallback: string) => { const i = process.argv.indexOf(name); return i >= 0 ? process.argv[i + 1] : fallback; };
const exe = arg('--exe', 'C:/dev/out/video-end/re-ps3-native.exe');
const flash = arg('--dev-flash', 'C:/dev/rpcs3/bin/dev_flash');
const out = arg('--out', 'C:/dev/out/video-end-captures');
const ffmpeg = arg('--ffmpeg', 'ffmpeg');
const cases = arg('--cases', 'single,queue,repeat-one,stay,music').split(',');
let failures = 0;
const check = (label: string, ok: boolean, detail = '') => { console.log(`${ok ? 'PASS' : 'FAIL'}  ${label}${detail ? '  ' + detail : ''}`); if (!ok) failures++; };
if (!existsSync(exe)) { console.error(`missing ${exe}`); process.exit(2); }

const fx = `${out}/fixtures`;
function run(cmd: string[]) { const p = Bun.spawnSync(cmd, { stdout: 'pipe', stderr: 'pipe' }); return p.exitCode === 0; }
function makeFixtures() {
  for (const d of ['one', 'two', 'music']) mkdirSync(`${fx}/${d}`, { recursive: true });
  const video = (name: string, src: string, tone: number) => {
    if (existsSync(name)) return true;
    return run([ffmpeg, '-v', 'error', '-y', '-f', 'lavfi', '-i', `${src}=size=640x360:rate=25:duration=3`, '-f', 'lavfi', '-i', `sine=frequency=${tone}:duration=3`,
      '-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-c:a', 'aac', '-shortest', name]);
  };
  const ok = video(`${fx}/one/clip-a.mp4`, 'smptebars', 440) && video(`${fx}/two/clip-a.mp4`, 'smptebars', 440) && video(`${fx}/two/clip-b.mp4`, 'testsrc2', 660)
    && (existsSync(`${fx}/music/tone.wav`) || run([ffmpeg, '-v', 'error', '-y', '-f', 'lavfi', '-i', 'sine=frequency=330:duration=3', `${fx}/music/tone.wav`]));
  if (!ok) { console.error('could not generate the clips (pass --ffmpeg <path to ffmpeg.exe>)'); process.exit(2); }
}

// RIGHT count from the leftmost category: music is 3 steps in, video is 4 (the same walk the player audit used).
async function runCase(name: string, library: 'video' | 'music', folder: string, prefs: Record<string, unknown>, env: Record<string, string>, tail: string[]) {
  const dir = `${out}/${name}`;
  rmSync(dir, { recursive: true, force: true }); mkdirSync(`${dir}/data`, { recursive: true });
  writeFileSync(`${dir}/data/preferences.json`, JSON.stringify({ active: 'local', version: 2,
    values: { 'display/fullscreen': false, [`profile/local/library/${library}`]: `${fx}/${folder}`, ...prefs } }));
  const g = (n: string) => `grab=${dir}/${n}.png`;
  const col = library === 'video' ? 4 : 3;
  const script = ['1280x720', 'pause=3000', ...Array(6).fill('LEFT'), ...Array(col).fill('RIGHT'), ...Array(library === 'video' ? 4 : 2).fill('DOWN'), 'wait', 'ENTER', 'wait', 'wait', 'wait',
    g('1-library'), 'DOWN', 'wait', 'ENTER', 'pause=1200', g('2-playing'), ...tail, 'quit'];
  const p = Bun.spawn([exe, '--dev-flash', flash, '--data-dir', `${dir}/data`, '--render-clock', '30', '--script', script.join(',')], {
    env: { ...process.env, XMB_MEDIA_TRACE: '1', XMB_SCRIPT_FRAMELESS: '1', XMB_LOG: `${dir}/run.log`, XMB_STEAM_ARTWORK: '0', XMB_STEAM_ACTIVITY: '0', ...env },
    stdout: 'ignore', stderr: 'ignore' });
  const timer = setTimeout(() => p.kill(), 120000);
  await p.exited; clearTimeout(timer);
  const log = existsSync(`${dir}/run.log`) ? readFileSync(`${dir}/run.log`, 'utf8') : '';
  const state = (n: string) => { const f = `${dir}/${n}.png.json`; return existsSync(f) ? JSON.parse(readFileSync(f, 'utf8')) : {}; };
  const count = (re: RegExp) => (log.match(re) || []).length;
  return { log, state, count };
}

makeFixtures();
const END = /media queue: video ended, returning to the XMB/g, STOP = /media queue: stopped at end/g, PLAY = /media queue: play /g;

if (cases.includes('single')) {
  const r = await runCase('single', 'video', 'one', {}, {}, ['pause=5000', 'grab=' + `${out}/single/3-after-end.png`]);
  const s = r.state('2-playing'), e = r.state('3-after-end');
  check('single clip: was playing', s.mediaOpen === true && s.category === 'video', `mediaOpen=${s.mediaOpen}`);
  check('single clip: end returns to the XMB', r.count(END) === 1 && r.count(STOP) === 0, `ended=${r.count(END)} stayed=${r.count(STOP)}`);
  check('single clip: player closed, video column shown', e.mediaOpen === false && e.category === 'video' && e.depth === 1, `mediaOpen=${e.mediaOpen} depth=${e.depth} row=${e.entryTitle}`);
}
if (cases.includes('queue')) {
  const r = await runCase('queue', 'video', 'two', {}, {}, ['pause=2000', 'grab=' + `${out}/queue/3-second-item.png`, 'pause=4500', 'grab=' + `${out}/queue/4-after-end.png`]);
  const mid = r.state('3-second-item'), e = r.state('4-after-end');
  check('queue: both items played in order', r.count(PLAY) === 2 && /play 1 of 2 "clip-a/.test(r.log) && /play 2 of 2 "clip-b/.test(r.log), `plays=${r.count(PLAY)}`);
  check('queue: still open on the second item', mid.mediaOpen === true && mid.entryTitle === 'clip-b.mp4', `mediaOpen=${mid.mediaOpen} row=${mid.entryTitle}`);
  check('queue: exits only after the last item', r.count(END) === 1 && e.mediaOpen === false, `ended=${r.count(END)} mediaOpen=${e.mediaOpen}`);
}
if (cases.includes('repeat-one')) {
  const r = await runCase('repeat-one', 'video', 'one', { 'profile/local/media/repeatMode': 1 }, {}, ['pause=9000', 'grab=' + `${out}/repeat-one/3-after-9s.png`]);
  const e = r.state('3-after-9s');
  check('repeat one: the clip restarted', r.count(PLAY) >= 2, `plays=${r.count(PLAY)}`);
  check('repeat one: never returned to the XMB', r.count(END) === 0 && e.mediaOpen === true, `ended=${r.count(END)} mediaOpen=${e.mediaOpen}`);
}
if (cases.includes('stay')) {
  const r = await runCase('stay', 'video', 'one', {}, { XMB_VIDEO_END_STAY: '1' }, ['pause=5000', 'grab=' + `${out}/stay/3-after-end.png`]);
  const e = r.state('3-after-end');
  check('XMB_VIDEO_END_STAY=1: old behaviour (End of queue, player kept)', r.count(END) === 0 && r.count(STOP) === 1 && e.mediaOpen === true, `ended=${r.count(END)} stopped=${r.count(STOP)} mediaOpen=${e.mediaOpen}`);
}
if (cases.includes('music')) {
  const r = await runCase('music', 'music', 'music', {}, {}, ['pause=5000', 'grab=' + `${out}/music/3-after-end.png`]);
  const s = r.state('2-playing'), e = r.state('3-after-end');
  check('music: was playing', s.mediaOpen === true && s.category === 'music', `mediaOpen=${s.mediaOpen} category=${s.category}`);
  check('music: the page stays after the last track', r.count(END) === 0 && r.count(STOP) === 1 && e.mediaOpen === true, `ended=${r.count(END)} stopped=${r.count(STOP)} mediaOpen=${e.mediaOpen}`);
}
console.log(failures ? `${failures} FAILED` : 'all passed');
process.exit(failures ? 1 : 0);
