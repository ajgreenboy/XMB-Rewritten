// hdd-fixture.ts - a synthetic dev_hdd0 for the render gate's Trophy Collection / Game Data Utility states.
// Everything is generated at capture time into the capture's own temp directory, so no binary fixture is
// committed: PARAM.SFO, TROPCONF.SFM, TROPUSR.DAT and solid-colour PNGs are written byte by byte here.
// The layouts follow native-xmb/src/trophy_data.cpp (RPCS3 TROPUSR.h: magic 0x818F54AD, table type 6).
import { deflateSync } from 'node:zlib';
import { mkdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

const crcTable = Uint32Array.from({ length: 256 }, (_, n) => {
  for (let k = 0; k < 8; k++) n = (n >>> 1) ^ ((n & 1) ? 0xedb88320 : 0);
  return n >>> 0;
});
function crc32(bytes: Uint8Array) {
  let crc = 0xffffffff;
  for (const n of bytes) crc = (crc >>> 8) ^ crcTable[(crc ^ n) & 255];
  return (crc ^ 0xffffffff) >>> 0;
}
function chunk(type: string, data: Buffer) {
  const out = Buffer.alloc(12 + data.length);
  out.writeUInt32BE(data.length, 0);
  out.write(type, 4, 'ascii');
  data.copy(out, 8);
  out.writeUInt32BE(crc32(out.subarray(4, 8 + data.length)), 8 + data.length);
  return out;
}

// An 8-bit RGB PNG: a vertical gradient from `top` to `bottom`, so a frame shows that the picture was drawn
// (a flat fill would be indistinguishable from a background colour).
export function encodeGradientPng(w: number, h: number, top: [number, number, number], bottom: [number, number, number]) {
  const stride = w * 3 + 1;
  const raw = Buffer.alloc(stride * h);
  for (let y = 0; y < h; y++) {
    const t = h > 1 ? y / (h - 1) : 0;
    const px = top.map((c, i) => Math.round(c + (bottom[i] - c) * t));
    const row = y * stride;
    raw[row] = 0;
    for (let x = 0; x < w; x++) raw.set(px, row + 1 + x * 3);
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0);
  ihdr.writeUInt32BE(h, 4);
  ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
  return Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', ihdr), chunk('IDAT', deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]);
}

// PARAM.SFO: "\0PSF", version, key table @8, data table @12, count @16, 16-byte entries (all little-endian).
export function encodeSfo(values: Array<[string, string]>) {
  const keys = Buffer.from(values.map(([k]) => k + '\0').join(''), 'utf8');
  const datas = values.map(([, v]) => Buffer.from(v + '\0', 'utf8'));
  const keysOff = 20 + values.length * 16;
  const dataOff = (keysOff + keys.length + 3) & ~3;
  const header = Buffer.alloc(dataOff);
  header.write('\0PSF', 0, 'latin1');
  header.writeUInt32LE(0x101, 4);
  header.writeUInt32LE(keysOff, 8);
  header.writeUInt32LE(dataOff, 12);
  header.writeUInt32LE(values.length, 16);
  keys.copy(header, keysOff);
  let keyRel = 0, dataRel = 0;
  values.forEach(([k], i) => {
    const e = 20 + i * 16;
    header.writeUInt16LE(keyRel, e);
    header.writeUInt16LE(0x0204, e + 2);
    header.writeUInt32LE(datas[i].length, e + 4);
    header.writeUInt32LE(datas[i].length, e + 8);
    header.writeUInt32LE(dataRel, e + 12);
    keyRel += Buffer.byteLength(k, 'utf8') + 1;
    dataRel += datas[i].length;
  });
  return Buffer.concat([header, ...datas]);
}

// TROPUSR.DAT with one type-6 table. `unlocked` maps trophy id -> Unix seconds (0 = locked).
export function encodeTropUsr(unlocked: number[]) {
  const deltaSecs = 62135596800n;
  const header = 0x30, tableHeader = 0x20, stride = 0x60;
  const tableOff = header + tableHeader;
  const out = Buffer.alloc(tableOff + unlocked.length * stride);
  out.writeUInt32BE(0x818f54ad, 0);
  out.writeUInt32BE(1, 8);
  out.writeUInt32BE(6, header);
  out.writeUInt32BE(stride - 0x10, header + 4);
  out.writeUInt32BE(1, header + 8);
  out.writeUInt32BE(unlocked.length, header + 12);
  out.writeBigUInt64BE(BigInt(tableOff), header + 16);
  unlocked.forEach((secs, id) => {
    const rec = tableOff + id * stride;
    const tick = secs > 0 ? (BigInt(secs) + deltaSecs) * 1000000n : 0n;
    out.writeUInt32BE(6, rec);
    out.writeUInt32BE(stride - 0x10, rec + 4);
    out.writeUInt32BE(id, rec + 8);
    out.writeUInt32BE(id, rec + 0x10);
    out.writeUInt32BE(secs > 0 ? 1 : 0, rec + 0x14);
    out.writeBigUInt64BE(tick, rec + 0x20);
    out.writeBigUInt64BE(tick, rec + 0x28);
  });
  return out;
}

// One trophy set (four trophies: P locked, G earned, hidden S locked, B earned) and one game-data folder with
// ICON0, PIC1 and a CATEGORY "GD" PARAM.SFO. Returns the dev_hdd0 root.
export function writeSyntheticHdd(captureDir: string) {
  const root = join(captureDir, 'dev_hdd0');
  const set = join(root, 'home', '00000001', 'trophy', 'NPWR90001_00');
  mkdirSync(set, { recursive: true });
  writeFileSync(join(set, 'TROPCONF.SFM'),
    '<?xml version="1.0" encoding="UTF-8"?>\n<trophyconf version="1.1">\n' +
    '  <title-name>Render Gate Trophies</title-name>\n  <title-detail>Synthetic set for the render gate</title-detail>\n' +
    '  <trophy id="0" hidden="no" ttype="P"><name>All Clear</name><detail>Earn every trophy.</detail></trophy>\n' +
    '  <trophy id="1" hidden="no" ttype="G"><name>Boss Down</name><detail>Defeat the boss.</detail></trophy>\n' +
    '  <trophy id="2" hidden="yes" ttype="S"><name>Secret Room</name><detail>Find the secret room.</detail></trophy>\n' +
    '  <trophy id="3" hidden="no" ttype="B"><name>First Step</name><detail>Start the adventure.</detail></trophy>\n' +
    '</trophyconf>\n');
  // Fixed UTC noon timestamps; the row shows them in local time, which is stable on one machine.
  writeFileSync(join(set, 'TROPUSR.DAT'), encodeTropUsr([0, 1761393600, 0, 1761307200]));
  writeFileSync(join(set, 'ICON0.PNG'), encodeGradientPng(320, 176, [40, 90, 170], [10, 20, 60]));
  const trophyColours: Array<[number, number, number]> = [[200, 200, 220], [230, 180, 40], [170, 170, 170], [180, 110, 60]];
  trophyColours.forEach((c, i) => writeFileSync(join(set, `TROP00${i}.PNG`), encodeGradientPng(120, 120, c, [20, 20, 20])));

  const data = join(root, 'game', 'XMBF00001DATA');
  mkdirSync(join(data, 'USRDIR'), { recursive: true });
  writeFileSync(join(data, 'PARAM.SFO'), encodeSfo([
    ['APP_VER', '01.02'], ['CATEGORY', 'GD'], ['TITLE', 'Render Gate Game Data'], ['TITLE_ID', 'XMBF00001']]));
  writeFileSync(join(data, 'ICON0.PNG'), encodeGradientPng(320, 176, [150, 40, 90], [40, 10, 30]));
  writeFileSync(join(data, 'PIC1.PNG'), encodeGradientPng(1920, 1080, [20, 70, 60], [5, 15, 25]));
  writeFileSync(join(data, 'USRDIR', 'DATA.BIN'), Buffer.alloc(64 * 1024, 0x5a));

  // One PS3 save (home/<user>/savedata/<dir>) for Saved Data Utility (PS3) and its Copy / Delete / Information menu.
  const save = join(root, 'home', '00000001', 'savedata', 'XMBF00001-SAVE0');
  mkdirSync(save, { recursive: true });
  writeFileSync(join(save, 'PARAM.SFO'), encodeSfo([
    ['DETAIL', 'Chapter 2 - The Gate'], ['SUB_TITLE', 'Slot 1'], ['TITLE', 'Render Gate Save']]));
  writeFileSync(join(save, 'ICON0.PNG'), encodeGradientPng(320, 176, [60, 140, 80], [10, 40, 20]));
  writeFileSync(join(save, 'DATA.SAV'), Buffer.alloc(16 * 1024, 0x33));
  return root;
}
