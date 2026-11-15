import { decodePng, psnr, ssim, type PngImage } from './ab-compare';

// Scores a frame grabbed at another size / device ratio against the 1920x1080 DPR 1 frame of the same state.
//   bun work/dpi-compare.ts <ref-1920x1080.png> <frame.png> [--sharp]
// The frame is cropped to its contain-fit 16:9 area (the same rectangle DesignSpace uses), box-averaged down to
// 1920x1080 and scored with PSNR and SSIM. Reports the frame size, the 16:9 area, how much of the frame outside that
// area is non-black (pillar/letterbox bars must be black), and, with --sharp, the mean squared Laplacian (edge
// energy) of the full-resolution frame next to the DPR 1 frame bilinearly upscaled to the same size: a frame that
// really renders at its own resolution has far more edge energy than an upscaled 1080p one.

const [refPath, imgPath, ...rest] = process.argv.slice(2);
if (!refPath || !imgPath) { console.error('usage: dpi-compare.ts <ref.png> <frame.png> [--sharp]'); process.exit(2); }
const ref = decodePng(refPath), img = decodePng(imgPath);
const s = Math.min(img.w / 1920, img.h / 1080);
const cw = Math.round(1920 * s), ch = Math.round(1080 * s);
const left = Math.round((img.w - cw) / 2), top = Math.round((img.h - ch) / 2);

function box(src: PngImage, x0: number, y0: number, w: number, h: number, tw: number, th: number): PngImage {
  const out = new Uint8Array(tw * th * 4);
  for (let y = 0; y < th; y++) {
    const ya = y0 + (y * h) / th, yb = y0 + ((y + 1) * h) / th;
    for (let x = 0; x < tw; x++) {
      const xa = x0 + (x * w) / tw, xb = x0 + ((x + 1) * w) / tw;
      let r = 0, g = 0, b = 0, n = 0;
      for (let yy = Math.floor(ya); yy < Math.max(Math.floor(ya) + 1, Math.ceil(yb)); yy++)
        for (let xx = Math.floor(xa); xx < Math.max(Math.floor(xa) + 1, Math.ceil(xb)); xx++) {
          const i = (Math.min(yy, src.h - 1) * src.w + Math.min(xx, src.w - 1)) * 4;
          r += src.rgba[i]; g += src.rgba[i + 1]; b += src.rgba[i + 2]; n++;
        }
      const o = (y * tw + x) * 4; out[o] = r / n; out[o + 1] = g / n; out[o + 2] = b / n; out[o + 3] = 255;
    }
  }
  return { w: tw, h: th, rgba: out };
}
function bilinear(src: PngImage, tw: number, th: number): PngImage {
  const out = new Uint8Array(tw * th * 4);
  for (let y = 0; y < th; y++) {
    const fy = Math.max(0, (y + 0.5) * src.h / th - 0.5), y0 = Math.min(src.h - 1, Math.floor(fy)), y1 = Math.min(src.h - 1, y0 + 1), wy = fy - y0;
    for (let x = 0; x < tw; x++) {
      const fx = Math.max(0, (x + 0.5) * src.w / tw - 0.5), x0 = Math.min(src.w - 1, Math.floor(fx)), x1 = Math.min(src.w - 1, x0 + 1), wx = fx - x0;
      for (let c = 0; c < 3; c++) {
        const v = (src.rgba[(y0 * src.w + x0) * 4 + c] * (1 - wx) + src.rgba[(y0 * src.w + x1) * 4 + c] * wx) * (1 - wy)
          + (src.rgba[(y1 * src.w + x0) * 4 + c] * (1 - wx) + src.rgba[(y1 * src.w + x1) * 4 + c] * wx) * wy;
        out[(y * tw + x) * 4 + c] = v;
      }
      out[(y * tw + x) * 4 + 3] = 255;
    }
  }
  return { w: tw, h: th, rgba: out };
}
let outside = 0, outsideN = 0;
for (let y = 0; y < img.h; y += 2) for (let x = 0; x < img.w; x += 2) {
  if (x >= left && x < left + cw && y >= top && y < top + ch) continue;
  outsideN++; const i = (y * img.w + x) * 4;
  if (img.rgba[i] + img.rgba[i + 1] + img.rgba[i + 2] > 12) outside++;
}
const down = box(img, left, top, cw, ch, ref.w, ref.h);
console.log(`frame ${img.w}x${img.h}  16:9 area ${cw}x${ch} at ${left},${top}  scale ${s.toFixed(4)}  non-black outside area: ${outside}/${outsideN}`);
console.log(`vs ref (downscaled to ${ref.w}x${ref.h}): PSNR ${psnr(ref, down).toFixed(2)} dB  SSIM ${ssim(ref, down).toFixed(4)}`);

if (rest.includes('--sharp')) {
  const lap = (p: PngImage, x0: number, y0: number, w: number, h: number) => {
    let sum = 0, n = 0;
    const g = (x: number, y: number) => { const i = (y * p.w + x) * 4; return 0.299 * p.rgba[i] + 0.587 * p.rgba[i + 1] + 0.114 * p.rgba[i + 2]; };
    for (let y = y0 + 1; y < y0 + h - 1; y++) for (let x = x0 + 1; x < x0 + w - 1; x++) {
      const v = 4 * g(x, y) - g(x - 1, y) - g(x + 1, y) - g(x, y - 1) - g(x, y + 1); sum += v * v; n++;
    }
    return sum / n;
  };
  // Text/list area of the design space, mapped into each image.
  const area = [900, 380, 700, 520];
  const full = lap(img, left + Math.round(area[0] * s), top + Math.round(area[1] * s), Math.round(area[2] * s), Math.round(area[3] * s));
  const up = bilinear(ref, cw, ch); // what a 1080p buffer stretched to the panel looks like
  const upE = lap(up, Math.round(area[0] * s), Math.round(area[1] * s), Math.round(area[2] * s), Math.round(area[3] * s));
  // ratio of edge energy per device pixel: a 1080p source stretched by s has its energy divided by about s^2... report both
  console.log(`edge energy (text area): frame ${full.toFixed(1)}  1080p frame resampled to the same size ${upE.toFixed(1)}  ratio ${(full / Math.max(upE, 1e-9)).toFixed(2)}`);
}
