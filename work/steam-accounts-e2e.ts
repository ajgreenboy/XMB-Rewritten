// Scripted native run of "the Game column follows the signed-in Steam account", with no Steam Web API key
// (audit/STEAM-ACCOUNT-SWITCH.md, "Full account switch" and "Library without a key").
//
//   bun work/steam-accounts-e2e.ts --exe C:/dev/out/steam-accounts/re-ps3-native.exe [--keep]
//
// Everything is synthetic and scratch: a generated Steam root with two accounts (fake SteamID64s 7656119800000000x),
// per-account localconfig.vdf play records, static collections, librarycache files, a binary appinfo.vdf (v29)
// built here, two manifests for "installed" games of the PC, a stale family-shared file (the feature was dropped; it must be ignored), and a scratch
// registry key under HKCU\Software\XmbTest\. The script stands in for the Steam client by writing
// ActiveProcess\ActiveUser (and AutoLoginUser) into that scratch key only; no Steam process is started, stopped or
// read, and the real Steam key is never written. The app dumps its catalogue (XMB_DUMP_ENTRIES) after every library
// rescan, which is what is checked: no pixels involved.
//
// Proves: with no key the owned list is the signed-in account's keyless library and only that; a registry
// ActiveUser change made OUTSIDE the XMB (a Steam-client switch) swaps the owned list, the collections, play-record
// ordering and persona with no stale row; switching back restores the first account; and a fresh
// start with a signed-out client selects the account named by AutoLoginUser (the "restart" case).
import { spawn, spawnSync } from "node:child_process";
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

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

// A saved Web API key would make the app ask Valve; this e2e is about the keyless path. The child gets a scratch
// APPDATA and XMB_API_KEY_DIR (the key-file seam of the API Keys page), so the owner's real key file is never read.
const ID_A = "76561198000000001"; const ACCT_A = 39734273;
const ID_B = "76561198000000002"; const ACCT_B = 39734274;

const scratch = mkdtempSync(join(tmpdir(), "xmb-steam-accounts-"));
const root = join(scratch, "steam");
const data = join(scratch, "data");
const regKey = `HKEY_CURRENT_USER\\Software\\XmbTest\\SteamAccountsE2E-${process.pid}`;
const regPath = regKey.replace("HKEY_CURRENT_USER", "HKCU");
const put = (path: string, content: string | Buffer) => { mkdirSync(join(path, ".."), { recursive: true }); writeFileSync(path, content); };

// ---- appinfo.vdf (v29), documented in native-xmb/src/steam_appinfo.h ----
const u32 = (v: number) => { const b = Buffer.alloc(4); b.writeUInt32LE(v >>> 0); return b; };
function appinfoV29(apps: { id: number; name: string; type: string }[]) {
  const table: string[] = [];
  const key = (k: string) => { let i = table.indexOf(k); if (i < 0) { table.push(k); i = table.length - 1; } return u32(i); };
  const str = (k: string, v: string) => Buffer.concat([Buffer.from([1]), key(k), Buffer.from(v + "\0")]);
  const obj = (k: string, ...kids: Buffer[]) => Buffer.concat([Buffer.from([0]), key(k), ...kids, Buffer.from([8])]);
  const entries: Buffer[] = [];
  for (const a of apps) {
    const kv = Buffer.concat([obj("appinfo", Buffer.concat([Buffer.from([2]), key("appid"), u32(a.id)]), obj("common", str("name", a.name), str("type", a.type))), Buffer.from([8])]);
    const fixed = Buffer.concat([u32(2), u32(1700000000), Buffer.alloc(8), Buffer.alloc(20, 0x11), u32(7), Buffer.alloc(20, 0x22)]);
    entries.push(Buffer.concat([u32(a.id), u32(fixed.length + kv.length), fixed, kv]));
  }
  const body = Buffer.concat([...entries, u32(0)]);
  const header = Buffer.concat([u32(0x07564429), u32(1), Buffer.alloc(8)]);
  header.writeBigInt64LE(BigInt(16 + body.length), 8);
  const tableBytes = Buffer.concat([u32(table.length), ...table.map((s) => Buffer.from(s + "\0"))]);
  return Buffer.concat([header, body, tableBytes]);
}

const user = (id: string, name: string, persona: string, ts: number) =>
  `\t"${id}"\n\t{\n\t\t"AccountName"\t\t"${name}"\n\t\t"PersonaName"\t\t"${persona}"\n\t\t"RememberPassword"\t\t"1"\n\t\t"AllowAutoLogin"\t\t"1"\n\t\t"timestamp"\t\t"${ts}"\n\t}\n`;
put(join(root, "config", "loginusers.vdf"), `"users"\n{\n${user(ID_A, "fixture_a", "Persona Aye", 1790000000)}${user(ID_B, "fixture_b", "Persona Bee", 1780000000)}}\n`);
put(join(root, "steam.exe"), "not a real client; nothing ever starts it\n");
const manifest = (id: number, name: string) =>
  `"AppState"\n{\n\t"appid"\t\t"${id}"\n\t"name"\t\t"${name}"\n\t"StateFlags"\t\t"4"\n\t"installdir"\t\t"Dir${id}"\n}\n`;
put(join(root, "steamapps", "appmanifest_7001.acf"), manifest(7001, "Fixture B Installed"));
put(join(root, "steamapps", "appmanifest_4001.acf"), manifest(4001, "Fixture A Installed"));
mkdirSync(join(root, "steamapps", "common", "Dir7001"), { recursive: true });
mkdirSync(join(root, "steamapps", "common", "Dir4001"), { recursive: true });
put(join(root, "appcache", "appinfo.vdf"), appinfoV29([
  { id: 1001, name: "Fixture Alpha", type: "Game" }, { id: 1002, name: "Fixture Bravo", type: "Game" },
  { id: 1004, name: "Fixture Editor Tool", type: "Tool" }, { id: 2001, name: "Fixture Collected", type: "game" },
  { id: 3001, name: "Fixture Cached", type: "Game" }, { id: 4001, name: "Fixture A Installed", type: "Game" },
  { id: 6001, name: "Fixture Demo", type: "Demo" }, { id: 7001, name: "Fixture B Installed", type: "Game" },
  { id: 7002, name: "Fixture B Played", type: "Game" }, { id: 8001, name: "Fixture B Collected", type: "Game" },
]));
const localconfig = (rec: [number, number, number][]) =>
  `"UserLocalConfigStore"\n{\n\t"Software"\n\t{\n\t\t"Valve"\n\t\t{\n\t\t\t"Steam"\n\t\t\t{\n\t\t\t\t"apps"\n\t\t\t\t{\n` +
  rec.map(([id, last, mins]) => `\t\t\t\t\t"${id}"\n\t\t\t\t\t{\n\t\t\t\t\t\t"LastPlayed"\t\t"${last}"\n\t\t\t\t\t\t"Playtime"\t\t"${mins}"\n\t\t\t\t\t}\n`).join("") +
  `\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n`;
const collection = (name: string, ids: number[]) => {
  const inner = JSON.stringify({ id: `uc-${name}`, name, added: ids, removed: [] });
  return ["user-collections.uc-" + name, { key: "user-collections.uc-" + name, timestamp: 1, value: inner, version: "1" }];
};
const acct = (n: number) => join(root, "userdata", String(n), "config");
// A: 4001 installed + played, 1001/1002 played, 1004 a tool, 1003-style stub, 6001 a demo; collected 2001; library cache 3001.
put(join(acct(ACCT_A), "localconfig.vdf"), localconfig([[1001, 1790000000, 120], [1002, 1700000000, 1], [1004, 1700000500, 30], [4001, 1700000100, 15], [6001, 1700000300, 5], [1003, 0, 0]]));
put(join(acct(ACCT_A), "cloudstorage", "cloud-storage-namespace-1.json"), JSON.stringify([collection("Fixture Favourites", [2001, 1001])]));
put(join(acct(ACCT_A), "librarycache", "3001.json"), "[[\"achievements\",{\"version\":2,\"data\":{}}]]");
// B: 7001 installed + played most recently, 1001 a little, 7002 played, collected 8001.
put(join(acct(ACCT_B), "localconfig.vdf"), localconfig([[1001, 1750000000, 5], [7001, 1780000000, 300], [7002, 1760000000, 60]]));
put(join(acct(ACCT_B), "cloudstorage", "cloud-storage-namespace-1.json"), JSON.stringify([collection("Fixture Bee Group", [8001])]));
mkdirSync(data, { recursive: true });
// Family Shared was dropped (decision D2): a leftover file from an older build must not add a title or a folder.
put(join(data, "family-shared.json"), JSON.stringify({ account: ID_A, apps: [{ id: 9001, title: "Fixture Shared A" }] }));

// ---- registry (scratch key only) ----
const reg = (...a: string[]) => spawnSync("reg", a, { encoding: "utf8" });
const setActive = (acctId: number) => reg("add", `${regPath}\\ActiveProcess`, "/v", "ActiveUser", "/t", "REG_DWORD", "/d", String(acctId), "/f");
const clearActive = () => reg("delete", `${regPath}\\ActiveProcess`, "/v", "ActiveUser", "/f");
const setAutoLogin = (name: string) => reg("add", regPath, "/v", "AutoLoginUser", "/t", "REG_SZ", "/d", name, "/f");
const realQuery = () => reg("query", "HKCU\\Software\\Valve\\Steam").stdout;
const realBefore = realQuery();

// ---- run ----
const dump = join(scratch, "entries.json");
const appdata = join(scratch, "appdata");
mkdirSync(appdata, { recursive: true });
const baseEnv: Record<string, string | undefined> = {
  ...process.env,
  XMB_STEAM_ROOT: root, XMB_STEAM_REGISTRY_KEY: regKey, XMB_STEAM_REGISTRY: "0", XMB_STEAM_HIDE: "0", XMB_STEAM_ARTWORK: "0",
  XMB_STEAM_SWITCH_NOPROC: "1", APPDATA: appdata, XMB_API_KEY_DIR: appdata, XMB_LOG: join(scratch, "app.log"), XMB_DUMP_ENTRIES: dump, STEAM_WEB_API_KEY: "",
};
delete baseEnv.QT_QPA_PLATFORM;
delete baseEnv.XMB_STEAM_ACTIVITY;

type Dump = any;
const readDump = (): Dump | undefined => { try { return JSON.parse(readFileSync(dump, "utf8")); } catch { return undefined; } };
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));
async function waitFor(pred: (d: Dump) => boolean, ms: number): Promise<Dump | undefined> {
  const end = Date.now() + ms;
  while (Date.now() < end) { const d = readDump(); if (d && pred(d)) return d; await sleep(150); }
  return undefined;
}
function start(timeoutMs = 120000) {
  const argv = ["--dev-flash", flash, "--data-dir", data, "--script", "pause=100000,quit"];
  if (existsSync(join(vsh, "custom_render_plugin.prx"))) argv.splice(2, 0, "--vsh-modules", vsh);
  const proc = spawn(exe, argv, { env: baseEnv as NodeJS.ProcessEnv, stdio: "ignore" });
  const killer = setTimeout(() => proc.kill(), timeoutMs);
  return { stop: () => { clearTimeout(killer); proc.kill(); } };
}
const steamIds = (d: Dump, category: string) => (d.categories.find((c: any) => c.id === category)?.entries ?? []).filter((e: any) => /^steam:\d+$/.test(e.id)).map((e: any) => e.id.slice(6));
const sorted = (xs: string[]) => [...xs].sort();
const same = (a: string[], b: string[]) => JSON.stringify(sorted(a)) === JSON.stringify(sorted(b));
const notInstalled = (d: Dump) => d.allTitles.filter((r: any) => /Not installed/.test(r.detail)).map((r: any) => r.id.replace("steam:", ""));

// ===== run 1: account A active, then a switch made "inside the Steam client" to B, then back =====
setActive(ACCT_A);
let app = start();
const a1 = await waitFor((d) => d.steamId === ID_A && d.categories, 60000);
expect(!!a1, "run 1: the library loads for account A (registry ActiveUser A)");
if (a1) {
  expect(a1.ownedSource === "keyless", `A: owned list source is keyless (${a1.ownedSource})`);
  expect(same(a1.ownedIds, ["1001", "1002", "2001", "3001", "4001"]), `A: keyless list is A's games only [${sorted(a1.ownedIds)}]`);
  expect(!a1.ownedIds.some((id: string) => ["7001", "7002", "8001", "1004", "6001", "1003"].includes(id)), "A: no B game, tool, demo or stub in the list");
  expect(a1.steamPersona === "Persona Aye", `A: persona (${a1.steamPersona})`);
  expect(same(a1.collectionNames, ["Fixture Favourites"]), `A: collections [${a1.collectionNames}]`);
  expect(same(a1.activityIds, ["1001", "1002", "1003", "1004", "4001", "6001"]), `A: play records are A's [${sorted(a1.activityIds)}]`);
  expect(same(notInstalled(a1), ["1001", "1002", "2001", "3001"]), `A: Not installed rows [${sorted(notInstalled(a1))}]`);
  expect(!a1.allTitles.some((r: any) => r.id === "steam:9001") && !a1.ownedIds.includes("9001"), "A: a leftover family-shared file adds no title (Family Shared was dropped)");
  expect(steamIds(a1, "game").join() === "4001,7001", `A: installed rows ordered by A's play records (${steamIds(a1, "game")})`);
  const row2001 = a1.allTitles.find((r: any) => r.id === "steam:2001");
  expect(String(row2001?.detail).startsWith("Fixture Favourites"), `A: collection label on a collected row (${row2001?.detail})`);
}
const stamp = (d?: Dump) => JSON.stringify(d);
const before = stamp(readDump());
console.log("stand-in: the Steam client switches to account B (registry ActiveUser only)");
setActive(ACCT_B);
const b1 = await waitFor((d) => d.steamId === ID_B && stamp(d) !== before, 30000);
expect(!!b1, "run 1: the library followed the registry ActiveUser change within the poll window, with nothing armed");
if (b1) {
  expect(b1.ownedSource === "keyless", "B: owned list source is keyless");
  expect(same(b1.ownedIds, ["1001", "7001", "7002", "8001"]), `B: keyless list is B's games only [${sorted(b1.ownedIds)}]`);
  expect(!b1.ownedIds.some((id: string) => ["2001", "3001", "4001", "1002"].includes(id)), "B: no row of account A remains");
  expect(b1.steamPersona === "Persona Bee", `B: persona (${b1.steamPersona})`);
  expect(same(b1.collectionNames, ["Fixture Bee Group"]), `B: collections swapped [${b1.collectionNames}]`);
  expect(same(b1.activityIds, ["1001", "7001", "7002"]), `B: play records swapped [${sorted(b1.activityIds)}]`);
  expect(!b1.allTitles.some((r: any) => r.id === "steam:9001"), "B: no family-shared title either");
  expect(same(notInstalled(b1), ["1001", "7002", "8001"]), `B: Not installed rows [${sorted(notInstalled(b1))}]`);
  expect(steamIds(b1, "game").join() === "7001,4001", `B: installed rows re-ordered by B's play records (${steamIds(b1, "game")})`);
  const row8001 = b1.allTitles.find((r: any) => r.id === "steam:8001");
  expect(String(row8001?.detail).startsWith("Fixture Bee Group"), `B: collection label (${row8001?.detail})`);
}
const beforeBack = stamp(readDump());
console.log("stand-in: the Steam client switches back to account A");
setActive(ACCT_A);
const a2 = await waitFor((d) => d.steamId === ID_A && stamp(d) !== beforeBack, 30000);
expect(!!a2, "switching back is followed too");
if (a2 && a1) expect(same(a2.ownedIds, a1.ownedIds) && same(a2.collectionNames, a1.collectionNames) && same(a2.activityIds, a1.activityIds), "back on A: everything matches the first load");
app.stop();
await sleep(1500);

// ===== run 2: restart with a signed-out client; AutoLoginUser decides =====
clearActive();
setAutoLogin("fixture_b");
rmSync(dump, { force: true });
app = start();
const b2 = await waitFor((d) => d.categories, 60000);
expect(!!b2 && b2.steamId === ID_B, `restart, signed out, AutoLoginUser=fixture_b: account B (${b2?.steamId?.slice(-1)}) although A has the newer login`);
if (b2) expect(same(b2.ownedIds, ["1001", "7001", "7002", "8001"]), "restart: B's list from the first scan, nothing of A");
app.stop();
await sleep(1500);
setAutoLogin("fixture_a");
rmSync(dump, { force: true });
app = start();
const a3 = await waitFor((d) => d.categories, 60000);
expect(!!a3 && a3.steamId === ID_A, `restart, AutoLoginUser=fixture_a: account A (${a3?.steamId?.slice(-1)})`);
app.stop();

expect(realQuery() === realBefore, "the real HKCU\\Software\\Valve\\Steam key is unchanged");
reg("delete", regPath, "/f");
if (!keep) rmSync(scratch, { recursive: true, force: true }); else console.log("kept scratch:", scratch);
console.log(problems.length ? `\nFAILED ${problems.length} check(s)` : "\nsteam-accounts e2e: all checks passed");
process.exit(problems.length ? 1 : 0);
