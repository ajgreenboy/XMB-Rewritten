// Scripted native run of "switch Steam account from the XMB, then the Game column follows the new account"
// (audit/STEAM-ACCOUNT-SWITCH.md, "Library refresh after a switch").
//
//   bun work/steam-switch-refresh-e2e.ts --exe C:/dev/out/steam-switch-refresh/re-ps3-native.exe [--keep]
//
// Everything is synthetic and scratch: a generated Steam root (two accounts in config/loginusers.vdf, three
// appmanifests, a different play record and a different grid artwork colour per account), a scratch data
// directory and a scratch registry key under HKCU\Software\XmbTest\. XMB_STEAM_SWITCH_NOPROC=1 means no Steam
// process is ever started or stopped; the script stands in for Steam by writing ActiveProcess\ActiveUser into
// the scratch key once the XMB has rewritten loginusers.vdf. The real Steam key's AutoLoginUser is compared
// before and after. Nothing is committed from the fixture.
import { spawn, spawnSync } from "node:child_process";
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { deflateSync } from "node:zlib";

const args = process.argv.slice(2);
const opt = (name: string, dflt = "") => {
  const i = args.indexOf(`--${name}`);
  return i >= 0 && args[i + 1] ? args[i + 1] : dflt;
};
const exe = opt("exe");
const keep = args.includes("--keep");
const flash = opt("flash", "C:/dev/rpcs3/bin/dev_flash");
const vsh = opt("vsh-modules", "C:/dev/xmb/fw493/vsh_module");
if (!exe || !existsSync(exe)) { console.error("pass --exe <re-ps3-native.exe>"); process.exit(2); }

const problems: string[] = [];
const expect = (ok: boolean, what: string) => { console.log(`${ok ? "ok  " : "FAIL"} ${what}`); if (!ok) problems.push(what); };

const ID_A = "76561198000000001"; // account id 39734273
const ID_B = "76561198000000002"; // account id 39734274
const ACCT_A = 39734273;
const ACCT_B = 39734274;

// ---- fixture ----
const scratch = mkdtempSync(join(tmpdir(), "xmb-switch-refresh-"));
const root = join(scratch, "steam");
const data = join(scratch, "data");
const regKey = `HKEY_CURRENT_USER\\Software\\XmbTest\\SwitchRefreshE2E-${process.pid}`;
const regPath = regKey.replace("HKEY_CURRENT_USER", "HKCU");

function crc32(buf: Buffer) {
  let c, crc = 0xffffffff;
  for (let n = 0; n < buf.length; n++) {
    c = (crc ^ buf[n]) & 0xff;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    crc = (crc >>> 8) ^ c;
  }
  return (crc ^ 0xffffffff) >>> 0;
}
function png(size: number, r: number, g: number, b: number) {
  const chunk = (type: string, body: Buffer) => {
    const t = Buffer.concat([Buffer.from(type), body]);
    const len = Buffer.alloc(4); len.writeUInt32BE(body.length);
    const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(t));
    return Buffer.concat([len, t, crc]);
  };
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(size, 0); ihdr.writeUInt32BE(size, 4); ihdr[8] = 8; ihdr[9] = 2;
  const row = Buffer.concat([Buffer.from([0]), Buffer.from(Array.from({ length: size }, () => [r, g, b]).flat())]);
  const raw = Buffer.concat(Array.from({ length: size }, () => row));
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk("IHDR", ihdr), chunk("IDAT", deflateSync(raw)), chunk("IEND", Buffer.alloc(0))]);
}
const put = (path: string, content: string | Buffer) => { mkdirSync(join(path, ".."), { recursive: true }); writeFileSync(path, content); };

const games = [
  { id: "1001", name: "Alpha Quest" },
  { id: "1002", name: "Bravo Racer" },
  { id: "1003", name: "Charlie Tactics" },
];
const user = (id: string, name: string, persona: string, recent: number) =>
  `\t"${id}"\n\t{\n\t\t"AccountName"\t\t"${name}"\n\t\t"PersonaName"\t\t"${persona}"\n\t\t"RememberPassword"\t\t"1"\n` +
  `\t\t"WantsOfflineMode"\t\t"0"\n\t\t"SkipOfflineModeWarning"\t\t"0"\n\t\t"AllowAutoLogin"\t\t"${recent}"\n\t\t"MostRecent"\t\t"${recent}"\n\t\t"Timestamp"\t\t"1700000000"\n\t}\n`;
put(join(root, "config", "loginusers.vdf"), `"users"\n{\n${user(ID_A, "fixture_a", "Persona Aye", 1)}${user(ID_B, "fixture_b", "Persona Bee", 0)}}\n`);
put(join(root, "steam.exe"), "not a real client; NOPROC never starts it\n");
for (const g of games) {
  put(join(root, "steamapps", `appmanifest_${g.id}.acf`),
    `"AppState"\n{\n\t"appid"\t\t"${g.id}"\n\t"Universe"\t\t"1"\n\t"name"\t\t"${g.name}"\n\t"StateFlags"\t\t"4"\n\t"installdir"\t\t"${g.name.replace(/ /g, "")}"\n\t"SizeOnDisk"\t\t"1000"\n}\n`);
  mkdirSync(join(root, "steamapps", "common", g.name.replace(/ /g, "")), { recursive: true });
}
// Account A: Bravo played most recently, then Alpha, Charlie never. Account B: Charlie, then Alpha, Bravo never.
const DAY = 86400, T0 = 1_762_000_000; // 2025-11-01
const localconfig = (rec: Record<string, [number, number]>) =>
  `"UserLocalConfigStore"\n{\n\t"Software"\n\t{\n\t\t"Valve"\n\t\t{\n\t\t\t"Steam"\n\t\t\t{\n\t\t\t\t"apps"\n\t\t\t\t{\n` +
  Object.entries(rec).map(([id, [last, mins]]) => `\t\t\t\t\t"${id}"\n\t\t\t\t\t{\n\t\t\t\t\t\t"LastPlayed"\t\t"${last}"\n\t\t\t\t\t\t"Playtime"\t\t"${mins}"\n\t\t\t\t\t}\n`).join("") +
  `\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n`;
put(join(root, "userdata", String(ACCT_A), "config", "localconfig.vdf"), localconfig({ "1002": [T0 + 30 * DAY, 125], "1001": [T0 + 5 * DAY, 60] }));
put(join(root, "userdata", String(ACCT_B), "config", "localconfig.vdf"), localconfig({ "1003": [T0 + 40 * DAY, 300], "1001": [T0 + 10 * DAY, 15] }));
for (const g of games) {
  put(join(root, "userdata", String(ACCT_A), "config", "grid", `${g.id}.png`), png(16, 200, 20, 20));
  put(join(root, "userdata", String(ACCT_B), "config", "grid", `${g.id}.png`), png(16, 20, 20, 200));
}
mkdirSync(data, { recursive: true });

// ---- registry (scratch key only) ----
const reg = (...a: string[]) => spawnSync("reg", a, { encoding: "utf8" });
const setActive = (acct: number) => reg("add", `${regPath}\\ActiveProcess`, "/v", "ActiveUser", "/t", "REG_DWORD", "/d", String(acct), "/f");
const realAutoLogin = () => reg("query", "HKCU\\Software\\Valve\\Steam", "/v", "AutoLoginUser").stdout.trim();
const realBefore = realAutoLogin();
setActive(ACCT_A);

// ---- run ----
const messages = join(scratch, "app.log");
const dump = join(scratch, "entries.json");
const env: Record<string, string | undefined> = {
  ...process.env,
  XMB_STEAM_ROOT: root,
  XMB_STEAM_REGISTRY_KEY: regKey,
  XMB_STEAM_REGISTRY: "0",
  XMB_STEAM_SWITCH_NOPROC: "1",
  XMB_STEAM_SWITCH_VERIFY_MS: "20000",
  XMB_STEAM_HIDE: "0",
  XMB_STEAM_ARTWORK: "0",
  XMB_LOG: messages,
  XMB_DUMP_ENTRIES: dump,
  STEAM_WEB_API_KEY: "",
};
delete env.QT_QPA_PLATFORM;

async function runApp(script: string[], onTick?: () => void, timeoutMs = 90000) {
  const argv = ["--dev-flash", flash, "--data-dir", data, "--script", script.join(",")];
  if (existsSync(join(vsh, "custom_render_plugin.prx"))) argv.splice(2, 0, "--vsh-modules", vsh);
  const proc = spawn(exe, argv, { env: env as NodeJS.ProcessEnv, stdio: "ignore" });
  const timer = setInterval(() => onTick?.(), 200);
  const killer = setTimeout(() => { console.error("timed out; killing pid " + proc.pid); proc.kill(); }, timeoutMs);
  const code: number = await new Promise((res) => proc.on("exit", (c) => res(c ?? -1)));
  clearInterval(timer); clearTimeout(killer);
  return code;
}
const j = (p: string) => JSON.parse(readFileSync(p, "utf8"));
const steamRows = (side: any) => (side.entryRows as any[]).filter((r) => String(r.id).startsWith("steam:") && !String(r.id).startsWith("steam-"));

// Probe: where is Steam Account in the Users column, and what is selected there?
const probeGrab = join(scratch, "probe.png");
let code = await runApp(["wait", ...Array(6).fill("LEFT"), "wait", `grab=${probeGrab}`, "quit"]);
expect(code === 0, "probe run exits 0");
const probe = j(probeGrab + ".json");
const rowIds: string[] = probe.entryRows.map((r: any) => r.id);
const target = rowIds.indexOf("steam-accounts");
expect(probe.category === "users" || probe.categoryIndex === 0, `probe is on the Users column (category ${probe.category})`);
expect(target >= 0, `Users column has the Steam Account row (index ${target} of ${rowIds.length})`);
const delta = target - probe.selection;
const moves = delta >= 0 ? Array(delta).fill("DOWN") : Array(-delta).fill("UP");

const before = join(scratch, "before.png");
const after = join(scratch, "after.png");
const loginUsers = join(root, "config", "loginusers.vdf");
const loginBefore = readFileSync(loginUsers, "utf8");
let wroteActive = false;
const tick = () => {
  // Steam's stand-in: once the XMB has written the new selection, "Steam" reports the new ActiveUser.
  if (!wroteActive && readFileSync(loginUsers, "utf8") !== loginBefore) { wroteActive = true; setActive(ACCT_B); console.log("stand-in: ActiveUser -> account B"); }
};
const script = [
  "wait", "wait", "DOWN", "DOWN", "wait", `grab=${before}`,         // Game column, third row selected (Charlie in A order)
  ...Array(6).fill("LEFT"), "wait", ...moves, "wait", "ENTER", "wait", "wait",
  "DOWN", "wait", "ENTER", "wait", "wait",                          // pick the other account -> prompt 1
  "LEFT", "wait", "ENTER", "wait", "wait",                          // Yes
  "LEFT", "wait", "ENTER", "wait",                                  // prompt 2: Yes -> the handoff runs
  "pause=9000",                                                      // verify, refresh, rescan
  "ESC", "wait", "ESC", "wait", "ESC", "wait",
  ...Array(6).fill("RIGHT"), "wait", "wait", "wait", `grab=${after}`, "quit",
];
code = await runApp(script, tick);
expect(code === 0, "switch run exits 0");
expect(wroteActive, "the XMB rewrote loginusers.vdf (the switch ran)");

const log = existsSync(messages) ? readFileSync(messages, "utf8") : "";
expect(/steam: handoff finished outcome=0/.test(log), "switch outcome SignedIn (0)");
expect(/steam: account changed \(Persona Bee\); refreshing the library/.test(log), "log: account changed (Persona Bee)");
expect(/content: Steam installed 3/.test(log), "log: library rescanned");

const b = j(before + ".json"), a = j(after + ".json");
const rb = steamRows(b), ra = steamRows(a);
expect(b.category === "game" && a.category === "game", "both grabs are on the Game column");
console.log("before:", rb.map((r) => `${r.id} ${r.title} | ${r.detail} | rgb ${r.artworkRgb}`));
console.log("after: ", ra.map((r) => `${r.id} ${r.title} | ${r.detail} | rgb ${r.artworkRgb}`));
expect(rb.map((r) => r.id).join() === "steam:1002,steam:1001,steam:1003", "before: account A order Bravo, Alpha, Charlie");
expect(ra.map((r) => r.id).join() === "steam:1003,steam:1001,steam:1002", "after: account B order Charlie, Alpha, Bravo");
expect(![...rb, ...ra].some((r) => /played/i.test(String(r.detail))), "no row carries play time or a last-played date (owner direction 2026-11-23; the order still follows the play records)");
expect(rb.every((r) => r.artworkReady) && ra.every((r) => r.artworkReady), "artwork loaded on every row, both times");
const [rr, , rbl] = String(rb[0]?.artworkRgb).split(",").map(Number);
const [ar, , abl] = String(ra[0]?.artworkRgb).split(",").map(Number);
expect(rr > 150 && rbl < 80, `before: account A's grid artwork (red) rgb ${rb[0]?.artworkRgb}`);
expect(abl > 150 && ar < 80, `after: account B's grid artwork (blue) rgb ${ra[0]?.artworkRgb}`);
expect(b.entryId === "steam:1003" && a.entryId === b.entryId, `selection kept by id (${b.entryId} -> ${a.entryId})`);
const idxBefore = rb.findIndex((r) => r.id === b.entryId), idxAfter = ra.findIndex((r) => r.id === a.entryId);
expect(idxBefore === 2 && idxAfter === 0, "the kept row moved with the new order (third Steam row to first)");

const realAfter = realAutoLogin();
expect(realBefore === realAfter, "the real HKCU\\Software\\Valve\\Steam AutoLoginUser is unchanged");

reg("delete", regPath, "/f");
if (!keep) rmSync(scratch, { recursive: true, force: true }); else console.log("kept scratch:", scratch);
console.log(problems.length ? `\nFAILED ${problems.length} check(s)` : "\nsteam-switch-refresh e2e: all checks passed");
process.exit(problems.length ? 1 : 0);
