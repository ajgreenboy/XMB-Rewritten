// Regression check for the named-state deterministic-region gate.  It mutates
// pixels in the two UI surfaces that must remain gating (the in-game guide and
// the focused autologon row) and proves the configured metrics reject them.
// This intentionally uses the frozen references as both sides: it exercises
// only the gate's metric floors and cannot depend on a live capture or a
// machine-specific compositor.
import { readFileSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { psnr, ssim, type PngImage, type Crop } from './ab-compare';

const repo = resolve(import.meta.dir, '..');
const thresholds = JSON.parse(readFileSync(join(repo, 'work', 'ab-layers.thresholds.json'), 'utf8'));
const floor = thresholds.stateSelfBaseline;

// Keep this check runnable from CI and from any caller cwd. It deliberately
// uses a synthetic frame: the assertion is about the configured metric floor
// rejecting a changed gated surface, not about requiring private reference
// captures or firmware assets.
const blank: PngImage = { w: 1920, h: 1080, rgba: new Uint8Array(1920 * 1080 * 4) };

function mutate(source: PngImage, x: number, y: number, w: number, h: number): PngImage {
  const rgba = new Uint8Array(source.rgba);
  for (let yy = y; yy < y + h; yy++) for (let xx = x; xx < x + w; xx++) {
    const i = (yy * source.w + xx) * 4;
    rgba[i] = 255;
    rgba[i + 1] = 0;
    rgba[i + 2] = 255;
  }
  return { w: source.w, h: source.h, rgba };
}

function state(name: string) {
  const found = thresholds.states.find((candidate: any) => candidate.name === name);
  if (!found) throw new Error(`missing state ${name}`);
  return found;
}

function check(name: string, regionName: string, mutation: [number, number, number, number]) {
  const recipe = state(name);
  const reference = blank;
  const [x, y, w, h] = mutation;
  const region = recipe.regions.find((candidate: any) => candidate.name === regionName);
  if (!region) throw new Error(`${name}: mutation point is not covered by a gating region`);
  const [rx, ry, rw, rh] = region.crop as Crop;
  if (x < rx || y < ry || x + w > rx + rw || y + h > ry + rh)
    throw new Error(`${name}: mutation does not fit entirely inside ${region.name}`);
  const changed = mutate(reference, x, y, w, h);
  const crop = region.crop as Crop;
  const p = psnr(reference, changed, crop);
  const s = ssim(reference, changed, crop);
  if (p >= floor.psnrDb && s >= floor.ssim)
    throw new Error(`${name}: mutation passed ${region.name} (${p.toFixed(2)} dB / ${s.toFixed(4)})`);
  console.log(`${name}: ${region.name} mutation rejected (${p.toFixed(2)} dB / ${s.toFixed(4)})`);
}

check('ingame-xmb', 'guide', [200, 960, 20, 20]);
check('settings-autologon-row', 'focused row', [800, 500, 20, 20]);
check('first-run-autologon-step', 'step rows', [800, 600, 20, 20]);
