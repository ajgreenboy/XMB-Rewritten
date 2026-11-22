// Scripted native run of the PS3 logo tiles (native-xmb/src/logo_tiles.h): installed Steam titles wear their
// transparent logo, unframed, instead of the boxed header; XMB_PS3_LOGO_TILES=0 restores the header look.
//
//   bun work/ps3-logo-tiles-e2e.ts --exe C:/dev/out/ps3-logo-tiles/re-ps3-native.exe [--out C:/dev/out/ps3-logo-tiles-e2e]
//
// Everything is synthetic and offline (XMB_STEAM_ARTWORK=0, so no request is ever made): a generated Steam root
// with appcache/librarycache/<id>/ holding a logo.png (direct), a logo.png in a hashed sub-folder, and a title with
// only a header.jpg. Real Steam is never started or read. Screenshots land in --out for a visual check.
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
const out = opt("out", "C:/dev/out/ps3-logo-tiles-e2e");
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
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) { const p = pixel(x, y); raw.set(p, y * (w * 4 + 1) + 1 + x * 4); }
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk("IHDR", ihdr), chunk("IDAT", deflateSync(raw)), chunk("IEND", Buffer.alloc(0))]);
}
// A stand-in logo: a wide white wordmark-ish bar pair with a red accent on a transparent 640x360 canvas, so the
// trimmed bounds (about 440x100) differ from the canvas and a wrong crop or a frame would show.
const logoPng = () => rgbaPng(640, 360, (x, y) => {
  if (x >= 100 && x < 540 && y >= 120 && y < 170) return [245, 245, 245, 255];
  if (x >= 140 && x < 500 && y >= 180 && y < 220) return [230, 40, 40, 255];
  return [0, 0, 0, 0];
});
const headerPng = () => rgbaPng(460, 215, () => [30, 70, 200, 255]);
const put = (path: string, content: string | Buffer) => { mkdirSync(join(path, ".."), { recursive: true }); writeFileSync(path, content); };

const games = [
  { id: "2001", name: "Direct Logo Game" },
  { id: "2002", name: "Hashed Logo Game" },
  { id: "2003", name: "Header Only Game" },
];
const base = join(out, "fixture");
rmSync(base, { recursive: true, force: true });
const root = join(base, "steam");
const data = join(base, "data");
mkdirSync(data, { recursive: true });
put(join(root, "steam.exe"), "stand-in\n");
for (const g of games) {
  put(join(root, "steamapps", `appmanifest_${g.id}.acf`),
    `"AppState"\n{\n\t"appid"\t\t"${g.id}"\n\t"Universe"\t\t"1"\n\t"name"\t\t"${g.name}"\n\t"StateFlags"\t\t"4"\n\t"installdir"\t\t"${g.name.replace(/ /g, "")}"\n\t"SizeOnDisk"\t\t"1000"\n}\n`);
  mkdirSync(join(root, "steamapps", "common", g.name.replace(/ /g, "")), { recursive: true });
  put(join(root, "appcache", "librarycache", g.id, "header.jpg"), headerPng());
}
put(join(root, "appcache", "librarycache", "2001", "logo.png"), logoPng());
put(join(root, "appcache", "librarycache", "2002", "0f1e2d3c4b5a69788796a5b4c3d2e1f0", "logo.png"), logoPng());

const env: Record<string, string | undefined> = {
  ...process.env,
  XMB_STEAM_ROOT: root,
  XMB_STEAM_REGISTRY: "0",
  XMB_STEAM_HIDE: "0",
  XMB_STEAM_ARTWORK: "0",
  XMB_NO_SETUP_ROWS: "1",
  STEAM_WEB_API_KEY: "",
};
delete env.QT_QPA_PLATFORM;

async function run(tag: string, extra: Record<string, string>) {
  const shot = join(out, `${tag}.png`);
  const dump = join(out, `${tag}-entries.json`);
  const argv = ["--dev-flash", flash, "--data-dir", data, "--script", ["wait", "wait", "wait", `grab=${shot}`, "quit"].join(",")];
  if (existsSync(join(vsh, "custom_render_plugin.prx"))) argv.splice(2, 0, "--vsh-modules", vsh);
  const proc = spawn(exe, argv, { env: { ...env, ...extra, XMB_DUMP_ENTRIES: dump } as NodeJS.ProcessEnv, stdio: "ignore" });
  const killer = setTimeout(() => proc.kill(), 90000);
  const code: number = await new Promise((res) => proc.on("exit", (c) => res(c ?? -1)));
  clearTimeout(killer);
  const entries = existsSync(dump) ? JSON.parse(readFileSync(dump, "utf8")) : { categories: [] };
  const game = (entries.categories as any[]).find((c) => c.id === "game");
  const rows = ((game?.entries ?? []) as any[]).filter((r) => /^steam:\d+$/.test(r.id));
  return { code, rows, shot };
}

mkdirSync(out, { recursive: true });
const on = await run("logo-on", {});
expect(on.code === 0, "default run exits 0");
console.log("logo-on rows:", on.rows.map((r) => `${r.id} logoArt=${!!r.logoArt} hasArtwork=${r.hasArtwork}`));
const byId = (rows: any[], id: string) => rows.find((r) => r.id === `steam:${id}`);
expect(on.rows.length === 3, "three Steam rows in the Game column");
expect(!!byId(on.rows, "2001")?.logoArt, "client logo.png (direct) -> logo row");
expect(!!byId(on.rows, "2002")?.logoArt, "client logo.png in a hashed sub-folder -> logo row");
expect(byId(on.rows, "2003") && !byId(on.rows, "2003").logoArt && byId(on.rows, "2003").hasArtwork === true, "no logo -> header art kept (fallback)");

const off = await run("logo-off", { XMB_PS3_LOGO_TILES: "0" });
expect(off.code === 0, "opt-out run exits 0");
console.log("logo-off rows:", off.rows.map((r) => `${r.id} logoArt=${!!r.logoArt} hasArtwork=${r.hasArtwork}`));
expect(off.rows.length === 3 && off.rows.every((r) => !r.logoArt), "XMB_PS3_LOGO_TILES=0: no logo rows");
expect(off.rows.every((r) => r.hasArtwork === true), "XMB_PS3_LOGO_TILES=0: every row wears its header");

console.log(`screenshots: ${on.shot} ${off.shot}`);
console.log(problems.length ? `\nFAILED ${problems.length} check(s)` : "\nps3-logo-tiles e2e: all checks passed");
process.exit(problems.length ? 1 : 0);
