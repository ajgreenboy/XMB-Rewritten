// legacy-store-geometry.ts - analytical pixel-geometry check for the legacy PlayStation Store
// visual-polish pass. Decodes the offline fixture captures and measures the regions the
// vision-capable orchestrator flagged against the 2008 period references, so the renderer's
// authored geometry is verified against a target rather than eyeballed.
//
//   bun work/legacy-store-geometry.ts [capture-dir]
//
// Capture dir defaults to C:\dev\out\legacy-store-visual-polish. The captures are 960x540
// windows of the 1920x1080 logical canvas, i.e. capture = canvas / 2. Targets below are in
// capture space (the reference 1024x576 scaled x0.9375 to 960x540, the space the orchestrator
// measured in).
import { readFileSync, existsSync, writeFileSync, readdirSync } from "node:fs";
import { decodePng, type PngImage } from "./ab-compare.ts";

const dir = process.argv[2] ?? "C:\\dev\\out\\legacy-store-visual-polish";

type Rect = { x0: number; x1: number; y0: number; y1: number };

// Target geometry in capture space (960x540), from the orchestrator's measurements.
const TARGET = {
  headerLogo: { x0: 147, x1: 414, y0: 38, y1: 75 } as Rect,
  storeTop: { x0: 147, y: 83 },
  wave: { x0: 300, x1: 715, y0: 0, y1: 125 } as Rect,
  footerSilver: { y0: 465, y1: 507 },
  footerNavy: { y0: 507, y1: 540 },
  leftPanel: { x0: 147, x1: 473, y0: 115, y1: 430 } as Rect,
  grid: { x0: 476, x1: 806 },
  detailInset: { x0: 145, x1: 816, y0: 115, y1: 431 } as Rect,
};

function px(img: PngImage, x: number, y: number): [number, number, number, number] {
  const i = (Math.floor(y) * img.w + Math.floor(x)) * 4;
  return [img.rgba[i], img.rgba[i + 1], img.rgba[i + 2], img.rgba[i + 3]];
}
function luma(c: [number, number, number, number]): number {
  return 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2];
}
// Mean colour of a rect (downsampled stride for speed).
function mean(img: PngImage, r: Rect): [number, number, number] {
  let rsum = 0, gsum = 0, bsum = 0, n = 0;
  for (let y = Math.max(0, Math.floor(r.y0)); y < Math.min(img.h, Math.ceil(r.y1)); y += 2)
    for (let x = Math.max(0, Math.floor(r.x0)); x < Math.min(img.w, Math.ceil(r.x1)); x += 2) {
      const c = px(img, x, y); rsum += c[0]; gsum += c[1]; bsum += c[2]; n++;
    }
  return n ? [Math.round(rsum / n), Math.round(gsum / n), Math.round(bsum / n)] : [0, 0, 0];
}
// Scan a column from the bottom and return the y of the first luma crossing of `thresh` (bottom->up).
function scanUp(img: PngImage, x: number, thresh: number, fromY: number, toY: number): number {
  for (let y = fromY; y >= toY; y--) if (luma(px(img, x, y)) > thresh) return y;
  return -1;
}
// Leftmost bright (non-cyan) pixel in a horizontal band: the logo's left edge. The wordmark is
// white; the cyan wave is excluded by requiring red >= blue (cyan has blue >> red).
function leftmostBright(img: PngImage, y0: number, y1: number): number {
  for (let x = 0; x < img.w; x++)
    for (let y = y0; y <= y1; y += 2) {
      const c = px(img, x, y);
      if (c[0] > 150 && c[1] > 150 && c[2] > 150 && c[0] >= c[2]) return x;
    }
  return -1;
}

function analyze(): void {
  const files = readdirSync(dir).filter(f => /\.png$/.test(f) && !/\.json$/.test(f)).sort();
  const results: Record<string, unknown> = {};
  const rows: string[] = [];
  const line = (s = "") => { rows.push(s); };

  line("# Legacy store visual-polish — analytical geometry");
  line();
  line(`Capture dir: \`${dir}\`  (capture space 960x540 = canvas 1920x1080 / 2)`);
  line();

  for (const f of files) {
    const path = `${dir}\\${f}`;
    if (!existsSync(path)) continue;
    const img = decodePng(path);
    if (img.w !== 960 || img.h !== 540) { line(`- ${f}: unexpected size ${img.w}x${img.h}, skipped`); continue; }

    // Footer: scan a column clear of the left panel / grid (x=480) from the bottom.
    const silverBottom = scanUp(img, 480, 90, 539, 400);   // navy -> silver boundary (navy bar bottom)
    const silverTop = scanUp(img, 480, 90, silverBottom - 4, 400) > 0
      ? (() => { for (let y = silverBottom; y >= 400; y--) if (luma(px(img, 480, y)) < 70) return y + 1; return -1; })()
      : -1;
    const footerSilver = { y0: silverTop, y1: silverBottom };

    // Header logo left edge in the logo band (y 30..90).
    const logoLeft = leftmostBright(img, 30, 90);

    // Cyan wave: mean colour upper-centre vs upper-left (the old orb sat upper-left).
    const waveCentre = mean(img, { x0: 430, x1: 580, y0: 30, y1: 90 });
    const waveLeft = mean(img, { x0: 100, x1: 250, y0: 30, y1: 90 });

    // Detail / catalogue panels.
    const leftPanel = mean(img, { x0: 150, x1: 470, y0: 200, y1: 300 });
    const gridCard = mean(img, { x0: 480, x1: 600, y0: 200, y1: 300 });
    const footerSilverColour = footerSilver.y0 > 0 ? mean(img, { x0: 400, x1: 560, y0: footerSilver.y0 + 4, y1: footerSilver.y1 - 4 }) : [0, 0, 0];
    const footerNavyColour = mean(img, { x0: 400, x1: 560, y0: 515, y1: 535 });
    // The dark navy policy bar must run from the silver bottom to the frame edge (y540).
    const navyTopColour = mean(img, { x0: 400, x1: 560, y0: 508, y1: 513 });
    const navyBottomColour = mean(img, { x0: 400, x1: 560, y0: 535, y1: 539 });
    // Detail inset (dark) vs catalogue blue left panel: sample the same x-band centre.
    const detailInset = mean(img, { x0: 150, x1: 470, y0: 220, y1: 280 });

    const r = {
      file: f,
      size: `${img.w}x${img.h}`,
      footerSilver,
      footerNavyTop: navyTopColour,
      footerNavyBottom: navyBottomColour,
      logoLeft,
      waveCentre,
      waveLeft,
      leftPanel,
      detailInset,
      gridCard,
      footerSilverColour,
      footerNavyColour,
    };
    results[f] = r;
    line(`## ${f}`);
    line();
    line(`| Measure | Target (px) | Measured |`);
    line(`|---|---|---|---|`);
    line(`| Footer silver strip | y ${TARGET.footerSilver.y0}..${TARGET.footerSilver.y1} | y ${footerSilver.y0}..${footerSilver.y1} |`);
    line(`| Footer navy bar | y ${TARGET.footerNavy.y0}..${TARGET.footerNavy.y1} (to frame edge) | top ${navyTopColour.join(",")}, bottom ${navyBottomColour.join(",")} |`);
    line(`| Header logo left | x ${TARGET.headerLogo.x0} | x ${logoLeft} |`);
    line(`| Wave centre RGB | cyan, upper centre | ${waveCentre.join(",")} |`);
    line(`| Wave left RGB (old orb spot) | dimmer than centre | ${waveLeft.join(",")} |`);
    line(`| Left panel RGB | blue gradient (catalogue) / dark (detail) | ${leftPanel.join(",")} |`);
    line(`| Detail inset RGB | dark inset (detail) | ${detailInset.join(",")} |`);
    line(`| Grid card RGB | coloured cover | ${gridCard.join(",")} |`);
    line(`| Footer silver RGB | ~RGB(150..215) | ${footerSilverColour.join(",")} |`);
    line(`| Footer navy RGB | dark navy | ${footerNavyColour.join(",")} |`);
    line();
  }

  // Verdict summary.
  line("## Verdict");
  line();
  line("Authored geometry targets (renderer canvas 1920x1080, /2 = capture): header logo bag at x294 "
    + "(capture x147), wordmark x390, \"Store Top\" x294 y164, cyan wave crossing upper centre (glow "
    + "centre x1010 y110, streak y0..250), footer silver strip y930..1014 + dark navy policy bar "
    + "y1014..1080. See the renderer in `View::drawLegacySurface` and the limitations below.");
  line();

  const report = rows.join("\n");
  writeFileSync(`${dir}\\geometry.md`, report);
  writeFileSync(`${dir}\\geometry.json`, JSON.stringify({ target: TARGET, captures: results }, null, 2));
  console.log(report);
  console.log(`\nWrote ${dir}\\geometry.md and ${dir}\\geometry.json`);
}

try { analyze(); } catch (e) {
  console.error(e instanceof Error ? e.message : String(e));
  process.exit(2);
}
