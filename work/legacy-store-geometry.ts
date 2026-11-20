// Native Download Games capture gate. The filename remains for existing callers, but this is
// now a route/content gate for the ordinary native list pages, not a legacy visual baseline.
import { existsSync, readFileSync, writeFileSync } from "node:fs";
import { decodePng } from "./ab-compare.ts";

const dir = process.argv[2] ?? "C:\\dev\\out\\native-hydra-xmb-e2e";
const expected = [
  ["run/01-hub.png", "game:search", "Search Games", 3],
  ["run/02-popular.png", "game:result:0", "Fixture Game", 1],
  ["run/03-sources.png", "hydra:source:http://127.0.0.1:8766/source-feed", "Fixture Source", 1],
  ["run/04-source-list.png", "game:result:0", "Fixture Game Deluxe", 1],
  ["run/05-feed.png", "game:detail:download", "Download", 7],
  ["run/06-detail.png", "game:source:0", "Fixture Source", 3],
  ["run/07-source-sizes.png", "game:confirm:download", "Download", 1],
  ["run/08-search.png", "game:result:0", "Fixture Game", 1],
] as const;
let pass = 0, fail = 0;
const report: string[] = ["# Native Download Games capture gate", `Capture dir: ${dir}`, ""];
const check = (name: string, ok: boolean, detail: string) => { report.push(`${ok ? "PASS" : "FAIL"} ${name} ${detail}`); ok ? pass++ : fail++; };
const pathOf = (relative: string) => `${dir}\\${relative.replaceAll("/", "\\")}`;
for (const [relative, id, title, count] of expected) {
  const png = pathOf(relative), sidecar = `${png}.json`;
  check(`${relative} PNG`, existsSync(png), png); check(`${relative} sidecar`, existsSync(sidecar), sidecar);
  if (!existsSync(sidecar) || !existsSync(png)) continue;
  try {
    const state = JSON.parse(readFileSync(sidecar, "utf8"));
    check(`${relative} route`, state.category === "game" && state.entryId === id && state.entryTitle === title,
      `${state.category}/${state.entryId}/${state.entryTitle}`);
    check(`${relative} row count`, state.entryCount === count, `expected ${count}, got ${state.entryCount}`);
    check(`${relative} native page`, state.pageKind === "page" && state.width === 960 && state.height === 540, `${state.pageKind} ${state.width}x${state.height}`);
    const rows = Array.isArray(state.entryRows) ? state.entryRows : [];
    check(`${relative} bounded row observability`, rows.length === Math.min(count, 128), `${rows.length} rows`);
    if (relative === "run/01-hub.png") check(`${relative} hub ordering`, rows.map((r: any) => r.id).join(",") === "game:search,game:browse,game:sources", rows.map((r: any) => r.id).join(","));
    if (relative === "run/06-detail.png") {
      check(`${relative} source names`, rows.map((r: any) => r.title).join(",") === "Fixture Source,Fixture Source,Fixture Source", rows.map((r: any) => r.title).join(","));
      check(`${relative} source sizes`, rows.map((r: any) => r.fieldValue).join(",") === "1 GB,2 GB,3 GB", rows.map((r: any) => r.fieldValue).join(","));
      check(`${relative} unsupported notice`, rows[2]?.detail?.includes("no supported") === true, String(rows[2]?.detail));
    }
    if (relative === "run/05-feed.png") check(`${relative} detail summary`, rows[0]?.title === "Download" && rows.some((r: any) => r.title === "Release Year"), "Download + metadata rows");
    const image = decodePng(png); let ink = 0;
    for (let y = 120; y < Math.min(image.h, 520); y += 2) for (let x = 260; x < Math.min(image.w, 920); x += 2) {
      const i = (y * image.w + x) * 4; if (0.299 * image.rgba[i] + 0.587 * image.rgba[i + 1] + 0.114 * image.rgba[i + 2] > 42) ink++;
    }
    check(`${relative} rendered native-list evidence`, ink > 500, `${ink} sampled lit pixels in list region`);
  } catch (error) { check(`${relative} parses`, false, String(error)); }
}
report.push("", `## Result: ${fail === 0 ? "PASS" : "FAIL"} (${pass} passed, ${fail} failed)`, "");
writeFileSync(`${dir}\\geometry.md`, report.join("\n")); writeFileSync(`${dir}\\geometry.json`, JSON.stringify({ gate: "native-download-games", pass, fail, expected }, null, 2));
console.log(report.join("\n")); process.exit(fail ? 1 : 0);
