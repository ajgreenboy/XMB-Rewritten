// Native Download Games capture gate. The filename is retained for existing CI callers;
// this gate no longer asserts the removed legacy Store surface or any visual baseline.
import { existsSync, readFileSync, writeFileSync } from "node:fs";

const dir = process.argv[2] ?? "C:\\dev\\out\\native-hydra-xmb-e2e";
const files = ["run/01-hub.png", "run/02-popular.png", "run/03-sources.png", "run/04-source-list.png", "run/05-feed.png", "run/06-detail.png", "run/07-source-sizes.png", "run/08-search.png"];
let pass = 0, fail = 0;
const report: string[] = ["# Native Download Games capture gate", `Capture dir: ${dir}`, ""];
const check = (name: string, ok: boolean, detail: string) => { report.push(`${ok ? "PASS" : "FAIL"} ${name} ${detail}`); ok ? pass++ : fail++; };
for (const file of files) {
  const png = `${dir}\\${file.replaceAll("/", "\\")}`, sidecar = `${png}.json`;
  check(`${file} PNG`, existsSync(png), png);
  check(`${file} sidecar`, existsSync(sidecar), sidecar);
  if (!existsSync(sidecar)) continue;
  try {
    const state = JSON.parse(readFileSync(sidecar, "utf8"));
    check(`${file} native category`, state.category === "game", `category=${state.category}`);
    check(`${file} has native state`, typeof state.pageKind === "string" && !String(state.pageKind).startsWith("legacy"), `pageKind=${state.pageKind}`);
    check(`${file} dimensions`, state.width > 0 && state.height > 0, `${state.width}x${state.height}`);
    check(`${file} entry evidence`, typeof state.entryId === "string" && typeof state.entryTitle === "string", `${state.entryId} / ${state.entryTitle}`);
  } catch (error) { check(`${file} sidecar parses`, false, String(error)); }
}
report.push("", `## Result: ${fail === 0 ? "PASS" : "FAIL"} (${pass} passed, ${fail} failed)`, "");
writeFileSync(`${dir}\\geometry.md`, report.join("\n"));
writeFileSync(`${dir}\\geometry.json`, JSON.stringify({ gate: "native-download-games", pass, fail, files }, null, 2));
console.log(report.join("\n"));
process.exit(fail ? 1 : 0);
