// Scripted native run of the focused Game row's date line and "PS3" badge (native-xmb/src/game_badge.h).
//
//   bun work/game-badge-e2e.ts --exe C:/dev/out/game-badge/re-ps3-native.exe [--out C:/dev/out/game-badge-e2e]
//
// Synthetic and offline: a generated Steam root (one account, two installed stand-in titles, play records for
// only one of them), XMB_STEAM_REGISTRY=0 and XMB_STEAM_ARTWORK=0. Real Steam is never started or read.
// Screenshots land in --out: badge-on (default), badge-off (XMB_GAME_BADGE=0).
import { spawn } from "node:child_process";
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { deflateSync } from "node:zlib";

const args = process.argv.slice(2);
const opt = (name: string, dflt = "") => {
  const i = args.indexOf(`--${name}`);
  return i >= 0 && args[i + 1] ? args[i + 1] : dflt;
};
const exe = opt("exe");
const out = opt("out", "C:/dev/out/game-badge-e2e");
const flash = opt("flash", "C:/dev/rpcs3/bin/dev_flash");
const vsh = opt("vsh-modules", "C:/dev/xmb/fw493/vsh_module");
if (!exe || !existsSync(exe)) { console.error("pass --exe <re-ps3-native.exe>"); process.exit(2); }

const problems: string[] = [];
const expect = (ok: boolean, what: string) => { console.log(`${ok ? "ok  " : "FAIL"} ${what}`); if (!ok) problems.push(what); };

function crc32(buf: Buffer) {
  let c, crc = 0xffffffff;
  for (let n = 0; n < buf.length; n++) {
    c = (crc ^ buf[n]) & 0xff;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    crc = (crc >>> 8) ^ c;
  }
  return (crc ^ 0xffffffff) >>> 0;
}
function rgbaPng(w: number, h: number, pixel: (x: number, y: number) => [number, number, number, number]) {
  const chunk = (type: string, body: Buffer) => {
    const t = Buffer.concat([Buffer.from(type), body]);
    const len = Buffer.alloc(4); len.writeUInt32BE(body.length);
    const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(t));
    return Buffer.concat([len, t, crc]);
  };
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 6;
  const raw = Buffer.alloc((w * 4 + 1) * h);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) raw.set(pixel(x, y), y * (w * 4 + 1) + 1 + x * 4);
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk("IHDR", ihdr), chunk("IDAT", deflateSync(raw)), chunk("IEND", Buffer.alloc(0))]);
}
const logoPng = () => rgbaPng(640, 360, (x, y) =>
  x >= 100 && x < 540 && y >= 120 && y < 170 ? [245, 245, 245, 255] : x >= 140 && x < 500 && y >= 180 && y < 220 ? [230, 40, 40, 255] : [0, 0, 0, 0]);
const put = (path: string, content: string | Buffer) => { mkdirSync(join(path, ".."), { recursive: true }); writeFileSync(path, content); };

const ID = "76561198000000001", ACCT = 39734273;
const games = [{ id: "3001", name: "Played Title" }, { id: "3002", name: "Never Played Title" }];
const base = join(out, "fixture");
rmSync(base, { recursive: true, force: true });
const root = join(base, "steam"), data = join(base, "data");
mkdirSync(data, { recursive: true });
put(join(root, "steam.exe"), "stand-in\n");
put(join(root, "config", "loginusers.vdf"),
  `"users"\n{\n\t"${ID}"\n\t{\n\t\t"AccountName"\t\t"fixture"\n\t\t"PersonaName"\t\t"Fixture"\n\t\t"RememberPassword"\t\t"1"\n\t\t"MostRecent"\t\t"1"\n\t\t"Timestamp"\t\t"1700000000"\n\t}\n}\n`);
for (const g of games) {
  put(join(root, "steamapps", `appmanifest_${g.id}.acf`),
    `"AppState"\n{\n\t"appid"\t\t"${g.id}"\n\t"Universe"\t\t"1"\n\t"name"\t\t"${g.name}"\n\t"StateFlags"\t\t"4"\n\t"installdir"\t\t"${g.name.replace(/ /g, "")}"\n\t"SizeOnDisk"\t\t"1000"\n}\n`);
  mkdirSync(join(root, "steamapps", "common", g.name.replace(/ /g, "")), { recursive: true });
  put(join(root, "appcache", "librarycache", g.id, "logo.png"), logoPng());
}
// 2026-11-21 07:34 UTC, 125 minutes. The second title has no record at all.
put(join(root, "userdata", String(ACCT), "config", "localconfig.vdf"),
  `"UserLocalConfigStore"\n{\n\t"Software"\n\t{\n\t\t"Valve"\n\t\t{\n\t\t\t"Steam"\n\t\t\t{\n\t\t\t\t"apps"\n\t\t\t\t{\n\t\t\t\t\t"3001"\n\t\t\t\t\t{\n\t\t\t\t\t\t"LastPlayed"\t\t"1795246440"\n\t\t\t\t\t\t"Playtime"\t\t"125"\n\t\t\t\t\t}\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n`);

const env: Record<string, string | undefined> = {
  ...process.env, XMB_STEAM_ROOT: root, XMB_STEAM_REGISTRY: "0", XMB_STEAM_HIDE: "0", XMB_STEAM_ARTWORK: "0",
  XMB_NO_SETUP_ROWS: "1", STEAM_WEB_API_KEY: "",
};
delete env.QT_QPA_PLATFORM;
delete env.XMB_STEAM_ACTIVITY;

async function run(tag: string, extra: Record<string, string>, moves: string[]) {
  const shot = join(out, `${tag}.png`), dump = join(out, `${tag}-entries.json`);
  const argv = ["--dev-flash", flash, "--data-dir", data, "--script", ["wait", ...moves, "wait", "wait", "wait", `grab=${shot}`, "quit"].join(",")];
  if (existsSync(join(vsh, "custom_render_plugin.prx"))) argv.splice(2, 0, "--vsh-modules", vsh);
  const proc = spawn(exe, argv, { env: { ...env, ...extra, XMB_DUMP_ENTRIES: dump } as NodeJS.ProcessEnv, stdio: "ignore" });
  const killer = setTimeout(() => proc.kill(), 90000);
  const code: number = await new Promise((res) => proc.on("exit", (c) => res(c ?? -1)));
  clearTimeout(killer);
  const entries = existsSync(dump) ? JSON.parse(readFileSync(dump, "utf8")) : { categories: [] };
  const game = (entries.categories as any[]).find((c) => c.id === "game");
  return { code, shot, rows: ((game?.entries ?? []) as any[]).filter((r) => /^steam:\d+$/.test(r.id)), meta: existsSync(shot + ".json") ? JSON.parse(readFileSync(shot + ".json", "utf8")) : {} };
}

mkdirSync(out, { recursive: true });
const moves = (opt("moves", "RIGHT,RIGHT,RIGHT,RIGHT")).split(",").filter((m) => m && m !== "none");
const on = await run("badge-on", {}, moves);
expect(on.code === 0, "default run exits 0");
console.log("category:", on.meta.category, "selection:", on.meta.selection, "rows:", on.rows.map((r) => `${r.id} detail=${JSON.stringify(r.detail)} lastPlayed=${r.lastPlayed}`));
const played = on.rows.find((r) => r.id === "steam:3001"), never = on.rows.find((r) => r.id === "steam:3002");
expect(!!played, "played title listed");
expect(!!never, "never-played title listed");
const off = await run("badge-off", { XMB_GAME_BADGE: "0" }, moves);
expect(off.code === 0, "opt-out run exits 0");
console.log("badge-off rows:", off.rows.map((r) => `${r.id} detail=${JSON.stringify(r.detail)}`));
expect(/Last played/.test(off.rows.find((r) => r.id === "steam:3001")?.detail ?? ""), "XMB_GAME_BADGE=0 keeps the date in the detail line");
console.log(`screenshots: ${on.shot} ${off.shot}`);
console.log(problems.length ? `\nFAILED ${problems.length} check(s)` : "\ngame-badge e2e: all checks passed");
process.exit(problems.length ? 1 : 0);
