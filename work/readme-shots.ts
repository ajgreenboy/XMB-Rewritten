// Regenerates the README screenshots (native-xmb/screenshots/readme/) from a build, using only synthetic fixtures.
//
//   bun work/readme-shots.ts --exe C:/dev/out/readme-2/re-ps3-native.exe [--out C:/dev/out/readme-2-shots] [--only home,settings]
//       [--clip <video file>]
//
// Privacy and legal rules this script keeps (the images are public):
//   - Steam: a generated Steam root (XMB_STEAM_ROOT, XMB_STEAM_REGISTRY=0, XMB_STEAM_ARTWORK=0). Invented titles, neutral
//     artwork drawn here (text in a 5x7 bitmap font, plain shapes). Real Steam is never started, read or contacted.
//   - Jellyfin: work/jellyfin-standin.ts on loopback with its synthetic library and a synthetic test clip. Nothing leaves the machine.
//   - Friends: XMB_TEST_FRIENDS fixture names. Pending Installers: native-xmb/tests/installer_standin.cpp, never a real installer.
//   - Clock: --ui-clock / --background-clock / --render-clock are pinned, so no real date or time is drawn.
//   - Firmware UI textures are drawn because the XMB draws them (same precedent as the earlier README images).
// Every capture is the app's own `grab=` framebuffer grab at 1920x1080, then re-encoded as an optimised PNG
// (palette quantised only where the size demands it; see the README notes in native-xmb/SCREENSHOTS.md).
import { spawn, spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { cpSync, copyFileSync, existsSync, mkdirSync, readdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { deflateSync } from "node:zlib";

const args = process.argv.slice(2);
const opt = (name: string, dflt = "") => { const i = args.indexOf(`--${name}`); return i >= 0 && args[i + 1] ? args[i + 1] : dflt; };
const exe = opt("exe");
const out = opt("out", "C:/dev/out/readme-2-shots");
const flash = opt("flash", "C:/dev/rpcs3/bin/dev_flash");
const vsh = opt("vsh-modules", "C:/dev/xmb/fw493/vsh_module");
const clip = opt("clip", "C:/dev/out/jellyfin-standin-run/clip.mp4");
const only = opt("only", "").split(",").filter(Boolean);
const standinExe = opt("installer-standin", "C:/dev/out/installer-panel-tests/xmb-installer-standin.exe");
if (!exe || !existsSync(exe)) { console.error("pass --exe <re-ps3-native.exe>"); process.exit(2); }
const want = (name: string) => only.length === 0 || only.includes(name);
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

// ---- PNG helpers -------------------------------------------------------------------------------------------------
function crc32(buf: Buffer) {
  let c, crc = 0xffffffff;
  for (let n = 0; n < buf.length; n++) { c = (crc ^ buf[n]) & 0xff; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; crc = (crc >>> 8) ^ c; }
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
const put = (path: string, content: string | Buffer) => { mkdirSync(join(path, ".."), { recursive: true }); writeFileSync(path, content); };

// 5x7 bitmap font, upper case and digits. Used only to put an invented title on the synthetic logo art.
const FONT: Record<string, string[]> = {
  A: ["01110", "10001", "10001", "11111", "10001", "10001", "10001"], B: ["11110", "10001", "10001", "11110", "10001", "10001", "11110"],
  C: ["01110", "10001", "10000", "10000", "10000", "10001", "01110"], D: ["11110", "10001", "10001", "10001", "10001", "10001", "11110"],
  E: ["11111", "10000", "10000", "11110", "10000", "10000", "11111"], F: ["11111", "10000", "10000", "11110", "10000", "10000", "10000"],
  G: ["01110", "10001", "10000", "10111", "10001", "10001", "01111"], H: ["10001", "10001", "10001", "11111", "10001", "10001", "10001"],
  I: ["01110", "00100", "00100", "00100", "00100", "00100", "01110"], J: ["00111", "00010", "00010", "00010", "00010", "10010", "01100"],
  K: ["10001", "10010", "10100", "11000", "10100", "10010", "10001"], L: ["10000", "10000", "10000", "10000", "10000", "10000", "11111"],
  M: ["10001", "11011", "10101", "10101", "10001", "10001", "10001"], N: ["10001", "11001", "10101", "10011", "10001", "10001", "10001"],
  O: ["01110", "10001", "10001", "10001", "10001", "10001", "01110"], P: ["11110", "10001", "10001", "11110", "10000", "10000", "10000"],
  Q: ["01110", "10001", "10001", "10001", "10101", "10010", "01101"], R: ["11110", "10001", "10001", "11110", "10100", "10010", "10001"],
  S: ["01111", "10000", "10000", "01110", "00001", "00001", "11110"], T: ["11111", "00100", "00100", "00100", "00100", "00100", "00100"],
  U: ["10001", "10001", "10001", "10001", "10001", "10001", "01110"], V: ["10001", "10001", "10001", "10001", "10001", "01010", "00100"],
  W: ["10001", "10001", "10001", "10101", "10101", "11011", "10001"], X: ["10001", "10001", "01010", "00100", "01010", "10001", "10001"],
  Y: ["10001", "10001", "01010", "00100", "00100", "00100", "00100"], Z: ["11111", "00001", "00010", "00100", "01000", "10000", "11111"],
  " ": ["00000", "00000", "00000", "00000", "00000", "00000", "00000"],
};
function textMask(lines: string[], scale: number) {
  const gap = 1;
  const width = Math.max(...lines.map((l) => l.length * (5 + gap) - gap)) * scale;
  const height = (lines.length * 7 + (lines.length - 1) * 3) * scale;
  const inside = (x: number, y: number) => {
    if (y < 0 || x < 0) return false;
    const line = Math.floor(y / ((7 + 3) * scale)); const ly = Math.floor((y % ((7 + 3) * scale)) / scale);
    if (line >= lines.length || ly >= 7 || y >= height) return false;
    const text = lines[line]; const offset = Math.floor((width / scale - (text.length * (5 + gap) - gap)) / 2) * scale;
    const gx = Math.floor((x - offset) / scale); if (x < offset || gx < 0) return false;
    const ci = Math.floor(gx / (5 + gap)); const cx = gx % (5 + gap);
    if (ci >= text.length || cx >= 5) return false;
    return FONT[text[ci]]?.[ly]?.[cx] === "1";
  };
  return { width, height, inside };
}

// ---- synthetic Steam root ----------------------------------------------------------------------------------------
const STEAM_ID = "76561198000000001", ACCOUNT = 39734273;
// Invented titles. Not the names of any real game.
const TITLES: { id: string; name: string; lines: string[]; accent: [number, number, number]; played?: number; minutes?: number }[] = [
  { id: "4101", name: "Nova Harbor", lines: ["NOVA", "HARBOR"], accent: [60, 170, 230], played: 1795200000, minutes: 410 },
  { id: "4102", name: "Quiet Orbit", lines: ["QUIET", "ORBIT"], accent: [150, 120, 230], played: 1795100000, minutes: 95 },
  { id: "4103", name: "Glass Meridian", lines: ["GLASS", "MERIDIAN"], accent: [90, 210, 170], played: 1794900000, minutes: 1260 },
  { id: "4104", name: "Ember Courier", lines: ["EMBER", "COURIER"], accent: [240, 130, 60], played: 1794700000, minutes: 30 },
  { id: "4105", name: "Tidal Archive", lines: ["TIDAL", "ARCHIVE"], accent: [70, 140, 230], played: 1794500000, minutes: 640 },
  { id: "4106", name: "Lantern Run", lines: ["LANTERN", "RUN"], accent: [240, 200, 70], played: 1794300000, minutes: 215 },
  { id: "4107", name: "Hollow Atlas", lines: ["HOLLOW", "ATLAS"], accent: [200, 90, 130], played: 1794100000, minutes: 2 },
  { id: "4108", name: "Copper Sky", lines: ["COPPER", "SKY"], accent: [220, 150, 90] },
];
const logoPng = (t: (typeof TITLES)[number]) => {
  const scale = 9;
  const m = textMask(t.lines, scale);
  const ox = Math.floor((640 - m.width) / 2), oy = Math.floor((360 - m.height) / 2) - 14;
  const barY = oy + m.height + 22;
  return rgbaPng(640, 360, (x, y) => {
    if (m.inside(x - ox, y - oy)) return [246, 246, 248, 255];
    if (y >= barY && y < barY + 10 && x >= ox && x < ox + m.width) return [...t.accent, 255];
    return [0, 0, 0, 0];
  });
};
const headerPng = (t: (typeof TITLES)[number]) => rgbaPng(460, 215, (x, y) => {
  const k = y / 215; return [Math.round(24 + t.accent[0] * 0.3 * (1 - k)), Math.round(28 + t.accent[1] * 0.3 * (1 - k)), Math.round(40 + t.accent[2] * 0.35 * (1 - k)), 255];
});

function makeSteam(base: string) {
  const root = join(base, "steam");
  put(join(root, "steam.exe"), "stand-in\n");
  put(join(root, "config", "loginusers.vdf"),
    `"users"\n{\n\t"${STEAM_ID}"\n\t{\n\t\t"AccountName"\t\t"fixture"\n\t\t"PersonaName"\t\t"Player One"\n\t\t"RememberPassword"\t\t"1"\n\t\t"MostRecent"\t\t"1"\n\t\t"Timestamp"\t\t"1700000000"\n\t}\n}\n`);
  let apps = "";
  for (const t of TITLES) {
    const dir = t.name.replace(/ /g, "");
    put(join(root, "steamapps", `appmanifest_${t.id}.acf`),
      `"AppState"\n{\n\t"appid"\t\t"${t.id}"\n\t"Universe"\t\t"1"\n\t"name"\t\t"${t.name}"\n\t"StateFlags"\t\t"4"\n\t"installdir"\t\t"${dir}"\n\t"SizeOnDisk"\t\t"1000"\n}\n`);
    mkdirSync(join(root, "steamapps", "common", dir), { recursive: true });
    put(join(root, "appcache", "librarycache", t.id, "header.jpg"), headerPng(t));
    put(join(root, "appcache", "librarycache", t.id, "logo.png"), logoPng(t));
    if (t.played) apps += `\t\t\t\t\t"${t.id}"\n\t\t\t\t\t{\n\t\t\t\t\t\t"LastPlayed"\t\t"${t.played}"\n\t\t\t\t\t\t"Playtime"\t\t"${t.minutes}"\n\t\t\t\t\t}\n`;
  }
  put(join(root, "userdata", String(ACCOUNT), "config", "localconfig.vdf"),
    `"UserLocalConfigStore"\n{\n\t"Software"\n\t{\n\t\t"Valve"\n\t\t{\n\t\t\t"Steam"\n\t\t\t{\n\t\t\t\t"apps"\n\t\t\t\t{\n${apps}\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n`);
  return root;
}

// ---- run harness -------------------------------------------------------------------------------------------------
const CLOCK = ["--render-clock", "30", "--background-clock", "2026-01-15T14:00:00", "--ui-clock", "2026-01-15T14:00:00", "--focus-time", "2000"];
type Run = { name: string; tokens: string[]; env?: Record<string, string>; data?: (dir: string) => void; timeoutMs?: number; extraArgs?: string[]; realSteam?: boolean; during?: () => Promise<void> };
async function runXmb(r: Run, steamRoot: string) {
  const dir = join(out, "runs", r.name);
  rmSync(dir, { recursive: true, force: true });
  mkdirSync(join(dir, "data"), { recursive: true });
  r.data?.(dir);
  const argv = ["--dev-flash", flash, "--data-dir", join(dir, "data"), ...CLOCK, ...(r.extraArgs ?? []), "--script", r.tokens.join(",")];
  if (existsSync(join(vsh, "custom_render_plugin.prx"))) argv.splice(2, 0, "--vsh-modules", vsh);
  const env: Record<string, string | undefined> = {
    ...process.env, XMB_STEAM_ROOT: steamRoot, XMB_STEAM_REGISTRY: "0", XMB_STEAM_HIDE: "0", XMB_STEAM_ARTWORK: "0",
    XMB_NO_SETUP_ROWS: "1", XMB_SINGLE_INSTANCE: "0", STEAM_WEB_API_KEY: "", XMB_LOG: join(dir, "xmb.log"), XMB_DUMP_ENTRIES: join(dir, "entries.json"),
    XMB_SCRIPT_FRAMELESS: "1", ...(r.env ?? {}),
  };
  delete env.QT_QPA_PLATFORM;
  if (r.realSteam) { delete env.XMB_STEAM_ROOT; delete env.XMB_STEAM_REGISTRY; delete env.XMB_STEAM_HIDE; }
  const proc = spawn(exe, argv, { env: env as NodeJS.ProcessEnv, stdio: "ignore" });
  const killer = setTimeout(() => proc.kill(), r.timeoutMs ?? 180000);
  r.during?.().catch((e) => console.error("during:", e));
  const code: number = await new Promise((res) => proc.on("exit", (c) => res(c ?? -1)));
  clearTimeout(killer);
  console.log(`${r.name}: exit ${code}`);
  return dir;
}
const grabs = (dir: string, name: string) => join(dir, `${name}.png`).replace(/\\/g, "/");

// The Game column starts at the root; DOWN moves within the column. One token is applied every 300 ms.
async function main() {
  mkdirSync(out, { recursive: true });
  const fixtureDir = join(out, "fixture");
  rmSync(fixtureDir, { recursive: true, force: true });
  const steam = makeSteam(fixtureDir);
  const results: Record<string, string> = {};
  const probe = async (name: string, tokens: string[], more: Partial<Run> = {}) => {
    if (!want(name)) return;
    const dir = await runXmb({ name, tokens: ["full", "pause=3500", ...tokens, "quit"], ...more }, steam);
    results[name] = dir;
  };
  await probe("home", ["DOWN", "DOWN", "DOWN", "DOWN", "pause=1200", `grab=${grabs(join(out, "runs", "home"), "home")}`], { realSteam: true });
  await probe("settings", ["LEFT", "LEFT", "LEFT", "LEFT", "LEFT", "pause=600", "DOWN", "DOWN", "DOWN", "DOWN", "DOWN", "DOWN", "pause=1200", `grab=${grabs(join(out, "runs", "settings"), "settings")}`]);
  await probe("osk", ["LEFT", "LEFT", "LEFT", "LEFT", "LEFT", "pause=600", "osk=Search|", "pause=800", "type=hi", "pause=1200", `grab=${grabs(join(out, "runs", "osk"), "osk")}`]);
  await probe("psn", ["RIGHT", "RIGHT", "pause=700", "ENTER", "pause=500", `grab=${grabs(join(out, "runs", "psn"), "psn")}`]);
  await probe("toast", ["LEFT", "LEFT", "LEFT", "LEFT", "LEFT", "pause=600", "NOTIFY=Download complete|Sample Title is ready to play", "pause=1500", `grab=${grabs(join(out, "runs", "toast"), "toast")}`]);
  if (want("coldboot")) {
    // No --script (a script skips the intro). XMB_INTRO_GRAB saves the frame as drawn at each intro-clock second; the process is stopped after.
    const dir = join(out, "runs", "coldboot");
    rmSync(dir, { recursive: true, force: true }); mkdirSync(join(dir, "data"), { recursive: true });
    writeFileSync(join(dir, "data", "preferences.json"), JSON.stringify({ values: { "display/fullscreen": true }, version: 1 }));
    const argv = ["--dev-flash", flash, "--data-dir", join(dir, "data"), "--vsh-modules", vsh];
    const env = { ...process.env, XMB_STEAM_ROOT: steam, XMB_STEAM_REGISTRY: "0", XMB_STEAM_ARTWORK: "0", XMB_SINGLE_INSTANCE: "0", XMB_BOOT_CLOCK: "2026-01-15T14:00:00",
      XMB_INTRO_GRAB: join(dir, "grabs").split("\\").join("/") + "@0.6,1.0,1.3,1.6,2.0,3.0,6.0,9.0,12.0" };
    const proc = spawn(exe, argv, { env: env as NodeJS.ProcessEnv, stdio: "ignore" });
    await sleep(30000); proc.kill();
    console.log("coldboot: stopped");
  }
  const jellyfin = () => {
    const logFile = join(out, "standin.log");
    const p = spawn("bun", ["work/jellyfin-standin.ts", "--port", "8099", "--clip", clip, "--log", logFile], { stdio: "ignore", env: { ...process.env, STANDIN_TOKEN: "standin-readme-token" } });
    return p;
  };
  const jfData = (dir: string) => {
    put(join(dir, "data", "apps.json"), JSON.stringify({ apps: [{ arguments: [], category: "tv", id: "jellyfin", kind: "jellyfin", title: "Jellyfin", url: "http://127.0.0.1:8099" }], version: 1 }));
    put(join(dir, "data", "preferences.json"), JSON.stringify({ values: { "display/fullscreen": true }, version: 1 }));
  };
  if (want("jfvideo")) {
    const sp = jellyfin(); await sleep(1500);
    const d = join(out, "runs", "jfvideo"); const g = (n: string) => `grab=${join(d, n + ".png").split("\\").join("/")}`;
    await probe("jfvideo", ["LEFT", "UP", "pause=500", "ENTER", "pause=1500", g("a"), "ENTER", "pause=800", "type=tester", "OSKENTER", "pause=600", "DOWN", "pause=300", "ENTER", "pause=800", "type=standin-test", "OSKENTER", "pause=600", g("b"), "DOWN", "DOWN", "pause=300", "ENTER", "pause=4000", "DOWN", "DOWN", "DOWN", "DOWN", "pause=400", "ENTER", "pause=1500", g("c"), "ENTER", "pause=2500", g("p1"), "TRIANGLE", "pause=900", g("p2"), "TRIANGLE", "pause=300", "RIGHT", "pause=900", g("p3"), "DOWN", "pause=700", g("p4")], { data: jfData });
    spawnSync("taskkill", ["/PID", String(sp.pid), "/T", "/F"], { stdio: "ignore" });
  }
  if (want("jfmusic")) {
    const sp = jellyfin(); await sleep(1500);
    const d = join(out, "runs", "jfmusic"); const g = (n: string) => `grab=${join(d, n + ".png").split("\\").join("/")}`;
    const copyData = jfData;
    await probe("jfmusic", ["LEFT", "LEFT", "LEFT", "pause=700", "DOWN", "DOWN", "DOWN", "DOWN", "pause=500", g("m1"), "ENTER", "pause=1500", "ENTER", "pause=800", "type=tester", "OSKENTER", "pause=600", "DOWN", "pause=300", "ENTER", "pause=800", "type=standin-test", "OSKENTER", "pause=600", "DOWN", "DOWN", "pause=300", "ENTER", "pause=4000", g("m2"), "ENTER", "pause=2500", g("m3"), "ENTER", "pause=2500", g("m4"), "ENTER", "pause=3500", g("m5"), "ENTER", "pause=4000", g("m6")], { data: copyData });
    spawnSync("taskkill", ["/PID", String(sp.pid), "/T", "/F"], { stdio: "ignore" });
  }
  {
    const d = join(out, "runs", "gallery"); const g = (n: string) => `grab=${join(d, n + ".png").split("\\").join("/")}`;
    const galData = (dir: string) => put(join(dir, "data", "preferences.json"), JSON.stringify({ active: "local", version: 2, values: { "display/fullscreen": true, "profile/local/library/photo": "C:/dev/out/gal/photos" } }));
    await probe("gallery", ["LEFT", "LEFT", "LEFT", "LEFT", "pause=600", "DOWN", "pause=400", "ENTER", "pause=12000", "wait", "wait", g("piles"), "ENTER", "pause=1800", g("grid")], { data: galData });
  }
  if (want("installer")) {
    // Synthetic Inno-like stand-in (native-xmb/tests/installer_standin.cpp): a visible window, a /LOG file, files written to /DIR.
    const base = join(out, "installer-fixture"); rmSync(base, { recursive: true, force: true });
    const library = join(base, "drive"), games = join(library, "XMB Games"), pending = join(games, "Pending Installers", "synthetic");
    mkdirSync(pending, { recursive: true }); mkdirSync(join(games, "Download Archives", "synthetic"), { recursive: true }); mkdirSync(join(base, "no-steam"), { recursive: true });
    const setup = join(pending, "setup.exe"); copyFileSync(standinExe, setup);
    const sha = createHash("sha256").update(readFileSync(setup)).digest("hex");
    const win = (p: string) => p.split("/").join("\\");
    const seed = (dir: string) => put(join(dir, "data", "pending-installers.json"), JSON.stringify({ version: 1, entries: [{ id: "synthetic", title: "Synthetic Sample Game", installer: win(setup), archive: win(join(games, "Download Archives", "synthetic", "archive.zip")), destination: "", libraryRoot: win(games), sha256: sha, status: "pending", error: "" }] }));
    const envI = { XMB_STEAM_ROOT: join(base, "no-steam"), XMB_GAME_LIBRARY_ROOT: win(library), XMB_STANDIN_MODE: "success", XMB_STANDIN_SECONDS: "60", XMB_DIALOG_LOG: "1" };
    const d0 = join(out, "runs", "installer-find");
    const dd = await runXmb({ name: "installer-find", tokens: ["full", "pause=4000", `grab=${join(d0, "c.png").split("\\").join("/")}`, "quit"], data: seed, env: envI }, join(base, "no-steam"));
    const rows = JSON.parse(readFileSync(join(dd, "c.png.json"), "utf8")).entryRows as { id: string }[];
    const idx = rows.findIndex((r) => r.id === "game:pending-installers");
    console.log("pending installers row index", idx);
    const d = join(out, "runs", "installer"); const g = (n: string) => `grab=${join(d, n + ".png").split("\\").join("/")}`;
    await runXmb({ name: "installer", tokens: ["full", "pause=4000", ...Array(idx).fill("DOWN"), "pause=300", "ENTER", "pause=1500", "ENTER", "pause=1500", "DOWN", "DOWN", "DOWN", "pause=400", "ENTER", "pause=800", "RIGHT", "pause=300", "ENTER", "pause=2500", "pause=3000", g("panel"), "quit"], data: seed, env: envI, timeoutMs: 120000 }, join(base, "no-steam"));
    spawnSync("taskkill", ["/IM", "setup.exe", "/F", "/T"], { stdio: "ignore" });
  }
  // ---- final: pick one grab per shot and write the optimised PNG into the repository ----
  if (opt("publish", "") !== "") {
    const dest = opt("publish");
    mkdirSync(dest, { recursive: true });
    const pick = (name: string, run: string, file: string) => {
      const src = join(out, "runs", run, file);
      if (!existsSync(src)) { console.log("missing", src); return; }
      const r = spawnSync("bun", ["work/png-optimise.ts", src, join(dest, name), "--max-kb", "640"], { encoding: "utf8" });
      console.log(r.stdout.trim());
    };
    const intro = join(out, "runs", "coldboot", "grabs");
    const introFile = existsSync(intro) ? readdirSync(intro).filter((n) => /^intro_[0-9]/.test(n)).map((n) => ({ n, t: Number(n.slice(6, -4)) })).sort((a, b) => Math.abs(a.t - 1.2) - Math.abs(b.t - 1.2))[0]?.n : "";
    pick("01-game-column.png", "home", "home.png");
    pick("02-settings.png", "settings", "settings.png");
    pick("03-jellyfin-video-player.png", "jfvideo", "p2.png");
    pick("04-jellyfin-music-player.png", "jfmusic", "m6.png");
    pick("05-photo-gallery.png", "gallery", "piles.png");
    pick("06-on-screen-keyboard.png", "osk", "osk.png");
    pick("07-psn-sign-in.png", "psn", "psn.png");
    pick("08-pending-installers-panel.png", "installer", "panel.png");
    pick("09-notification-toast.png", "toast", "toast.png");
    if (introFile) pick("10-cold-boot-wordmark.png", "coldboot/grabs", introFile);
  }
  console.log(JSON.stringify(results, null, 1));
}
await main();
