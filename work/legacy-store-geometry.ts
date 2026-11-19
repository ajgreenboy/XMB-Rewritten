// legacy-store-geometry.ts - analytical pixel-geometry GATE for the legacy PlayStation Store
// visual-polish pass. It decodes the offline fixture captures and ASSERTS the regions the
// vision-capable orchestrator flagged against the 2008 period references. It exits nonzero when a
// capture is missing, its sidecar disagrees with the expected page state, or any geometry/colour
// assertion fails, so `work/legacy-store-capture.ps1` can treat it as a real gate.
//
//   bun work/legacy-store-geometry.ts [capture-dir]
//
// Capture dir defaults to C:\dev\out\legacy-store-visual-polish. The captures are 960x540 windows
// of the 1920x1080 logical canvas (capture = canvas / 2). Targets below are authored-approximation
// targets in that capture space (the reference 1024x576 scaled x0.9375), NOT pixel-perfect claims:
// every check carries an explicit tolerance and the wording below is deliberate about that.
import { readFileSync, existsSync, writeFileSync } from "node:fs";
import { decodePng, type PngImage } from "./ab-compare.ts";

const dir = process.argv[2] ?? "C:\\dev\\out\\legacy-store-visual-polish";

type Rect = { x0: number; x1: number; y0: number; y1: number };

// The four fixtures this pass owns, with the page state each `grab=` sidecar must report and the
// content mode its frame draws (catalogue = featured panel + 5-column grid; detail = dark inset).
const EXPECTED = [
  { file: "01-store-selected.png", pageKind: "legacyStore", selection: 0, focusedRow: "game:result:0", mode: "catalogue" },
  { file: "02-store-grid.png", pageKind: "legacyStore", selection: 1, focusedRow: "game:result:1", mode: "catalogue" },
  { file: "03-detail.png", pageKind: "legacyDetail", selection: 0, focusedRow: "game:detail", mode: "detail" },
  { file: "04-search-focus.png", pageKind: "legacyStore", selection: 1, focusedRow: "legacy:search", mode: "catalogue" },
] as const;

// Authored geometry targets (capture space). Tolerances are deliberate: the renderer geometry is
// authored against the period references, not traced, so these bounds are "close to the reference",
// never "pixel-perfect".
const FOOTER_SILVER = { y0: 465, y1: 507 };   // silver action strip
const FOOTER_TOL = 5;                          // px either side
const BAG = { x0: 147, x1: 191, y0: 38, y1: 82 };  // the firmware bag glyph's drawn rect

function px(img: PngImage, x: number, y: number): [number, number, number, number] {
  const i = (Math.floor(y) * img.w + Math.floor(x)) * 4;
  return [img.rgba[i], img.rgba[i + 1], img.rgba[i + 2], img.rgba[i + 3]];
}
function luma(c: readonly [number, number, number, number?]): number {
  return 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2];
}
function mean(img: PngImage, r: Rect): [number, number, number] {
  let rs = 0, gs = 0, bs = 0, n = 0;
  for (let y = Math.max(0, Math.floor(r.y0)); y < Math.min(img.h, Math.ceil(r.y1)); y++)
    for (let x = Math.max(0, Math.floor(r.x0)); x < Math.min(img.w, Math.ceil(r.x1)); x++) {
      const c = px(img, x, y); rs += c[0]; gs += c[1]; bs += c[2]; n++;
    }
  return n ? [Math.round(rs / n), Math.round(gs / n), Math.round(bs / n)] : [0, 0, 0];
}
// The footer is full-width, so a single column away from the panels/grid (x=900) separates the
// three bands cleanly: dark navy ground -> bright silver strip -> dark navy policy bar.
function detectFooter(img: PngImage): { silverTop: number; silverBottom: number } {
  const x = 900;
  let silverBottom = -1;
  for (let y = 539; y >= 400; y--) if (luma(px(img, x, y)) > 90) { silverBottom = y; break; }
  if (silverBottom < 0) return { silverTop: -1, silverBottom: -1 };
  let silverTop = -1;
  for (let y = silverBottom; y >= 400; y--) if (luma(px(img, x, y)) < 70) { silverTop = y + 1; break; }
  return { silverTop, silverBottom };
}
// The bag glyph (a blue "steel blue" body) is detected ONLY in its bounded drawn rect, so the white
// wordmark to its right and the cyan wave behind it cannot be mistaken for the glyph.
function countBag(img: PngImage): number {
  let n = 0;
  for (let y = BAG.y0; y < BAG.y1; y++) for (let x = BAG.x0; x < BAG.x1; x++) {
    const c = px(img, x, y);
    if (c[0] >= 35 && c[1] >= 80 && c[2] >= 110 && c[2] > c[0]) n++;
  }
  return n;
}

let pass = 0, fail = 0;
const results: Record<string, unknown> = {};
const report: string[] = [];
const out = (s = "") => { report.push(s); };
const record = (name: string, ok: boolean, detail: string) => {
  if (ok) { pass++; out(`PASS  ${name}  ${detail}`); }
  else { fail++; out(`FAIL  ${name}  ${detail}`); console.error(`FAIL  ${name}  ${detail}`); }
};

out("# Legacy store visual-polish — geometry gate");
out();
out(`Capture dir: \`${dir}\`  (capture space 960x540 = canvas 1920x1080 / 2)`);
out();
out("Checks below are authored-approximation targets with explicit tolerances, not pixel-perfect");
out("claims. The footer silver strip, dark navy policy bar, upper-centre cyan wave and the bag glyph");
out("are each asserted; a failure exits nonzero so the capture script can fail the run.");
out();

for (const exp of EXPECTED) {
  const png = `${dir}\\${exp.file}`;
  const json = `${png}.json`;
  const cap: Record<string, unknown> = {};

  if (!existsSync(png)) {
    record(`${exp.file} capture exists`, false, `${png} missing`);
    results[exp.file] = { missing: true };
    continue;
  }
  if (!existsSync(json)) {
    record(`${exp.file} sidecar exists`, false, `${json} missing`);
    results[exp.file] = { missingSidecar: true };
    continue;
  }

  let sidecar: any;
  try { sidecar = JSON.parse(readFileSync(json, "utf8")); }
  catch (e) { record(`${exp.file} sidecar parses`, false, String(e)); results[exp.file] = { badSidecar: true }; continue; }

  record(`${exp.file} pageKind`, sidecar.pageKind === exp.pageKind, `expected ${exp.pageKind}, got ${sidecar.pageKind}`);
  record(`${exp.file} selection`, sidecar.selection === exp.selection, `expected ${exp.selection}, got ${sidecar.selection}`);
  record(`${exp.file} focusedRow`, sidecar.focusedRow === exp.focusedRow, `expected ${exp.focusedRow}, got ${sidecar.focusedRow}`);

  let img: PngImage;
  try { img = decodePng(png); } catch (e) { record(`${exp.file} decode`, false, String(e)); results[exp.file] = { decodeError: true }; continue; }
  record(`${exp.file} size`, img.w === 960 && img.h === 540, `got ${img.w}x${img.h}`);

  // Footer silver strip bounded around the target, and the dark navy policy bar below it to the edge.
  const { silverTop, silverBottom } = detectFooter(img);
  record(`${exp.file} footer silver top`, Math.abs(silverTop - FOOTER_SILVER.y0) <= FOOTER_TOL,
    `y=${silverTop} (target ${FOOTER_SILVER.y0}\u00B1${FOOTER_TOL})`);
  record(`${exp.file} footer silver bottom`, Math.abs(silverBottom - FOOTER_SILVER.y1) <= FOOTER_TOL,
    `y=${silverBottom} (target ${FOOTER_SILVER.y1}\u00B1${FOOTER_TOL})`);
  const navy = mean(img, { x0: 800, x1: 900, y0: 515, y1: 535 });
  record(`${exp.file} navy policy bar dark`, luma(navy) < 45, `RGB ${navy.join(",")} luma ${luma(navy).toFixed(0)}`);
  const navyBottom = mean(img, { x0: 800, x1: 900, y0: 535, y1: 539 });
  record(`${exp.file} navy bar reaches frame edge`, luma(navyBottom) < 45, `RGB ${navyBottom.join(",")} luma ${luma(navyBottom).toFixed(0)}`);

  // Cyan wave: the firmware-derived curved crest (store_wave, /datacom/store.mnu). The bright cyan
  // core (glow_col) crosses the upper middle as an arch. Three assertions: the apex band is bright
  // cyan, it is more cyan than the old upper-left orb spot, and the crest is an arch (its bright core
  // spans a vertical range) rather than a straight horizontal line.
  const wc = mean(img, { x0: 400, x1: 530, y0: 40, y1: 48 });
  const wl = mean(img, { x0: 30, x1: 100, y0: 40, y1: 70 });
  const cyanC = wc[2] - wc[0], cyanL = wl[2] - wl[0];
  record(`${exp.file} wave crest is bright cyan`, cyanC >= 150, `RGB ${wc.join(",")} B\u2212R=${cyanC}`);
  record(`${exp.file} wave moved to centre`, cyanC > cyanL + 20, `centre B\u2212R ${cyanC} > old-left ${cyanL}+20`);
  let coreMin = Infinity, coreMax = -Infinity;
  for (let y = 30; y < 160; y++) for (let x = 280; x < 680; x++) {
    const c = px(img, x, y);
    if (c[2] > 190 && c[1] > 170) { if (y < coreMin) coreMin = y; if (y > coreMax) coreMax = y; }
  }
  record(`${exp.file} wave crest is curved (arch)`, coreMin !== Infinity && coreMax - coreMin >= 40,
    `bright crest core y ${coreMin}..${coreMax} (range ${coreMax - coreMin})`);

  // Bag glyph within its bounded rect (the white wordmark at x>=195 is outside it).
  const bagCount = countBag(img);
  record(`${exp.file} bag glyph present`, bagCount >= 100, `${bagCount} blue-body px in x${BAG.x0}..${BAG.x1}`);

  // Mode-specific content.
  if (exp.mode === "catalogue") {
    const lp = mean(img, { x0: 300, x1: 460, y0: 200, y1: 230 });
    record(`${exp.file} left panel blue`, lp[2] - lp[0] >= 50, `RGB ${lp.join(",")} B\u2212R=${lp[2] - lp[0]}`);
    const grid = mean(img, { x0: 480, x1: 800, y0: 120, y1: 180 });
    record(`${exp.file} grid covers present`, luma(grid) > 45, `RGB ${grid.join(",")} luma ${luma(grid).toFixed(0)}`);
  } else {
    const inset = mean(img, { x0: 300, x1: 700, y0: 118, y1: 123 });
    record(`${exp.file} detail inset dark`, luma(inset) < 50, `RGB ${inset.join(",")} luma ${luma(inset).toFixed(0)}`);
    const cover = mean(img, { x0: 155, x1: 265, y0: 181, y1: 291 });
    record(`${exp.file} detail cover present`, luma(cover) > 50, `RGB ${cover.join(",")} luma ${luma(cover).toFixed(0)}`);
  }

  cap.footerSilver = { top: silverTop, bottom: silverBottom };
  cap.navy = navy; cap.navyBottom = navyBottom;
  cap.waveApex = wc; cap.waveOldLeft = wl; cap.waveApexCyan = cyanC; cap.waveOldLeftCyan = cyanL;
  cap.waveCoreYRange = [coreMin, coreMax];
  cap.bagBluePixels = bagCount;
  results[exp.file] = cap;
  out();
}

out(`## Result: ${fail === 0 ? "PASS" : "FAIL"} (${pass} passed, ${fail} failed)`);
out();

const text = report.join("\n");
writeFileSync(`${dir}\\geometry.md`, text);
writeFileSync(`${dir}\\geometry.json`, JSON.stringify({ targets: { footerSilver: FOOTER_SILVER, footerTol: FOOTER_TOL, bag: BAG }, captures: results, pass, fail }, null, 2));
console.log(text);
console.log(`\nWrote ${dir}\\geometry.md and ${dir}\\geometry.json`);

if (fail > 0) { console.error(`\nGEOMETRY GATE FAILED: ${fail} check(s) failed`); process.exit(1); }
console.log(`\nGEOMETRY GATE PASSED: ${pass} check(s)`);
