// Shrinks a 1920x1080 screenshot PNG for the repository: 8-bit RGB/RGBA PNG in, indexed PNG out.
//
//   bun work/png-optimise.ts <in.png> <out.png> [--colors 256] [--max-kb 600] [--dither 0.75]
//
// Lossless first (per-row filter choice, zlib level 9). When that is still over --max-kb the image is quantised to an
// adaptive palette (median cut over a 5-bit histogram) with error-diffusion dithering, which keeps the XMB's smooth
// gradients free of visible banding. No dependencies beyond Bun and node:zlib.
import { readFileSync, writeFileSync } from "node:fs";
import { deflateSync, inflateSync } from "node:zlib";

const [inPath, outPath] = process.argv.slice(2);
const opt = (name: string, dflt: number) => { const i = process.argv.indexOf(`--${name}`); return i >= 0 ? Number(process.argv[i + 1]) : dflt; };
const maxColors = opt("colors", 256), maxBytes = opt("max-kb", 600) * 1024, ditherAmount = opt("dither", 0.6);
if (!inPath || !outPath) { console.error("usage: png-optimise.ts <in> <out>"); process.exit(2); }

function crc32(buf: Buffer) {
  let crc = 0xffffffff;
  for (let n = 0; n < buf.length; n++) { crc ^= buf[n]; for (let k = 0; k < 8; k++) crc = crc & 1 ? 0xedb88320 ^ (crc >>> 1) : crc >>> 1; }
  return (crc ^ 0xffffffff) >>> 0;
}
const chunk = (type: string, body: Buffer) => {
  const t = Buffer.concat([Buffer.from(type), body]);
  const len = Buffer.alloc(4); len.writeUInt32BE(body.length);
  const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(t));
  return Buffer.concat([len, t, crc]);
};

// ---- decode ----
const src = readFileSync(inPath);
let pos = 8, width = 0, height = 0, colorType = 0; const idat: Buffer[] = [];
while (pos < src.length) {
  const len = src.readUInt32BE(pos), type = src.toString("ascii", pos + 4, pos + 8), body = src.subarray(pos + 8, pos + 8 + len);
  if (type === "IHDR") { width = body.readUInt32BE(0); height = body.readUInt32BE(4); colorType = body[9]; if (body[8] !== 8 || body[12] !== 0) throw new Error("only 8-bit non-interlaced PNG"); }
  if (type === "IDAT") idat.push(body);
  pos += 12 + len;
}
const bpp = colorType === 6 ? 4 : colorType === 2 ? 3 : 0;
if (!bpp) throw new Error(`unsupported colour type ${colorType}`);
const raw = inflateSync(Buffer.concat(idat));
const stride = width * bpp;
const px = Buffer.alloc(stride * height);
for (let y = 0; y < height; y++) {
  const f = raw[y * (stride + 1)], o = y * (stride + 1) + 1, d = y * stride;
  for (let i = 0; i < stride; i++) {
    const a = i >= bpp ? px[d + i - bpp] : 0, b = y ? px[d - stride + i] : 0, c = i >= bpp && y ? px[d - stride + i - bpp] : 0;
    let v = raw[o + i];
    if (f === 1) v += a; else if (f === 2) v += b; else if (f === 3) v += (a + b) >> 1;
    else if (f === 4) { const p = a + b - c, pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c); v += pa <= pb && pa <= pc ? a : pb <= pc ? b : c; }
    px[d + i] = v & 255;
  }
}

// ---- encode helpers ----
function filtered(data: Uint8Array, rowBytes: number, pbpp: number) {
  const out = Buffer.alloc((rowBytes + 1) * height);
  const cand = Array.from({ length: 5 }, () => new Uint8Array(rowBytes));
  for (let y = 0; y < height; y++) {
    let best = 0, bestScore = Infinity;
    for (let f = 0; f < 5; f++) {
      let score = 0; const c = cand[f];
      for (let i = 0; i < rowBytes; i++) {
        const x = data[y * rowBytes + i], a = i >= pbpp ? data[y * rowBytes + i - pbpp] : 0, b = y ? data[(y - 1) * rowBytes + i] : 0, cc = i >= pbpp && y ? data[(y - 1) * rowBytes + i - pbpp] : 0;
        let p = 0;
        if (f === 1) p = a; else if (f === 2) p = b; else if (f === 3) p = (a + b) >> 1;
        else if (f === 4) { const q = a + b - cc, pa = Math.abs(q - a), pb = Math.abs(q - b), pc = Math.abs(q - cc); p = pa <= pb && pa <= pc ? a : pb <= pc ? b : cc; }
        const v = (x - p) & 255; c[i] = v; score += v < 128 ? v : 256 - v;
      }
      if (score < bestScore) { bestScore = score; best = f; }
    }
    out[y * (rowBytes + 1)] = best; out.set(cand[best], y * (rowBytes + 1) + 1);
  }
  return out;
}
const sig = Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]);
const ihdr = (ct: number) => { const h = Buffer.alloc(13); h.writeUInt32BE(width, 0); h.writeUInt32BE(height, 4); h[8] = 8; h[9] = ct; return chunk("IHDR", h); };

// Lossless attempt on RGB (alpha is opaque in a framebuffer grab).
const rgb = Buffer.alloc(width * height * 3);
for (let i = 0, j = 0; i < px.length; i += bpp) { rgb[j++] = px[i]; rgb[j++] = px[i + 1]; rgb[j++] = px[i + 2]; }
const lossless = Buffer.concat([sig, ihdr(2), chunk("IDAT", deflateSync(filtered(rgb, width * 3, 3), { level: 9 })), chunk("IEND", Buffer.alloc(0))]);
if (lossless.length <= maxBytes) { writeFileSync(outPath, lossless); console.log(`${outPath}: lossless RGB ${(lossless.length / 1024).toFixed(0)} KB`); process.exit(0); }

// ---- median-cut palette over a 5-bit histogram ----
const hist = new Uint32Array(32768);
for (let i = 0; i < rgb.length; i += 3) hist[((rgb[i] >> 3) << 10) | ((rgb[i + 1] >> 3) << 5) | (rgb[i + 2] >> 3)]++;
type Box = { keys: number[]; count: number };
const keys: number[] = []; for (let k = 0; k < 32768; k++) if (hist[k]) keys.push(k);
const ch = (k: number, c: number) => c === 0 ? k >> 10 : c === 1 ? (k >> 5) & 31 : k & 31;
let boxes: Box[] = [{ keys, count: keys.reduce((s, k) => s + hist[k], 0) }];
while (boxes.length < maxColors) {
  let bi = -1, bs = 0;
  boxes.forEach((b, i) => { if (b.keys.length > 1 && b.count > bs) { bs = b.count; bi = i; } });
  if (bi < 0) break;
  const b = boxes[bi];
  let axis = 0, bestRange = -1;
  for (let c = 0; c < 3; c++) { let lo = 31, hi = 0; for (const k of b.keys) { const v = ch(k, c); if (v < lo) lo = v; if (v > hi) hi = v; } if (hi - lo > bestRange) { bestRange = hi - lo; axis = c; } }
  b.keys.sort((x, y) => ch(x, axis) - ch(y, axis));
  let acc = 0, cut = 1; for (let i = 0; i < b.keys.length; i++) { acc += hist[b.keys[i]]; if (acc >= b.count / 2) { cut = Math.max(1, Math.min(b.keys.length - 1, i + 1)); break; } }
  const a = b.keys.slice(0, cut), c = b.keys.slice(cut);
  boxes.splice(bi, 1, { keys: a, count: a.reduce((s, k) => s + hist[k], 0) }, { keys: c, count: c.reduce((s, k) => s + hist[k], 0) });
}
const palette = boxes.map((b) => {
  let r = 0, g = 0, bl = 0, n = 0;
  for (const k of b.keys) { const h = hist[k]; r += (((k >> 10) << 3) + 4) * h; g += ((((k >> 5) & 31) << 3) + 4) * h; bl += (((k & 31) << 3) + 4) * h; n += h; }
  return [Math.round(r / n), Math.round(g / n), Math.round(bl / n)];
});
// nearest-palette cache keyed by 5-bit colour
const cache = new Int16Array(32768).fill(-1);
const nearest = (r: number, g: number, b: number) => {
  const key = ((Math.max(0, Math.min(255, r)) >> 3) << 10) | ((Math.max(0, Math.min(255, g)) >> 3) << 5) | (Math.max(0, Math.min(255, b)) >> 3);
  let best = cache[key];
  if (best < 0) {
    let bd = Infinity; const rr = ((key >> 10) << 3) + 4, gg = (((key >> 5) & 31) << 3) + 4, bb = ((key & 31) << 3) + 4;
    for (let i = 0; i < palette.length; i++) { const dr = palette[i][0] - rr, dg = palette[i][1] - gg, db = palette[i][2] - bb, d = dr * dr * 3 + dg * dg * 4 + db * db * 2; if (d < bd) { bd = d; best = i; } }
    cache[key] = best;
  }
  return best;
};
// Floyd-Steinberg on the true colours; the strongest dither that still fits the size budget wins (an explicit --dither is tried first).
const plte = Buffer.alloc(palette.length * 3); palette.forEach((p, i) => plte.set(p, i * 3));
function encodeWith(amount: number) {
  const idx = new Uint8Array(width * height);
  let errA = new Float32Array((width + 2) * 3), errB = new Float32Array((width + 2) * 3);
  for (let y = 0; y < height; y++) {
    errB.fill(0);
    for (let x = 0; x < width; x++) {
      const o = (y * width + x) * 3;
      const r = rgb[o] + errA[(x + 1) * 3], g = rgb[o + 1] + errA[(x + 1) * 3 + 1], b = rgb[o + 2] + errA[(x + 1) * 3 + 2];
      const n = nearest(Math.round(r), Math.round(g), Math.round(b)); idx[y * width + x] = n;
      const e = [(r - palette[n][0]) * amount, (g - palette[n][1]) * amount, (b - palette[n][2]) * amount];
      for (let c = 0; c < 3; c++) {
        errA[(x + 2) * 3 + c] += e[c] * 7 / 16; errB[x * 3 + c] += e[c] * 3 / 16; errB[(x + 1) * 3 + c] += e[c] * 5 / 16; errB[(x + 2) * 3 + c] += e[c] / 16;
      }
    }
    [errA, errB] = [errB, errA];
  }
  return Buffer.concat([sig, ihdr(3), chunk("PLTE", plte), chunk("IDAT", deflateSync(filtered(idx, width, 1), { level: 9 })), chunk("IEND", Buffer.alloc(0))]);
}
let chosen = Buffer.alloc(0), used = 0;
for (const amount of [ditherAmount, 0.4, 0.3, 0.22, 0.16, 0.1, 0.06, 0]) {
  chosen = encodeWith(amount); used = amount;
  if (chosen.length <= maxBytes) break;
}
writeFileSync(outPath, chosen);
console.log(`${outPath}: ${palette.length}-colour indexed, dither ${used}, ${(chosen.length / 1024).toFixed(0)} KB (lossless was ${(lossless.length / 1024).toFixed(0)} KB)`);
