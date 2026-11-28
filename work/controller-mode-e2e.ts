// Scripted native run of Controller mode's lifecycle (native-xmb/audit/CONTROLLER-MODE.md): the virtual pad exists only
// while a Steam title runs. Fake backend only (XMB_CONTROLLER_BACKEND=fake): no driver, no real Steam.
//
//   bun work/controller-mode-e2e.ts --exe C:/dev/out/controller-mode/re-ps3-native.exe \
//       --standin C:/dev/out/controller-mode-tests/xmb-steam-quit-standin.exe [--out C:/dev/out/controller-mode-e2e]
//
// Everything is synthetic: a generated Steam root with one installed title whose "game" is the Quit Game tests' stand-in
// (a Win32 window program that leaves by itself after N seconds), XMB_STEAM_FAKE=1 so steam.exe is never started, and a
// fake pad that appends "<epoch ms> create|update|destroy" lines to a file. The run asserts, from the file and from a
// poll of the process list: the pad is created before the stand-in exists, destroyed after it has gone, created once,
// and never created when the mode is off (kill switch, per-game override, driver missing).
import { spawn, spawnSync } from "node:child_process";
import { copyFileSync, existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { join } from "node:path";

const args = process.argv.slice(2);
const opt = (name: string, dflt = "") => {
  const i = args.indexOf(`--${name}`);
  return i >= 0 && args[i + 1] ? args[i + 1] : dflt;
};
const exe = opt("exe");
const standin = opt("standin");
const out = opt("out", "C:/dev/out/controller-mode-e2e");
const flash = opt("flash", "C:/dev/rpcs3/bin/dev_flash");
const vsh = opt("vsh-modules", "C:/dev/xmb/fw493/vsh_module");
if (!exe || !existsSync(exe)) { console.error("pass --exe <re-ps3-native.exe>"); process.exit(2); }
if (!standin || !existsSync(standin)) { console.error("pass --standin <xmb-steam-quit-standin.exe>"); process.exit(2); }

const problems: string[] = [];
const expect = (ok: boolean, what: string) => { console.log(`${ok ? "ok  " : "FAIL"} ${what}`); if (!ok) problems.push(what); };
const put = (path: string, content: string | Buffer) => { mkdirSync(join(path, ".."), { recursive: true }); writeFileSync(path, content); };

const APP_ID = "424242";
const GAME_EXE = "cm-standin-game.exe";   // unique image name, so the poll never matches another program
const base = join(out, "fixture");
rmSync(out, { recursive: true, force: true });
mkdirSync(out, { recursive: true });

function fixture(name: string, overlay?: object) {
  const root = join(base, name, "steam"), data = join(base, name, "data");
  mkdirSync(data, { recursive: true });
  put(join(root, "steam.exe"), "stand-in\n");   // never run: XMB_STEAM_FAKE=1 replaces it
  put(join(root, "steamapps", `appmanifest_${APP_ID}.acf`),
    `"AppState"\n{\n\t"appid"\t\t"${APP_ID}"\n\t"Universe"\t\t"1"\n\t"name"\t\t"Controller Stand-in"\n\t"StateFlags"\t\t"4"\n\t"installdir"\t\t"StandinGame"\n\t"SizeOnDisk"\t\t"1000"\n}\n`);
  mkdirSync(join(root, "steamapps", "common", "StandinGame"), { recursive: true });
  copyFileSync(standin, join(root, "steamapps", "common", "StandinGame", GAME_EXE));
  if (overlay) put(join(data, "steam-collections-local.json"), JSON.stringify({ version: 1, ...overlay }, null, 2));
  return { root, data };
}

type Result = { events: { t: number; what: string }[]; seen: { first: number; last: number } | null; log: string };

async function run(tag: string, env: Record<string, string>, overlay?: object): Promise<Result> {
  const { root, data } = fixture(tag, overlay);
  const fakeLog = join(out, `${tag}-pad.log`);
  const xmbLog = join(out, `${tag}-xmb.log`);
  const standinSeconds = 6;
  const script = ["full", "pause=5000", "ENTER", "pause=16000", "quit"];
  const argv = ["--dev-flash", flash, "--data-dir", data, "--no-intro", "--steam-root", root, "--script", script.join(",")];
  if (existsSync(join(vsh, "custom_render_plugin.prx"))) argv.splice(2, 0, "--vsh-modules", vsh);
  const childEnv: Record<string, string | undefined> = {
    ...process.env,
    XMB_SINGLE_INSTANCE: "0", XMB_STEAM_ROOT: root, XMB_STEAM_REGISTRY: "0", XMB_STEAM_HIDE: "0", XMB_STEAM_ARTWORK: "0",
    XMB_STEAM_FAKE: "1", XMB_STEAM_FAKE_EXE: GAME_EXE, XMB_STEAM_FAKE_ARGS: `--max-seconds ${standinSeconds}`,
    XMB_NO_SETUP_ROWS: "1", XMB_NO_GAMEBOOT: "1", XMB_LAUNCH_COVER: "0", STEAM_WEB_API_KEY: "",
    XMB_CONTROLLER_BACKEND: "fake", XMB_CONTROLLER_FAKE_LOG: fakeLog, XMB_CONTROLLER_DRIVER: "present",
    XMB_LOG: xmbLog, ...env,
  };
  delete childEnv.QT_QPA_PLATFORM; delete childEnv.QT_PLUGIN_PATH;
  const proc = spawn(exe, argv, { env: childEnv as NodeJS.ProcessEnv, stdio: "ignore" });
  const killer = setTimeout(() => proc.kill(), 90000);
  // Poll the process list for the stand-in game while the XMB runs.
  let first = 0, last = 0, alive = true;
  const poll = (async () => {
    while (alive) {
      const r = spawnSync("tasklist", ["/FI", `IMAGENAME eq ${GAME_EXE}`, "/NH"], { encoding: "utf8" });
      if (r.stdout && r.stdout.includes(GAME_EXE)) { const now = Date.now(); if (!first) first = now; last = now; }
      await new Promise((res) => setTimeout(res, 60));
    }
  })();
  await new Promise<number>((res) => proc.on("exit", (c) => res(c ?? -1)));
  alive = false; await poll; clearTimeout(killer);
  spawnSync("taskkill", ["/IM", GAME_EXE, "/F"], { stdio: "ignore" });
  const events = existsSync(fakeLog)
    ? readFileSync(fakeLog, "utf8").split(/\r?\n/).filter(Boolean).map((l) => { const i = l.indexOf(" "); return { t: Number(l.slice(0, i)), what: l.slice(i + 1) }; })
    : [];
  return { events, seen: first ? { first, last } : null, log: existsSync(xmbLog) ? readFileSync(xmbLog, "utf8") : "" };
}

const count = (r: Result, word: string) => r.events.filter((e) => e.what === word).length;
const launched = (r: Result) => r.seen !== null;

// 1. Default: on, driver present (forced), fake backend.
const on = await run("on", {});
console.log("on events:", on.events.map((e) => e.what).filter((w) => !w.startsWith("update")).join(", "), `| stand-in seen ${on.seen ? on.seen.last - on.seen.first : 0} ms`);
expect(launched(on), "the stand-in title was launched through the XMB (Steam fake)");
expect(count(on, "create") === 1, "exactly one virtual pad was created");
expect(count(on, "destroy") === 1, "exactly one virtual pad was destroyed");
if (on.seen && count(on, "create") && count(on, "destroy")) {
  const created = on.events.find((e) => e.what === "create")!.t, destroyed = on.events.find((e) => e.what === "destroy")!.t;
  expect(created <= on.seen.first, `the pad was created before the game existed (${on.seen.first - created} ms ahead)`);
  expect(destroyed >= on.seen.last, `the pad was destroyed only after the game was gone (${destroyed - on.seen.last} ms after the last sighting)`);
  expect(destroyed - on.seen.last < 8000, "and promptly after the title ended");
  expect(on.events[0].what === "create" && on.events[on.events.length - 1].what === "destroy", "create is the first event and destroy the last");
}
expect(/controller: virtual pad removed/.test(on.log) && /controller: launch app=424242 backend=fake enabled=1 status=Active pad=1 started=1/.test(on.log), "the log says the pad was Active and then removed");
expect(/returned from Controller Stand-in/.test(on.log), "the XMB saw the title end");

// 2. Kill switch: no pad ever, the game still launches.
const killed = await run("killswitch", { XMB_CONTROLLER_MODE: "0" });
expect(launched(killed) && killed.events.length === 0, "XMB_CONTROLLER_MODE=0: the game launches and no pad is ever created");
expect(/status=Off pad=0 started=1/.test(killed.log), "the log says Off");

// 3. Per-game override Off.
const off = await run("override-off", {}, { controllerMode: { [APP_ID]: "off" } });
expect(launched(off) && off.events.length === 0, "per-game override Off: the game launches and no pad is created");

// 4. Driver missing: no pad, the launch is not blocked, the reason is logged.
const missing = await run("driver-missing", { XMB_CONTROLLER_DRIVER: "missing" });
expect(launched(missing) && missing.events.length === 0, "driver missing: the game launches and no pad is created");
expect(/status=Driver missing/.test(missing.log), "the log says Driver missing");

// 5. A failed create never blocks the launch (the fake is made to fail).
const failing = await run("create-fails", { XMB_CONTROLLER_FAKE_FAIL: "access denied" });
expect(launched(failing), "a failed create does not block the launch");
expect(/status=Error: access denied pad=0 started=1/.test(failing.log), "the log says Error with the reason");


// 6. The REAL ViGEm backend where the driver is not installed (this dev PC): the service query is forced to "present" so the
// launch reaches the backend, which must fail truthfully and never block the launch. Skipped when ViGEmBus is installed here.
const hasDriver = spawnSync("sc", ["query", "ViGEmBus"], { encoding: "utf8" }).status === 0;
if (hasDriver) console.log("skip real-backend failure check: ViGEmBus is installed on this machine");
else {
  const real = await run("real-backend-no-driver", { XMB_CONTROLLER_BACKEND: "vigem" });
  expect(launched(real), "real backend, driver absent: the game still launches");
  expect(/backend=vigem enabled=1 status=Error: The ViGEmBus driver is not installed or not running\. pad=0 started=1/.test(real.log), "real backend, driver absent: Error with the real reason, no pad");
}
console.log(problems.length ? `\nFAILED ${problems.length} check(s)` : "\ncontroller-mode e2e: all checks passed");
process.exit(problems.length ? 1 : 0);
