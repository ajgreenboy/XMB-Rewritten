import { readFileSync } from "node:fs";

const dir = process.argv[2];
if (!dir) throw new Error("usage: bun work/verify-imported-stale-art.ts <capture-dir>");
const state = (name: string) => JSON.parse(readFileSync(`${dir}\\${name}.png.json`, "utf8"));
const first = state("01-first");
const before = state("02-blue-before-old");
const after = state("03-after-old");
const third = state("04-third-reopen");
const row = (value: any) => value.entryRows?.[0];
const requests = readFileSync(`${dir}\\requests.log`, "utf8").trim().split(/\r?\n/).filter(Boolean);
const checks: [string, boolean, string][] = [
  ["first page is imported release source", first.entryId.startsWith("release:item:") && first.depth === 2, `${first.entryId} depth=${first.depth}`],
  ["reopen creates a distinct page identity", first.pageIdentity !== before.pageIdentity, `${first.pageIdentity} -> ${before.pageIdentity}`],
  ["delayed red request is still pending on first page", row(first).artworkReady === false && row(first).artworkUrl.endsWith("/red.png"), JSON.stringify(row(first))],
  ["new page resolves blue before old response", row(before).artworkReady === true && row(before).artworkRgb === "20,40,220" && row(before).artworkUrl.endsWith("/blue.png"), JSON.stringify(row(before))],
  ["late red response cannot replace blue art", row(after).artworkReady === true && row(after).artworkRgb === "20,40,220" && row(after).artworkUrl.endsWith("/blue.png"), JSON.stringify(row(after))],
  ["third reopen retains cached blue art", third.pageIdentity !== after.pageIdentity && row(third).artworkReady === true && row(third).artworkRgb === "20,40,220" && row(third).artworkUrl.endsWith("/blue.png"), JSON.stringify(row(third))],
  ["server observed red then one blue resource request", requests.filter((value) => value.endsWith("/red.png")).length === 1 && requests.filter((value) => value.endsWith("/blue.png")).length === 1 && requests.indexOf("/red.png") < requests.indexOf("/blue.png"), requests.join(",")],
];
let failed = 0;
for (const [name, ok, detail] of checks) { console.log(`${ok ? "PASS" : "FAIL"} ${name} ${detail}`); if (!ok) failed++; }
console.log(`Result: ${failed ? "FAIL" : "PASS"} (${checks.length - failed} passed, ${failed} failed)`);
process.exit(failed ? 1 : 0);
