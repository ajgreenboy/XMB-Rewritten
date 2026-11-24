// Scripted native run of the BD remote's quick taps and held keys (audit/BD-REMOTE.md, "Presses").
//
//   bun work/bd-presses-e2e.ts --exe C:/dev/out/bd-presses/re-ps3-native.exe [--out C:/dev/out/bd-presses-e2e]
//
// Two token forms are used:
//   BDREMOTE=<hex>        one report through View::replayBdReport (the parser, mapper and command dispatch; its own edge
//                         detection, no key repeat). Press then release with NO wait between: each must still act.
//   BDLIVE=<hex>+<hex>    reports into the LIVE press path (pad.cpp's remote latch, the one the HID reader thread feeds),
//                         joined with '+' so press and release land in the same script tick, i.e. between two 16 ms polls.
//                         A hold is a BDLIVE press, a pause, and a BDLIVE release: the remote sends no repeat report.
// A streaming DualSense cannot be added here: a scripted run gates every physical pad. That the remote does not share
// `newest` with the pads is structural (pad.cpp writes only the remote latch for the remote) and is covered by the
// PadLatch unit tests; the real-pad half is on the "to verify on a real remote" list.
import { spawn } from "node:child_process";
import { existsSync, mkdirSync, readFileSync, rmSync } from "node:fs";
import { join } from "node:path";

const args = process.argv.slice(2);
const opt = (name: string, dflt = "") => {
  const i = args.indexOf(`--${name}`);
  return i >= 0 && args[i + 1] ? args[i + 1] : dflt;
};
const exe = opt("exe");
const out = opt("out", "C:/dev/out/bd-presses-e2e");
const flash = opt("flash", "C:/dev/rpcs3/bin/dev_flash");
const vsh = opt("vsh-modules", "C:/dev/xmb/fw493/vsh_module");
if (!exe || !existsSync(exe)) { console.error("pass --exe <re-ps3-native.exe>"); process.exit(2); }
const problems: string[] = [];
const expect = (ok: boolean, what: string) => { console.log(`${ok ? "ok  " : "FAIL"} ${what}`); if (!ok) problems.push(what); };

// 12-byte reports: id 01, joypad bits (bytes 1-3), key code (byte 4), ff filler, press marker, battery 5.
const rep = (code: number, joypad = 0) => {
  const b = [0x01, joypad & 0xff, (joypad >> 8) & 0xff, (joypad >> 16) & 0xff, code, 0xff, 0xff, 0xff, 0xff, 0xff, code === 0xff && !joypad ? 0 : 1, 5];
  return b.map((x) => x.toString(16).padStart(2, "0")).join("");
};
const RELEASE = rep(0xff);
const UP = rep(0x54, 0x10), RIGHT = rep(0x55, 0x20), DOWN = rep(0x56, 0x40), ENTER = rep(0x0b, 0x080000);

mkdirSync(out, { recursive: true });
let n = 0;
async function run(tag: string, tokens: string[]) {
  // every "grab=<name>" in `tokens` is expanded to a state sidecar under out/tag-name.png(.json)
  const data = join(out, `data-${tag}`);
  rmSync(data, { recursive: true, force: true }); mkdirSync(data, { recursive: true });
  const script = tokens.map((t) => t.startsWith("grab=") ? `grab=${join(out, `${tag}-${t.slice(5)}.png`)}` : t).join(",");
  const argv = ["--dev-flash", flash, "--data-dir", data, "--script", `pause=2500,${script},quit`];
  if (existsSync(join(vsh, "custom_render_plugin.prx"))) argv.splice(2, 0, "--vsh-modules", vsh);
  const env = { ...process.env, XMB_NO_SETUP_ROWS: "1", XMB_SCRIPT_INTERVAL_MS: "10", XMB_STEAM_REGISTRY: "0", XMB_STEAM_ARTWORK: "0" } as NodeJS.ProcessEnv;
  delete env.QT_QPA_PLATFORM;
  const proc = spawn(exe, argv, { env, stdio: "ignore" });
  const killer = setTimeout(() => proc.kill(), 120000);
  const code: number = await new Promise((res) => proc.on("exit", (c) => res(c ?? -1)));
  clearTimeout(killer);
  const state = (name: string) => {
    const f = join(out, `${tag}-${name}.png.json`);
    return existsSync(f) ? JSON.parse(readFileSync(f, "utf8")) : {};
  };
  n++;
  return { code, state };
}
const settle = ["pause=700"];

// 1. REPLAY path: back-to-back press+release (no wait between) of an arrow and of Enter.
{
  const r = await run("replay", [
    ...settle, "grab=a",
    `BDREMOTE=${RIGHT}`, `BDREMOTE=${RELEASE}`, ...settle, "grab=b",
    `BDREMOTE=${RIGHT}`, `BDREMOTE=${RELEASE}`, ...settle,
    `BDREMOTE=${DOWN}`, `BDREMOTE=${RELEASE}`, ...settle, "grab=c",
    `BDREMOTE=${ENTER}`, `BDREMOTE=${RELEASE}`, ...settle, "grab=d",
  ]);
  const a = r.state("a"), b = r.state("b"), c = r.state("c"), d = r.state("d");
  expect(r.code === 0, "replay run exits 0");
  console.log("replay:", [a, b, c, d].map((s) => `${s.category}/${s.selection}/depth ${s.depth}/${s.entryId}`).join("  ->  "));
  expect(b.categoryIndex !== a.categoryIndex, "replay: a Right press+release back-to-back moved the category");
  expect(c.selection !== b.selection, "replay: a Down press+release back-to-back moved the selection");
  expect(d.depth !== c.depth || d.pageIdentity !== c.pageIdentity || d.entryId !== c.entryId, "replay: an Enter press+release back-to-back acted");
}

// 2. LIVE path: the same taps, press and release in one tick (between two polls). Before this fix the release overwrote
//    the press before the shell sampled it.
{
  const r = await run("live-tap", [
    ...settle, "grab=a",
    `BDLIVE=${RIGHT}+${RELEASE}`, ...settle, "grab=b",
    `BDLIVE=${RIGHT}+${RELEASE}`, ...settle, "grab=c",
    `BDLIVE=${DOWN}+${RELEASE}`, ...settle, "grab=d",
    `BDLIVE=${ENTER}+${RELEASE}`, ...settle, "grab=e",
  ]);
  const [a, b, c, d, e] = ["a", "b", "c", "d", "e"].map((k) => r.state(k));
  expect(r.code === 0, "live-tap run exits 0");
  console.log("live-tap:", [a, b, c, d, e].map((s) => `${s.category}/${s.selection}/depth ${s.depth}/${s.entryId}`).join("  ->  "));
  expect(b.categoryIndex !== a.categoryIndex, "live: a Right tap (press+release in one tick) moved the category");
  expect(c.categoryIndex !== b.categoryIndex, "live: a second Right tap moved it again");
  expect(d.selection !== c.selection, "live: a Down tap moved the selection");
  expect(e.depth !== d.depth || e.pageIdentity !== d.pageIdentity || e.entryId !== d.entryId, "live: an Enter tap acted");
  expect(typeof e.padEventsIgnored === "number", "sidecar reports the pad gate (physical pads stay gated)");
}

// 3. LIVE hold: one press report, ~3 s with NO report, one release. Key repeat (300 ms, then 67 ms) must keep going.
{
  const r = await run("live-hold", [
    ...settle, "DOWN", ...settle, "UP", ...settle, "grab=a",
    `BDLIVE=${DOWN}`, "pause=3000", `BDLIVE=${RELEASE}`, ...settle, "grab=b",
    "pause=1500", "grab=c",
  ]);
  const a = r.state("a"), b = r.state("b"), c = r.state("c");
  expect(r.code === 0, "live-hold run exits 0");
  console.log(`live-hold: selection ${a.selection} -> ${b.selection} -> ${c.selection} (entryCount ${a.entryCount})`);
  expect(b.selection - a.selection >= 5 || b.selection === a.entryCount - 1, "a Down held 3 s with no further reports moved the selection several times (repeat kept going)");
  expect(c.selection === b.selection, "after the release report nothing keeps moving");
}

console.log(problems.length ? `\nFAILED ${problems.length} check(s)` : "\nbd-presses e2e: all checks passed");
process.exit(problems.length ? 1 : 0);
