// Scripted native run of Settings > Network Settings > API Keys with a FAKE key (32 zeros).
//
//   bun work/api-keys-e2e.ts --exe C:/dev/out/api-keys/re-ps3-native.exe [--out C:/dev/out/api-keys/e2e]
//
// Offline and synthetic: XMB_API_KEY_DIR and --data-dir point at a scratch folder under --out, so no real
// application data is read or written; STEAM_WEB_API_KEY is emptied and the Steam root is a folder that does not exist
// (so the owned-games refresh stops at "No Steam account was found" and never contacts Valve). No real key is used.
// Screenshots land in --out: 01-list, 02-steam-notset, 03-saved-message, 04-steam-set, 05-badformat, 06-clear-confirm,
// 07-steam-cleared, 08-env.
import { spawn } from "node:child_process";
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { join } from "node:path";

const args = process.argv.slice(2);
const opt = (name: string, dflt = "") => {
  const i = args.indexOf(`--${name}`);
  return i >= 0 && args[i + 1] ? args[i + 1] : dflt;
};
const exe = opt("exe");
const out = opt("out", "C:/dev/out/api-keys/e2e");
const flash = opt("flash", "C:/dev/rpcs3/bin/dev_flash");
const vsh = opt("vsh-modules", "C:/dev/xmb/fw493/vsh_module");
if (!exe || !existsSync(exe)) { console.error("pass --exe <re-ps3-native.exe>"); process.exit(2); }

const problems: string[] = [];
const expect = (ok: boolean, what: string) => { console.log(`${ok ? "ok  " : "FAIL"} ${what}`); if (!ok) problems.push(what); };
const FAKE = "0".repeat(32);

const base = join(out, "fixture");
rmSync(base, { recursive: true, force: true });
mkdirSync(out, { recursive: true });


async function run(sub: string, extra: Record<string, string>, moves: string[]) {
  const dir = join(base, sub), keys = join(dir, "keys"), data = join(dir, "data");
  mkdirSync(keys, { recursive: true }); mkdirSync(data, { recursive: true });
  const log = join(dir, "xmb.log");
  const argv = ["--dev-flash", flash, "--data-dir", data, "--script", ["wait", ...moves, "quit"].join(",")];
  if (existsSync(join(vsh, "custom_render_plugin.prx"))) argv.splice(2, 0, "--vsh-modules", vsh);
  const env = {
    ...process.env, XMB_API_KEY_DIR: keys, STEAM_WEB_API_KEY: "", RAWG_API_KEY: "", SPOTIFY_CLIENT_ID: "", XMB_LOG: log,
    XMB_STEAM_ROOT: join(dir, "no-steam"), XMB_STEAM_REGISTRY: "0", XMB_NO_SETUP_ROWS: "1", ...extra,
  } as NodeJS.ProcessEnv;
  delete env.QT_QPA_PLATFORM;
  const proc = spawn(exe, argv, { env, stdio: "ignore" });
  const killer = setTimeout(() => proc.kill(), 120000);
  const code: number = await new Promise((res) => proc.on("exit", (c) => res(c ?? -1)));
  clearTimeout(killer);
  return { code, keys, data, log: existsSync(log) ? readFileSync(log, "utf8") : "" };
}
const grab = (name: string) => `grab=${join(out, name + ".png")}`;
const keyFile = (keys: string) => join(keys, "steam_api_key");

// 1. Nothing set, then enter the fake key through the OSK.
const goKeys = ["LEFT", "LEFT", "LEFT", "LEFT", "LEFT", "pause=700", ...Array(15).fill("DOWN"), "pause=500", "ENTER", "pause=900", ...Array(8).fill("DOWN"), "pause=300"];
const a = await run("save", {}, [
  ...goKeys, grab("01-list"),
  "ENTER", "pause=900", grab("02-list"), "ENTER", "pause=900", grab("02-steam-notset"),
  "DOWN", "ENTER", "pause=900", `type=${FAKE}`, "pause=500", grab("02b-osk"), "OSKENTER", "pause=1500", grab("03-saved-message"),
  "ENTER", "pause=700", grab("04-steam-set"),
]);
expect(a.code === 0, "save run exits 0");
expect(existsSync(keyFile(a.keys)) && readFileSync(keyFile(a.keys), "utf8") === FAKE, "the fake key is saved exactly, one line, no newline");
expect(!a.log.includes(FAKE), "the log never contains the key");
expect(/apikeys: steam saved/.test(a.log), "the log records the save without the value");


// 2. A wrong format is refused truthfully and nothing is written.
const b = await run("badformat", {}, [
  ...goKeys, "ENTER", "pause=900", "ENTER", "pause=900", "DOWN", "ENTER", "pause=900", "type=abc123", "pause=300", "OSKENTER", "pause=1200", grab("05-badformat"),
]);
expect(b.code === 0, "bad-format run exits 0");
expect(!existsSync(keyFile(b.keys)), "a key of the wrong format writes no file");
expect(/apikeys: steam refused/.test(b.log), "the log records the refusal (length only)");

// 3. Clear Key asks first, then removes only the saved file.
const prepare = (sub: string, key: string) => { mkdirSync(join(base, sub, "keys"), { recursive: true }); writeFileSync(join(base, sub, "keys", "steam_api_key"), key); };
prepare("clear", FAKE);
const c = await run("clear", {}, [
  ...goKeys, "ENTER", "pause=900", "ENTER", "pause=900", grab("06a-steam-saved"), "DOWN", "DOWN", "ENTER", "pause=900", grab("06-clear-confirm"),
  "LEFT", "pause=300", "ENTER", "pause=1200", grab("06b-cleared-message"), "ENTER", "pause=700", grab("07-steam-cleared"),
]);
expect(c.code === 0, "clear run exits 0");
expect(!existsSync(keyFile(c.keys)), "Clear Key (after confirming) removes the saved file");
expect(/apikeys: steam cleared/.test(c.log), "the log records the clear");

// 4. The environment wins: the row says so and Clear only removes the file.
prepare("env", FAKE);
const e = await run("env", { STEAM_WEB_API_KEY: FAKE }, [
  ...goKeys, grab("08a-list-env"), "ENTER", "pause=900", "ENTER", "pause=900", grab("08-env"),
]);
expect(e.code === 0, "environment run exits 0");
expect(existsSync(keyFile(e.keys)), "opening the page does not touch the saved file");
for (const r of [a, b, c, e]) expect(!r.log.includes(FAKE), "no run's log contains the key");
// 5. Spotify: the Client ID (public, plain-text editor) goes into spotify.json in the data folder; no secret exists.
const FAKE_ID = "1".repeat(32);
const sp = await run("spotify", {}, [
  ...goKeys, "ENTER", "pause=900", "DOWN", "DOWN", "pause=300", grab("09-list-spotify"), "ENTER", "pause=900", grab("10-spotify-notset"),
  "DOWN", "ENTER", "pause=900", `type=${FAKE_ID}`, "pause=400", grab("10b-spotify-osk"), "OSKENTER", "pause=1500", grab("11-spotify-saved"), "ENTER", "pause=700", grab("12-spotify-set"),
]);
expect(sp.code === 0, "spotify run exits 0");
const spFile = join(sp.data, "spotify.json");
expect(existsSync(spFile) && JSON.parse(readFileSync(spFile, "utf8")).clientId === FAKE_ID, "the Client ID is saved as clientId in spotify.json (the reader's own file)");
expect(!sp.log.includes(FAKE_ID), "the log never contains the Client ID");
console.log(`screenshots: ${out}`);
console.log(problems.length ? `\nFAILED ${problems.length} check(s)` : "\napi-keys e2e: all checks passed");
process.exit(problems.length ? 1 : 0);
