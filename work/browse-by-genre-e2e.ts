// Browse by Genre native interaction gate (audit/BROWSE-BY-GENRE-E2E.md).
//
// Runs the real re-ps3-native.exe against a deterministic loopback Hydra catalogue and drives the
// ordinary scripted-input route:
//
//   Game -> Download Games -> Browse by Genre -> genre list -> "Popular Games: <genre>" -> game
//   details -> source/edition options -> Back (three levels)
//
// No live feed is ever contacted (XMB_HYDRA_TEST_FIXTURE pins the hot and games endpoints to the
// loopback below) and no download/installer row is activated. The assertions read the capture
// sidecars the app writes next to each grab, so they are structural (page title, depth, row ids,
// titles, counts) rather than pixel comparisons.
//
//   bun work/browse-by-genre-e2e.ts [--exe <re-ps3-native.exe>] [--flash <dev_flash dir>] [--out <scratch dir>]
//
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { join, resolve } from 'node:path';

const arg = (name: string, fallback: string) => { const i = process.argv.indexOf(name); return i >= 0 ? process.argv[i + 1] : fallback; };
const repo = resolve(import.meta.dir, '..');
const exe = arg('--exe', join(repo, 'out', 'bbg-tests', 're-ps3-native.exe'));
const flash = arg('--flash', 'C:/dev/rpcs3/bin/dev_flash');
const out = arg('--out', join(repo, 'out', 'bbg-e2e'));

// A deterministic four-title hot catalogue. Havoc Runner carries two download sources and a Deluxe
// edition in its /games response, so one game demonstrates complete source and edition options.
const hot = [
  { title: 'Havoc Runner', libraryImageUrl: '', genres: ['Action', 'Adventure'] },
  { title: 'Quiet Grove', libraryImageUrl: '', genres: ['RPG'] },
  { title: 'Iron Bastion', libraryImageUrl: '', genres: ['Action'] },
  { title: 'Starlight Drift', libraryImageUrl: '', genres: ['Adventure'] },
];

let requests: string[] = [];
const server = Bun.serve({
  hostname: '127.0.0.1',
  port: 0,
  fetch(req) {
    const url = new URL(req.url);
    requests.push(`${req.method} ${url.pathname}${url.search}`);
    const json = (value: unknown) => new Response(JSON.stringify(value), { headers: { 'content-type': 'application/json' } });
    if (url.pathname === '/hot') return json(hot);
    if (url.pathname === '/games') {
      const q = (url.searchParams.get('q') ?? '').toLowerCase();
      const game = (source: string, fileSize: string, originalTitle: string) => ({
        name: 'Havoc Runner', title: originalTitle, originalTitle, source, fileSize, sourceStatus: 'Safe For Use',
        sourceUrl: `http://127.0.0.1:${server.port}/${source.toLowerCase().replace(/[^a-z]/g, '')}.json`,
      });
      if (q.includes('havoc')) {
        return json({ games: [
          game('Source Alpha', '2 GB', 'Havoc Runner'),
          game('Source Beta', '3 GB', 'Havoc Runner'),
          game('Source Beta', '4 GB', 'Havoc Runner — Deluxe Edition'),
        ] });
      }
      if (q.includes('quiet')) return json({ games: [game('Source Gamma', '1 GB', 'Quiet Grove')] });
      if (q.includes('iron')) return json({ games: [game('Source Delta', '4 GB', 'Iron Bastion')] });
      if (q.includes('starlight')) return json({ games: [game('Source Epsilon', '1 GB', 'Starlight Drift')] });
      return json({ games: [] });
    }
    return new Response('not found', { status: 404 });
  },
});

rmSync(out, { recursive: true, force: true });
mkdirSync(join(out, 'data'), { recursive: true });
mkdirSync(join(out, 'no-steam'), { recursive: true });

const nav = (n: number) => Array(n).fill('DOWN');
const script = [
  'pause=2000',
  ...nav(7), 'ENTER',                 // Game -> Download Games (row 7)
  'pause=800',
  ...nav(2), 'ENTER',                 // hub -> Browse by Genre (row 2)
  'pause=1500', `grab=${out}/01-genres.png`,
  'ENTER',                            // pick "Action" (row 0)
  'pause=1500', `grab=${out}/02-results.png`,
  'ENTER',                            // pick "Havoc Runner" (row 0)
  'pause=800', `grab=${out}/03-details.png`,
  'ENTER',                            // Download -> source/edition options
  'pause=800', `grab=${out}/04-sources.png`,
  'ESC', 'pause=800', `grab=${out}/05-back-details.png`,
  'ESC', 'pause=800', `grab=${out}/06-back-results.png`,
  'ESC', 'pause=800', `grab=${out}/07-back-genres.png`,
  'quit',
].join(',');

const env = {
  ...process.env,
  TMP: out, TEMP: out,
  XMB_SINGLE_INSTANCE: '0',
  XMB_STEAM_ROOT: `${out}/no-steam`,
  XMB_FIRST_RUN: '0',
  XMB_HYDRA_TEST_FIXTURE: '1',
  XMB_HYDRA_HOT_URL: `http://127.0.0.1:${server.port}/hot`,
  XMB_HYDRA_GAMES_URL: `http://127.0.0.1:${server.port}/games`,
  XMB_LOG: `${out}/xmb.log`,
} as Record<string, string>;

const proc = Bun.spawn([exe, '--dev-flash', flash, '--data-dir', `${out}/data`, '--no-intro', '--script', script],
  { stdout: 'ignore', stderr: 'ignore', env });
const timer = setTimeout(() => proc.kill(), 120000);
const code = await proc.exited;
clearTimeout(timer);
server.stop(true);
writeFileSync(`${out}/requests.log`, requests.join('\n') + '\n');

const side = (name: string) => { const p = `${out}/${name}.png.json`; return existsSync(p) ? JSON.parse(readFileSync(p, 'utf8')) : null; };

let failures = 0;
const check = (label: string, ok: boolean, detail = '') => { console.log(`${ok ? 'PASS' : 'FAIL'}  ${label}${detail ? '  ' + detail : ''}`); if (!ok) failures++; };
const rows = (s: any) => (s?.entryRows ?? []) as any[];

check('native exited cleanly', code === 0, `exit ${code}`);

const g1 = side('01-genres');
check('genre list: page title', g1?.pageTitle === 'Browse by Genre', g1?.pageTitle);
check('genre list: depth 2', g1?.depth === 2, `depth=${g1?.depth}`);
check('genre list: three genres', g1?.entryCount === 3, `count=${g1?.entryCount}`);
check('genre list: Action first with 2 games', rows(g1)[0]?.id === 'game:genre:Action' && rows(g1)[0]?.title === 'Action' && /2/.test(rows(g1)[0]?.detail ?? ''), `${rows(g1)[0]?.id} / ${rows(g1)[0]?.detail}`);
check('genre list: Adventure second', rows(g1)[1]?.id === 'game:genre:Adventure' && rows(g1)[1]?.title === 'Adventure', `${rows(g1)[1]?.id}`);
check('genre list: RPG third with 1 game', rows(g1)[2]?.id === 'game:genre:RPG' && rows(g1)[2]?.title === 'RPG' && /1/.test(rows(g1)[2]?.detail ?? ''), `${rows(g1)[2]?.id} / ${rows(g1)[2]?.detail}`);

const g2 = side('02-results');
check('results: honest page title', g2?.pageTitle === 'Popular Games: Action', g2?.pageTitle);
check('results: depth 3', g2?.depth === 3, `depth=${g2?.depth}`);
check('results: two ranked Action games', g2?.entryCount === 2, `count=${g2?.entryCount}`);
check('results: Havoc Runner ranked first', rows(g2)[0]?.id === 'game:result:0' && rows(g2)[0]?.title === 'Havoc Runner', `${rows(g2)[0]?.title}`);
check('results: Iron Bastion ranked second', rows(g2)[1]?.id === 'game:result:1' && rows(g2)[1]?.title === 'Iron Bastion', `${rows(g2)[1]?.title}`);

const g3 = side('03-details');
check('details: page title', g3?.pageTitle === 'Havoc Runner', g3?.pageTitle);
check('details: depth 4', g3?.depth === 4, `depth=${g3?.depth}`);
check('details: Download is the first row', rows(g3)[0]?.id === 'game:detail:download' && rows(g3)[0]?.title === 'Download', `${rows(g3)[0]?.id}`);
check('details: metadata rows present', rows(g3).some((r: any) => r.id === 'game:detail:genre') && rows(g3).some((r: any) => r.id === 'game:detail:year'), `count=${g3?.entryCount}`);

const g4 = side('04-sources');
check('sources: page title', g4?.pageTitle === 'Havoc Runner', g4?.pageTitle);
check('sources: depth 5', g4?.depth === 5, `depth=${g4?.depth}`);
check('sources: three options (two sources + one edition)', g4?.entryCount === 3, `count=${g4?.entryCount}`);
check('sources: Source Alpha standard 2 GB', rows(g4)[0]?.title === 'Source Alpha' && rows(g4)[0]?.fieldValue === '2 GB', `${rows(g4)[0]?.title} ${rows(g4)[0]?.fieldValue}`);
check('sources: Source Beta standard 3 GB', rows(g4)[1]?.title === 'Source Beta' && rows(g4)[1]?.fieldValue === '3 GB', `${rows(g4)[1]?.title} ${rows(g4)[1]?.fieldValue}`);
check('sources: Source Beta Deluxe 4 GB kept separate', rows(g4)[2]?.title === 'Source Beta' && rows(g4)[2]?.fieldValue === '4 GB' && /Deluxe/.test(rows(g4)[2]?.detail ?? ''), `${rows(g4)[2]?.detail}`);

const g5 = side('05-back-details');
check('back: source list returns to details', g5?.pageTitle === 'Havoc Runner' && g5?.depth === 4 && g5?.entryId === 'game:detail:download', `title=${g5?.pageTitle} depth=${g5?.depth} row=${g5?.entryId}`);
const g6 = side('06-back-results');
check('back: details returns to ranked results', g6?.pageTitle === 'Popular Games: Action' && g6?.depth === 3 && g6?.entryId === 'game:result:0', `title=${g6?.pageTitle} depth=${g6?.depth} row=${g6?.entryId}`);
const g7 = side('07-back-genres');
check('back: results returns to genre list', g7?.pageTitle === 'Browse by Genre' && g7?.depth === 2 && g7?.entryId === 'game:genre:Action', `title=${g7?.pageTitle} depth=${g7?.depth} row=${g7?.entryId}`);

check('loopback: only the hot and games endpoints were contacted', requests.length > 0 && requests.every((r) => r.startsWith('GET /hot') || r.startsWith('GET /games')), `${requests.length} requests`);
check('loopback: the hot catalogue was fetched once', requests.filter((r) => r.startsWith('GET /hot')).length === 1, requests.filter((r) => r.startsWith('GET /hot')).join(','));
check('loopback: no download/installer action was requested', !requests.some((r) => !r.startsWith('GET /hot') && !r.startsWith('GET /games')), requests.join(', '));

console.log(failures ? `\n${failures} FAILED (${out})` : `\nall passed (${out})`);
process.exit(failures ? 1 : 0);
