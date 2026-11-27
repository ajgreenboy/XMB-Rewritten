#![forbid(unsafe_code)]

use std::collections::HashMap;

const MAX_INPUT_BYTES: usize = 64 * 1024 * 1024;
const MAX_DIMENSION: i32 = 4096;
const MAX_CUES: usize = 20000;
const MAX_IMAGE_BYTES: i64 = 512 * 1024 * 1024;
const MAX_OBJECT_BYTES: i64 = 256 * 1024 * 1024;
const MAX_PIXEL_WORK: i64 = 1_500_000_000;
const MAX_PGS_OBJECTS: usize = 64;
const MAX_PGS_WINDOWS: usize = 64;
const MAX_PGS_COMP_OBJECTS: usize = 64;
const MAX_IDX_ENTRIES: usize = 200_000;
const MAX_RESYNC_BYTES: usize = 256 * 1024;
const MAX_SPU_PACKETS: i32 = 128;
const DEFAULT_DURATION_MS: i64 = 5000;
const MAX_GAP_AS_DURATION_MS: i64 = 10000;

#[inline]
fn be16(p: &[u8]) -> i32 {
    (i32::from(p[0]) << 8) | i32::from(p[1])
}

#[inline]
fn be32(p: &[u8]) -> u32 {
    u32::from_be_bytes([p[0], p[1], p[2], p[3]])
}

#[inline]
const fn qrgba(r: u32, g: u32, b: u32, a: u32) -> u32 {
    ((a & 0xff) << 24) | ((r & 0xff) << 16) | ((g & 0xff) << 8) | (b & 0xff)
}

#[inline]
const fn qalpha(c: u32) -> u32 {
    (c >> 24) & 0xff
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct Rect {
    pub x: i32,
    pub y: i32,
    pub w: i32,
    pub h: i32,
}

impl Rect {
    fn is_empty(self) -> bool {
        self.w <= 0 || self.h <= 0
    }
    fn right_excl(self) -> i32 {
        self.x + self.w
    }
    fn bottom_excl(self) -> i32 {
        self.y + self.h
    }
    fn intersect(self, other: Rect) -> Rect {
        if self.is_empty() || other.is_empty() {
            return Rect::default();
        }
        let l = self.x.max(other.x);
        let t = self.y.max(other.y);
        let r = self.right_excl().min(other.right_excl());
        let b = self.bottom_excl().min(other.bottom_excl());
        if r <= l || b <= t {
            Rect::default()
        } else {
            Rect { x: l, y: t, w: r - l, h: b - t }
        }
    }
    fn unite(self, other: Rect) -> Rect {
        if self.is_empty() {
            return other;
        }
        if other.is_empty() {
            return self;
        }
        let l = self.x.min(other.x);
        let t = self.y.min(other.y);
        let r = self.right_excl().max(other.right_excl());
        let b = self.bottom_excl().max(other.bottom_excl());
        Rect { x: l, y: t, w: r - l, h: b - t }
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Default)]
pub struct RawCue {
    pub start_ms: i64,
    pub end_ms: i64,
    pub rect: Rect,
    pub video_w: i32,
    pub video_h: i32,
    pub forced: bool,
    pub palette: Vec<u32>,
    pub pixels: Vec<u8>,
}

impl RawCue {
    fn image_bytes_qt(&self) -> i64 {
        // Qt's QImage::Format_Indexed8 pads scanlines to 4-byte alignment (`(width + 3) & ~3`).
        let stride = i64::from((self.rect.w + 3) & !3);
        stride * i64::from(self.rect.h)
    }
}

struct Timeline {
    out: Vec<RawCue>,
    bytes: i64,
    full: bool,
    dropped: i32,
}

impl Timeline {
    fn new() -> Self {
        Self { out: Vec::new(), bytes: 0, full: false, dropped: 0 }
    }
    fn add(&mut self, cue: RawCue) -> bool {
        if self.full {
            return false;
        }
        if cue.end_ms <= cue.start_ms {
            self.dropped += 1;
            return true;
        }
        let size = cue.image_bytes_qt();
        if self.out.len() >= MAX_CUES || self.bytes + size > MAX_IMAGE_BYTES {
            self.full = true;
            return false;
        }
        self.bytes += size;
        self.out.push(cue);
        true
    }
}

fn trim_to_visible(pixels: &mut Vec<u8>, w: &mut i32, h: &mut i32, palette: &[u32], rect: &mut Rect) -> bool {
    let mut visible = [false; 256];
    for (i, &c) in palette.iter().enumerate().take(256) {
        visible[i] = qalpha(c) != 0;
    }
    let width = *w as usize;
    let height = *h as usize;
    let mut left = width as i32;
    let mut right = -1i32;
    let mut top = height as i32;
    let mut bottom = -1i32;
    for y in 0..height {
        let line = &pixels[y * width..(y + 1) * width];
        let mut first = -1i32;
        let mut last = -1i32;
        for (x, &px) in line.iter().enumerate() {
            if visible[px as usize] {
                first = x as i32;
                break;
            }
        }
        if first < 0 {
            continue;
        }
        for x in (first as usize..width).rev() {
            if visible[line[x] as usize] {
                last = x as i32;
                break;
            }
        }
        left = left.min(first);
        right = right.max(last);
        top = top.min(y as i32);
        bottom = y as i32;
    }
    if right < 0 {
        return false;
    }
    let nw = right - left + 1;
    let nh = bottom - top + 1;
    if left != 0 || top != 0 || nw != *w || nh != *h {
        let mut cropped = Vec::with_capacity((nw as usize) * (nh as usize));
        for y in top..(top + nh) {
            let start = (y as usize) * width + (left as usize);
            cropped.extend_from_slice(&pixels[start..start + (nw as usize)]);
        }
        *pixels = cropped;
        *w = nw;
        *h = nh;
    }
    *rect = Rect { x: rect.x + left, y: rect.y + top, w: nw, h: nh };
    true
}

// ---- PGS --------------------------------------------------------------------

#[derive(Clone, Default)]
struct PgsObject {
    width: i32,
    height: i32,
    rle: Vec<u8>,
    decoded: bool,
    pixels: Vec<u8>,
}

#[derive(Clone, Copy, Default)]
struct YCbCrA {
    y: u8,
    cr: u8,
    cb: u8,
    a: u8,
}

impl YCbCrA {
    const fn default_entry() -> Self {
        Self { y: 16, cr: 128, cb: 128, a: 0 }
    }
}

#[derive(Clone)]
struct PgsCompObj {
    object_id: i32,
    window_id: i32,
    cropped: bool,
    forced: bool,
    x: i32,
    y: i32,
    crop: Rect,
}

#[derive(Clone, Default)]
struct PgsComposition {
    ms: i64,
    width: i32,
    height: i32,
    state: i32,
    palette_id: i32,
    objects: Vec<PgsCompObj>,
}

fn pgs_colour(e: YCbCrA, hd: bool) -> u32 {
    if e.a == 0 {
        return qrgba(0, 0, 0, 0);
    }
    let y = (f64::from(e.y) - 16.0) * 255.0 / 219.0;
    let cb = (f64::from(e.cb) - 128.0) * 255.0 / 224.0;
    let cr = (f64::from(e.cr) - 128.0) * 255.0 / 224.0;
    let (r, g, b) = if hd {
        (y + 1.5748 * cr, y - 0.187324 * cb - 0.468124 * cr, y + 1.8556 * cb)
    } else {
        (y + 1.402 * cr, y - 0.344136 * cb - 0.714136 * cr, y + 1.772 * cb)
    };
    let clamp = |v: f64| -> u32 { v.clamp(0.0, 255.0).round() as u32 };
    qrgba(clamp(r), clamp(g), clamp(b), u32::from(e.a))
}

fn decode_pgs_object(obj: &mut PgsObject) {
    obj.decoded = true;
    let count = (obj.width as usize) * (obj.height as usize);
    obj.pixels = vec![0xFFu8; count];
    let size = obj.rle.len();
    let mut i = 0usize;
    let mut x = 0i32;
    let mut y = 0i32;
    while i < size && y < obj.height {
        let mut colour = 0u8;
        let mut run = 1i32;
        let first = obj.rle[i];
        i += 1;
        if first != 0 {
            colour = first;
        } else {
            if i >= size {
                break;
            }
            let flag = obj.rle[i];
            i += 1;
            if flag == 0 {
                x = 0;
                y += 1;
                continue;
            }
            run = i32::from(flag & 0x3F);
            if (flag & 0x40) != 0 {
                if i >= size {
                    break;
                }
                run = (run << 8) | i32::from(obj.rle[i]);
                i += 1;
            }
            if (flag & 0x80) != 0 {
                if i >= size {
                    break;
                }
                colour = obj.rle[i];
                i += 1;
            }
        }
        if x < obj.width {
            let fill = run.min(obj.width - x) as usize;
            let dst = (y as usize) * (obj.width as usize) + (x as usize);
            obj.pixels[dst..dst + fill].fill(colour);
        }
        x = obj.width.min(x + run);
    }
    obj.rle = Vec::new();
}

struct PgsState {
    objects: HashMap<i32, PgsObject>,
    windows: HashMap<i32, Rect>,
    palettes: HashMap<i32, [YCbCrA; 256]>,
    work: i64,
    exhausted: bool,
}

impl PgsState {
    fn new() -> Self {
        Self {
            objects: HashMap::new(),
            windows: HashMap::new(),
            palettes: HashMap::new(),
            work: 0,
            exhausted: false,
        }
    }
    fn spend(&mut self, pixels: i64) -> bool {
        if self.work + pixels > MAX_PIXEL_WORK {
            self.exhausted = true;
            return false;
        }
        self.work += pixels;
        true
    }
    fn reset(&mut self) {
        self.objects.clear();
        self.windows.clear();
        self.palettes.clear();
    }
    fn decoded_bytes(&self) -> i64 {
        self.objects.values().map(|o| o.pixels.len() as i64).sum()
    }
}

fn compose_pgs(pcs: &PgsComposition, state: &mut PgsState, skipped: &mut i32) -> Option<RawCue> {
    if pcs.objects.is_empty() {
        return None;
    }
    let palette_entries = match state.palettes.get(&pcs.palette_id) {
        Some(p) => *p,
        None => {
            *skipped += 1;
            return None;
        }
    };
    let hd = pcs.height > 576;
    let mut table = vec![0u32; 256];
    let mut transparent: i32 = -1;
    for i in 0..256usize {
        table[i] = pgs_colour(palette_entries[i], hd);
        if transparent < 0 && qalpha(table[i]) == 0 {
            transparent = i as i32;
        }
    }
    if transparent < 0 {
        transparent = 0;
        for i in 1..256usize {
            if qalpha(table[i]) < qalpha(table[transparent as usize]) {
                transparent = i as i32;
            }
        }
    }

    struct Placement {
        object_id: i32,
        src_x: i32,
        src_y: i32,
        target: Rect,
    }
    let mut placements = Vec::new();
    let mut bounds = Rect::default();
    let video = Rect { x: 0, y: 0, w: pcs.width, h: pcs.height };

    for co in &pcs.objects {
        let dec_bytes = state.decoded_bytes();
        let Some(obj) = state.objects.get(&co.object_id) else {
            *skipped += 1;
            continue;
        };
        if obj.width <= 0 || obj.height <= 0 {
            *skipped += 1;
            continue;
        }
        let ow = obj.width;
        let oh = obj.height;
        let was_decoded = obj.decoded;
        if !was_decoded {
            if dec_bytes + i64::from(ow) * i64::from(oh) > MAX_OBJECT_BYTES {
                *skipped += 1;
                continue;
            }
            if !state.spend(i64::from(ow) * i64::from(oh)) {
                return None;
            }
            if let Some(m) = state.objects.get_mut(&co.object_id) {
                decode_pgs_object(m);
            }
        }
        let mut source = Rect { x: 0, y: 0, w: ow, h: oh };
        if co.cropped {
            source = source.intersect(co.crop);
        }
        if source.is_empty() {
            continue;
        }
        let target = Rect { x: co.x, y: co.y, w: source.w, h: source.h };
        let mut clip = video;
        if let Some(&win) = state.windows.get(&co.window_id) {
            clip = clip.intersect(win);
        }
        let visible = target.intersect(clip);
        if visible.is_empty() {
            continue;
        }
        placements.push(Placement {
            object_id: co.object_id,
            src_x: source.x + (visible.x - target.x),
            src_y: source.y + (visible.y - target.y),
            target: visible,
        });
        bounds = bounds.unite(visible);
    }

    if bounds.is_empty() {
        return None;
    }
    if !state.spend(2 * i64::from(bounds.w) * i64::from(bounds.h)) {
        return None;
    }
    let mut pixels = vec![transparent as u8; (bounds.w as usize) * (bounds.h as usize)];
    for p in placements {
        let Some(obj) = state.objects.get(&p.object_id) else { continue };
        let stride = obj.width as usize;
        for row in 0..p.target.h {
            let from_off = ((p.src_y + row) as usize) * stride + (p.src_x as usize);
            let to_off = ((p.target.y - bounds.y + row) as usize) * (bounds.w as usize)
                + ((p.target.x - bounds.x) as usize);
            let w = p.target.w as usize;
            let from = &obj.pixels[from_off..from_off + w];
            let to = &mut pixels[to_off..to_off + w];
            for x in 0..w {
                let idx = from[x];
                if qalpha(table[idx as usize]) != 0 {
                    to[x] = idx;
                }
            }
        }
    }

    let mut w = bounds.w;
    let mut h = bounds.h;
    let mut rect = bounds;
    if !trim_to_visible(&mut pixels, &mut w, &mut h, &table, &mut rect) {
        return None;
    }
    Some(RawCue {
        start_ms: 0,
        end_ms: 0,
        rect,
        video_w: pcs.width,
        video_h: pcs.height,
        forced: pcs.objects.iter().any(|o| o.forced),
        palette: table,
        pixels,
    })
}

fn summary_text(skipped: i32, timeline: &Timeline, what: &str) -> String {
    let mut parts = Vec::new();
    if skipped != 0 {
        parts.push(format!("{} {} skipped", skipped, what));
    }
    if timeline.dropped != 0 {
        parts.push(format!("{} empty-duration cue(s) dropped", timeline.dropped));
    }
    if timeline.full {
        parts.push(format!(
            "stopped at the {}-cue / {} MB limit",
            MAX_CUES,
            MAX_IMAGE_BYTES / (1024 * 1024)
        ));
    }
    parts.join("; ")
}

pub fn parse_pgs(sup: &[u8]) -> Result<(Vec<RawCue>, String), String> {
    if sup.is_empty() {
        return Err("empty PGS stream".to_string());
    }
    if sup.len() > MAX_INPUT_BYTES {
        return Err(format!("PGS stream is larger than {} MB", MAX_INPUT_BYTES / (1024 * 1024)));
    }
    if !sup.starts_with(b"PG") {
        return Err("not a PGS stream (no \"PG\" segment header)".to_string());
    }

    let n = sup.len();
    let mut timeline = Timeline::new();
    let mut state = PgsState::new();
    let mut pending: Option<PgsComposition> = None;
    let mut open: Option<RawCue> = None;
    let mut skipped = 0i32;
    let mut display_sets = 0i32;

    let finish = |pcs: &PgsComposition,
                  state: &mut PgsState,
                  timeline: &mut Timeline,
                  open: &mut Option<RawCue>,
                  skipped: &mut i32,
                  display_sets: &mut i32| {
        *display_sets += 1;
        let next_cue = compose_pgs(pcs, state, skipped);
        if let Some(mut cur) = open.take() {
            if let Some(ref c) = next_cue {
                if c.rect == cur.rect && c.palette == cur.palette && c.pixels == cur.pixels {
                    *open = Some(cur);
                    return;
                }
            }
            cur.end_ms = pcs.ms;
            timeline.add(cur);
        }
        if let Some(mut c) = next_cue {
            c.start_ms = pcs.ms;
            *open = Some(c);
        }
    };

    let mut pos = 0usize;
    while pos + 13 <= n && !timeline.full && !state.exhausted {
        if sup[pos] != b'P' || sup[pos + 1] != b'G' {
            skipped += 1;
            if let Some(rel) = sup[pos + 1..].windows(2).position(|w| w == b"PG") {
                pos = pos + 1 + rel;
                continue;
            }
            break;
        }
        let ms = i64::from(be32(&sup[pos + 2..pos + 6])) / 90;
        let seg_type = sup[pos + 10];
        let length = be16(&sup[pos + 11..pos + 13]) as usize;
        let body = pos + 13;
        if body + length > n {
            skipped += 1;
            break;
        }
        let s = &sup[body..body + length];
        match seg_type {
            0x16 => {
                if let Some(p) = pending.take() {
                    finish(&p, &mut state, &mut timeline, &mut open, &mut skipped, &mut display_sets);
                }
                if length < 11 {
                    skipped += 1;
                } else {
                    let mut pcs = PgsComposition {
                        ms,
                        width: be16(&s[0..2]),
                        height: be16(&s[2..4]),
                        state: i32::from(s[7]),
                        palette_id: i32::from(s[9]),
                        objects: Vec::new(),
                    };
                    let count = usize::from(s[10]);
                    let mut p = 11usize;
                    for _ in 0..count.min(MAX_PGS_COMP_OBJECTS) {
                        if p + 8 > length {
                            break;
                        }
                        let object_id = be16(&s[p..p + 2]);
                        let window_id = i32::from(s[p + 2]);
                        let flags = s[p + 3];
                        let cropped = (flags & 0x80) != 0;
                        let forced = (flags & 0x40) != 0;
                        let x = be16(&s[p + 4..p + 6]);
                        let y = be16(&s[p + 6..p + 8]);
                        p += 8;
                        let mut crop = Rect::default();
                        if cropped {
                            if p + 8 > length {
                                break;
                            }
                            crop = Rect {
                                x: be16(&s[p..p + 2]),
                                y: be16(&s[p + 2..p + 4]),
                                w: be16(&s[p + 4..p + 6]),
                                h: be16(&s[p + 6..p + 8]),
                            };
                            p += 8;
                        }
                        pcs.objects.push(PgsCompObj { object_id, window_id, cropped, forced, x, y, crop });
                    }
                    if pcs.width <= 0 || pcs.height <= 0 || pcs.width > MAX_DIMENSION || pcs.height > MAX_DIMENSION {
                        skipped += 1;
                    } else {
                        if (pcs.state & 0x80) != 0 {
                            state.reset();
                        }
                        pending = Some(pcs);
                    }
                }
            }
            0x17 => {
                if length >= 1 {
                    let count = usize::from(s[0]);
                    for i in 0..count {
                        let p = 1 + i * 9;
                        if p + 9 > length {
                            break;
                        }
                        let id = i32::from(s[p]);
                        if state.windows.len() >= MAX_PGS_WINDOWS && !state.windows.contains_key(&id) {
                            break;
                        }
                        state.windows.insert(
                            id,
                            Rect {
                                x: be16(&s[p + 1..p + 3]),
                                y: be16(&s[p + 3..p + 5]),
                                w: be16(&s[p + 5..p + 7]),
                                h: be16(&s[p + 7..p + 9]),
                            },
                        );
                    }
                }
            }
            0x14 => {
                if length >= 2 {
                    let pid = i32::from(s[0]);
                    let pal = state.palettes.entry(pid).or_insert([YCbCrA::default_entry(); 256]);
                    let mut p = 2usize;
                    while p + 5 <= length {
                        let idx = usize::from(s[p]);
                        pal[idx] = YCbCrA { y: s[p + 1], cr: s[p + 2], cb: s[p + 3], a: s[p + 4] };
                        p += 5;
                    }
                }
            }
            0x15 => {
                if length < 4 {
                    skipped += 1;
                } else {
                    let id = be16(&s[0..2]);
                    let seq = s[3];
                    if (seq & 0x80) != 0 {
                        if length < 11 {
                            skipped += 1;
                            state.objects.remove(&id);
                        } else {
                            let width = be16(&s[7..9]);
                            let height = be16(&s[9..11]);
                            if width <= 0 || height <= 0 || width > MAX_DIMENSION || height > MAX_DIMENSION {
                                skipped += 1;
                                state.objects.remove(&id);
                            } else if !state.objects.contains_key(&id) && state.objects.len() >= MAX_PGS_OBJECTS {
                                skipped += 1;
                            } else {
                                state.objects.insert(
                                    id,
                                    PgsObject { width, height, rle: s[11..].to_vec(), decoded: false, pixels: Vec::new() },
                                );
                            }
                        }
                    } else if let Some(found) = state.objects.get_mut(&id) {
                        if found.decoded {
                            skipped += 1;
                        } else {
                            found.rle.extend_from_slice(&s[4..]);
                        }
                    } else {
                        skipped += 1;
                    }

                    if (seq & 0x40) != 0 {
                        let dec_bytes = state.decoded_bytes();
                        if let Some(found) = state.objects.get(&id) {
                            if !found.decoded {
                                let ow = found.width;
                                let oh = found.height;
                                if dec_bytes + i64::from(ow) * i64::from(oh) > MAX_OBJECT_BYTES {
                                    skipped += 1;
                                    state.objects.remove(&id);
                                } else if state.spend(i64::from(ow) * i64::from(oh)) {
                                    if let Some(m) = state.objects.get_mut(&id) {
                                        decode_pgs_object(m);
                                    }
                                }
                            }
                        }
                    }
                }
            }
            0x80 => {
                if let Some(p) = pending.take() {
                    finish(&p, &mut state, &mut timeline, &mut open, &mut skipped, &mut display_sets);
                }
            }
            _ => {}
        }
        pos = body + length;
    }

    if let Some(p) = pending.take() {
        if !timeline.full && !state.exhausted {
            finish(&p, &mut state, &mut timeline, &mut open, &mut skipped, &mut display_sets);
        }
    }
    if let Some(mut cur) = open.take() {
        if !timeline.full {
            cur.end_ms = cur.start_ms + DEFAULT_DURATION_MS;
            timeline.add(cur);
        }
    }
    timeline.out.sort_by_key(|c| c.start_ms);
    if timeline.out.is_empty() {
        return Err(if display_sets != 0 {
            format!("no displayable picture in {} PGS display set(s)", display_sets)
        } else {
            "no complete PGS display set".to_string()
        });
    }
    let mut reason = summary_text(skipped, &timeline, "segment(s)/display set(s)");
    if state.exhausted {
        if !reason.is_empty() {
            reason.push_str("; ");
        }
        reason.push_str("stopped at the pixel-work limit");
    }
    Ok((timeline.out, reason))
}

// ---- VobSub -----------------------------------------------------------------

#[derive(Clone)]
struct IdxEntry {
    ms: i64,
    filepos: i64,
}

#[derive(Clone, Default)]
struct IdxTrack {
    index: i32,
    delay: i64,
    entries: Vec<IdxEntry>,
}

struct IdxFile {
    w: i32,
    h: i32,
    org_x: i32,
    org_y: i32,
    palette: [u32; 16],
    custom_colours: bool,
    custom: [u32; 4],
    custom_transparent: [bool; 4],
    time_offset: i64,
    lang_idx: i32,
    recognised: bool,
    tracks: Vec<IdxTrack>,
}

fn parse_idx_time(mut text: &str) -> Option<i64> {
    text = text.trim();
    let mut negative = false;
    if let Some(rest) = text.strip_prefix('-') {
        negative = true;
        text = rest;
    } else if let Some(rest) = text.strip_prefix('+') {
        text = rest;
    }
    let parts: Vec<&str> = text.split(':').collect();
    if parts.len() != 4 {
        return None;
    }
    let mut v = [0i64; 4];
    for i in 0..4 {
        let p = parts[i].trim().parse::<i64>().ok()?;
        if !(0..=100_000).contains(&p) {
            return None;
        }
        v[i] = p;
    }
    let ms = ((v[0] * 60 + v[1]) * 60 + v[2]) * 1000 + v[3];
    Some(if negative { -ms } else { ms })
}

fn find_ascii_ci(haystack: &str, needle_lower: &str) -> Option<usize> {
    let hb = haystack.as_bytes();
    let nb = needle_lower.as_bytes();
    if nb.is_empty() || hb.len() < nb.len() {
        return None;
    }
    hb.windows(nb.len()).position(|w| w.eq_ignore_ascii_case(nb))
}

fn parse_idx(bytes: &[u8]) -> IdxFile {
    const FALLBACK: [u32; 16] = [
        0x000000, 0x0000ff, 0x00ff00, 0xff0000, 0xffff00, 0xff00ff, 0x00ffff, 0xffffff,
        0x808000, 0x8080ff, 0x800080, 0x80ff80, 0x008080, 0xff8080, 0x555555, 0xaaaaaa,
    ];
    let mut palette = [0u32; 16];
    for i in 0..16 {
        palette[i] = 0xff000000 | FALLBACK[i];
    }
    let mut idx = IdxFile {
        w: 720,
        h: 480,
        org_x: 0,
        org_y: 0,
        palette,
        custom_colours: false,
        custom: [0; 4],
        custom_transparent: [false; 4],
        time_offset: 0,
        lang_idx: -1,
        recognised: false,
        tracks: Vec::new(),
    };
    let mut entries = 0usize;
    let text = String::from_utf8_lossy(bytes);
    for raw_line in text.split('\n') {
        let line = raw_line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let Some(colon) = line.find(':') else { continue };
        if colon == 0 {
            continue;
        }
        let key = line[..colon].trim().to_ascii_lowercase();
        let value = line[colon + 1..].trim();
        match key.as_str() {
            "size" => {
                let wh: Vec<&str> = value.split('x').collect();
                if wh.len() == 2 {
                    if let (Ok(w), Ok(h)) = (wh[0].trim().parse::<i32>(), wh[1].trim().parse::<i32>()) {
                        if w > 0 && h > 0 && w <= MAX_DIMENSION && h <= MAX_DIMENSION {
                            idx.w = w;
                            idx.h = h;
                            idx.recognised = true;
                        }
                    }
                }
            }
            "org" => {
                let xy: Vec<&str> = value.split(',').collect();
                if xy.len() == 2 {
                    if let (Ok(x), Ok(y)) = (xy[0].trim().parse::<i32>(), xy[1].trim().parse::<i32>()) {
                        if x.abs() <= MAX_DIMENSION && y.abs() <= MAX_DIMENSION {
                            idx.org_x = x;
                            idx.org_y = y;
                        }
                    }
                }
            }
            "palette" => {
                for (i, c_str) in value.split(',').take(16).enumerate() {
                    if let Ok(c) = u32::from_str_radix(c_str.trim(), 16) {
                        idx.palette[i] = 0xff000000 | (c & 0xffffff);
                    }
                }
                idx.recognised = true;
            }
            "custom colors" => {
                idx.custom_colours = value.len() >= 2 && value[..2].eq_ignore_ascii_case("on");
                if let Some(t) = find_ascii_ci(value, "tridx:") {
                    let bits = value[t + 6..].trim();
                    for (i, ch) in bits.chars().take(4).enumerate() {
                        idx.custom_transparent[i] = ch == '1';
                    }
                }
                if let Some(c) = find_ascii_ci(value, "colors:") {
                    for (i, c_str) in value[c + 7..].split(',').take(4).enumerate() {
                        if let Ok(v) = u32::from_str_radix(c_str.trim(), 16) {
                            idx.custom[i] = 0xff000000 | (v & 0xffffff);
                        }
                    }
                }
            }
            "time offset" => {
                if let Ok(v) = value.parse::<i64>() {
                    if v.abs() <= 24 * 3600 * 1000 {
                        idx.time_offset = v;
                    }
                }
            }
            "langidx" => {
                if let Ok(v) = value.parse::<i32>() {
                    idx.lang_idx = v;
                }
            }
            "id" => {
                if idx.tracks.len() < 64 {
                    let mut track = IdxTrack { index: -1, delay: 0, entries: Vec::new() };
                    if let Some(at) = find_ascii_ci(value, "index:") {
                        if let Ok(v) = value[at + 6..].trim().parse::<i32>() {
                            if (0..32).contains(&v) {
                                track.index = v;
                            }
                        }
                    }
                    if track.index < 0 {
                        track.index = (idx.tracks.len() as i32) & 0x1F;
                    }
                    idx.tracks.push(track);
                    idx.recognised = true;
                }
            }
            "delay" => {
                if let (Some(last), Some(d)) = (idx.tracks.last_mut(), parse_idx_time(value)) {
                    if d.abs() <= 24 * 3600 * 1000 {
                        last.delay += d;
                    }
                }
            }
            "timestamp" => {
                if entries < MAX_IDX_ENTRIES {
                    let time_part = value.split(',').next().unwrap_or("");
                    if let (Some(at), Some(ms)) = (find_ascii_ci(value, "filepos:"), parse_idx_time(time_part)) {
                        if let Ok(pos) = i64::from_str_radix(value[at + 8..].trim(), 16) {
                            if pos >= 0 {
                                if idx.tracks.is_empty() {
                                    idx.tracks.push(IdxTrack { index: 0, delay: 0, entries: Vec::new() });
                                }
                                let track = idx.tracks.last_mut().unwrap();
                                track.entries.push(IdxEntry {
                                    ms: ms + track.delay + idx.time_offset,
                                    filepos: pos,
                                });
                                entries += 1;
                                idx.recognised = true;
                            }
                        }
                    }
                }
            }
            _ => {}
        }
    }
    idx
}

fn read_pts(p: &[u8]) -> i64 {
    (i64::from((p[0] >> 1) & 7) << 30)
        | (i64::from(p[1]) << 22)
        | (i64::from(p[2] >> 1) << 15)
        | (i64::from(p[3]) << 7)
        | i64::from(p[4] >> 1)
}

fn walk_program_stream<F>(stream: &[u8], mut pos: usize, max_packets: i32, mut visit: F)
where
    F: FnMut(i32, bool, i64, &[u8]) -> bool,
{
    let n = stream.len();
    let mut packets = 0i32;
    while pos + 4 <= n {
        if max_packets >= 0 && packets >= max_packets {
            return;
        }
        packets += 1;
        if !(stream[pos] == 0 && stream[pos + 1] == 0 && stream[pos + 2] == 1) {
            let win_len = (n - pos - 1).min(MAX_RESYNC_BYTES);
            let slice = &stream[pos + 1..pos + 1 + win_len];
            if let Some(rel) = slice.windows(3).position(|w| w == [0, 0, 1]) {
                pos += 1 + rel;
                continue;
            }
            return;
        }
        let code = stream[pos + 3];
        if code == 0xBA {
            if pos + 5 > n {
                return;
            }
            if (stream[pos + 4] & 0xC0) == 0x40 {
                if pos + 14 > n {
                    return;
                }
                pos += 14 + usize::from(stream[pos + 13] & 7);
            } else {
                pos += 12;
            }
            continue;
        }
        if code == 0xB9 {
            return;
        }
        if code < 0xBB {
            pos += 4;
            continue;
        }
        if pos + 6 > n {
            return;
        }
        let end = n.min(pos + 6 + (be16(&stream[pos + 4..pos + 6]) as usize));
        if code == 0xBD {
            let mut q = pos + 6;
            let mut has_pts = false;
            let mut pts = 0i64;
            let mut ok = true;
            if q < end && (stream[q] & 0xC0) == 0x80 {
                if q + 3 > end {
                    ok = false;
                } else {
                    let flags = stream[q + 1];
                    let length = usize::from(stream[q + 2]);
                    if q + 3 + length > end {
                        ok = false;
                    } else {
                        if (flags & 0x80) != 0 && length >= 5 {
                            pts = read_pts(&stream[q + 3..q + 8]);
                            has_pts = true;
                        }
                        q += 3 + length;
                    }
                }
            } else {
                let mut stuffing = 0;
                while q < end && stream[q] == 0xFF && stuffing < 16 {
                    stuffing += 1;
                    q += 1;
                }
                if q < end && (stream[q] & 0xC0) == 0x40 {
                    q += 2;
                }
                if q < end {
                    if (stream[q] & 0xF0) == 0x20 {
                        if q + 5 <= end {
                            pts = read_pts(&stream[q..q + 5]);
                            has_pts = true;
                        }
                        q += 5;
                    } else if (stream[q] & 0xF0) == 0x30 {
                        if q + 10 <= end {
                            pts = read_pts(&stream[q..q + 5]);
                            has_pts = true;
                        }
                        q += 10;
                    } else if stream[q] == 0x0F {
                        q += 1;
                    }
                }
            }
            if ok && q < end && !visit(i32::from(stream[q]), has_pts, pts, &stream[q + 1..end]) {
                return;
            }
        }
        pos = end;
    }
}

fn read_spu_at(sub: &[u8], pos: usize, id: i32) -> Vec<u8> {
    let mut spu = Vec::new();
    let mut expected: i32 = -1;
    walk_program_stream(sub, pos, MAX_SPU_PACKETS, |stream, has_pts, _, data| {
        if stream != id {
            return true;
        }
        if !spu.is_empty() && has_pts {
            return false;
        }
        spu.extend_from_slice(data);
        if expected < 0 && spu.len() >= 2 {
            expected = be16(&spu[0..2]);
        }
        !(expected >= 0 && (spu.len() as i32) >= expected)
    });
    if expected < 4 || (spu.len() as i32) < expected {
        return Vec::new();
    }
    spu.truncate(expected as usize);
    spu
}

struct NibbleReader<'a> {
    data: &'a [u8],
    nibble: usize,
}

impl<'a> NibbleReader<'a> {
    fn next(&mut self) -> Option<i32> {
        if self.nibble >= self.data.len() * 2 {
            return None;
        }
        let b = self.data[self.nibble >> 1];
        let v = if (self.nibble & 1) != 0 { b & 0x0F } else { b >> 4 };
        self.nibble += 1;
        Some(i32::from(v))
    }
    fn align(&mut self) {
        self.nibble = (self.nibble + 1) & !1usize;
    }
}

struct SpuPicture {
    palette: Vec<u32>,
    pixels: Vec<u8>,
    rect: Rect,
    start_ms: i64,
    stop_ms: i64,
    forced: bool,
}

fn decode_spu(spu: &[u8], idx: &IdxFile, work: &mut i64) -> Option<SpuPicture> {
    let size = spu.len();
    if size < 4 {
        return None;
    }
    let control_start = be16(&spu[2..4]) as usize;
    if control_start < 4 || control_start + 4 > size {
        return None;
    }
    let mut colours = [0usize, 1, 2, 3];
    let mut alpha = [0u32, 15, 15, 15];
    let (mut x1, mut x2, mut y1, mut y2) = (-1i32, -1i32, -1i32, -1i32);
    let (mut field_top, mut field_bottom) = (-1i32, -1i32);
    let mut started = false;
    let mut start_ms = 0i64;
    let mut stop_ms = -1i64;
    let mut forced = false;
    let mut at = control_start;

    for _ in 0..64 {
        if at + 4 > size {
            break;
        }
        let raw_date = be16(&spu[at..at + 2]);
        let ms = i64::from(raw_date) * 1024 / 90;
        let next = be16(&spu[at + 2..at + 4]) as usize;
        let mut p = at + 4;
        let mut done = false;
        while p < size && !done {
            let cmd = spu[p];
            p += 1;
            match cmd {
                0x00 => {
                    forced = true;
                    if !started {
                        start_ms = ms;
                        started = true;
                    }
                }
                0x01 => {
                    if !started {
                        start_ms = ms;
                        started = true;
                    }
                }
                0x02 => {
                    if stop_ms < 0 && raw_date != 0xFFFF {
                        stop_ms = ms;
                    }
                }
                0x03 => {
                    if p + 2 > size {
                        done = true;
                    } else {
                        colours[3] = usize::from(spu[p] >> 4);
                        colours[2] = usize::from(spu[p] & 15);
                        colours[1] = usize::from(spu[p + 1] >> 4);
                        colours[0] = usize::from(spu[p + 1] & 15);
                        p += 2;
                    }
                }
                0x04 => {
                    if p + 2 > size {
                        done = true;
                    } else {
                        alpha[3] = u32::from(spu[p] >> 4);
                        alpha[2] = u32::from(spu[p] & 15);
                        alpha[1] = u32::from(spu[p + 1] >> 4);
                        alpha[0] = u32::from(spu[p + 1] & 15);
                        p += 2;
                    }
                }
                0x05 => {
                    if p + 6 > size {
                        done = true;
                    } else {
                        x1 = (i32::from(spu[p]) << 4) | i32::from(spu[p + 1] >> 4);
                        x2 = (i32::from(spu[p + 1] & 15) << 8) | i32::from(spu[p + 2]);
                        y1 = (i32::from(spu[p + 3]) << 4) | i32::from(spu[p + 4] >> 4);
                        y2 = (i32::from(spu[p + 4] & 15) << 8) | i32::from(spu[p + 5]);
                        p += 6;
                    }
                }
                0x06 => {
                    if p + 4 > size {
                        done = true;
                    } else {
                        field_top = be16(&spu[p..p + 2]);
                        field_bottom = be16(&spu[p + 2..p + 4]);
                        p += 4;
                    }
                }
                0x07 => {
                    if p + 2 > size {
                        done = true;
                    } else {
                        let len = be16(&spu[p..p + 2]);
                        if len < 2 {
                            done = true;
                        } else {
                            p += len as usize;
                        }
                    }
                }
                _ => done = true,
            }
        }
        if next <= at || next + 4 > size {
            break;
        }
        at = next;
    }

    if x1 < 0 || field_top < 0 || field_bottom < 0 || x2 < x1 || y2 < y1 {
        return None;
    }
    if stop_ms >= 0 && stop_ms <= start_ms {
        stop_ms = -1;
    }
    let mut width = x2 - x1 + 1;
    let mut height = y2 - y1 + 1;
    if width > MAX_DIMENSION || height > MAX_DIMENSION {
        return None;
    }
    if *work + 2 * i64::from(width) * i64::from(height) > MAX_PIXEL_WORK {
        *work = MAX_PIXEL_WORK + 1;
        return None;
    }
    *work += 2 * i64::from(width) * i64::from(height);

    let mut table = vec![0u32; 4];
    for i in 0..4 {
        let rgb = if idx.custom_colours { idx.custom[i] } else { idx.palette[colours[i]] };
        let a = if idx.custom_colours && idx.custom_transparent[i] { 0 } else { alpha[i] * 17 };
        table[i] = (rgb & 0x00ff_ffff) | (a << 24);
    }

    let mut pixels = vec![0u8; (width as usize) * (height as usize)];
    for field in 0..2i32 {
        let offset = if field != 0 { field_bottom } else { field_top } as usize;
        if offset >= size {
            continue;
        }
        let mut reader = NibbleReader { data: spu, nibble: offset * 2 };
        let mut ok = true;
        let mut y = field;
        while y < height && ok {
            let row_off = (y as usize) * (width as usize);
            let mut x = 0i32;
            while x < width {
                let Some(mut v) = reader.next() else { ok = false; break; };
                if v < 0x4 {
                    let Some(n1) = reader.next() else { ok = false; break; };
                    v = (v << 4) | n1;
                    if v < 0x10 {
                        let Some(n2) = reader.next() else { ok = false; break; };
                        v = (v << 4) | n2;
                        if v < 0x40 {
                            let Some(n3) = reader.next() else { ok = false; break; };
                            v = (v << 4) | n3;
                        }
                    }
                }
                let mut run = v >> 2;
                if run == 0 {
                    run = width - x;
                }
                let fill = run.min(width - x) as usize;
                pixels[row_off + (x as usize)..row_off + (x as usize) + fill].fill((v & 3) as u8);
                x += fill as i32;
            }
            reader.align();
            y += 2;
        }
    }

    let mut rect = Rect { x: x1 + idx.org_x, y: y1 + idx.org_y, w: width, h: height };
    if !trim_to_visible(&mut pixels, &mut width, &mut height, &table, &mut rect) {
        return None;
    }
    Some(SpuPicture { palette: table, pixels, rect, start_ms, stop_ms, forced })
}

pub fn parse_vobsub(idx_bytes: &[u8], sub: &[u8], track: i32) -> Result<(Vec<RawCue>, String), String> {
    if idx_bytes.len() > MAX_INPUT_BYTES || sub.len() > MAX_INPUT_BYTES {
        return Err(format!("VobSub input is larger than {} MB", MAX_INPUT_BYTES / (1024 * 1024)));
    }
    if sub.is_empty() {
        return Err("empty VobSub .sub".to_string());
    }
    let idx = parse_idx(idx_bytes);
    if !idx.recognised {
        return Err("not a VobSub .idx (no size, palette, id or timestamp line)".to_string());
    }

    let wanted = if track >= 0 { track } else { idx.lang_idx.max(0) };
    let mut chosen: Option<&IdxTrack> = idx.tracks.iter().find(|t| t.index == wanted);
    if chosen.is_none() && (wanted as usize) < idx.tracks.len() {
        chosen = Some(&idx.tracks[wanted as usize]);
    }
    if chosen.is_none() && track < 0 && !idx.tracks.is_empty() {
        chosen = idx.tracks.first();
    }
    if chosen.is_none() && !idx.tracks.is_empty() {
        return Err(format!("VobSub has no track {} ({} in the .idx)", track, idx.tracks.len()));
    }
    let stream_id = 0x20 + ((chosen.map_or(wanted.max(0), |t| t.index)) & 0x1F);

    let mut raws: Vec<(i64, Vec<u8>)> = Vec::new();
    let mut skipped = 0i32;

    if let Some(tr) = chosen.filter(|t| !t.entries.is_empty()) {
        for e in &tr.entries {
            if raws.len() >= MAX_CUES || (skipped as usize) >= MAX_CUES {
                break;
            }
            if e.filepos < 0 || (e.filepos as usize) >= sub.len() {
                skipped += 1;
                continue;
            }
            let spu = read_spu_at(sub, e.filepos as usize, stream_id);
            if spu.is_empty() {
                skipped += 1;
                continue;
            }
            raws.push((e.ms, spu));
        }
    } else {
        let mut spu: Vec<u8> = Vec::new();
        let mut pts = 0i64;
        let mut expected: i32 = -1;
        let offset = idx.time_offset + chosen.map_or(0, |t| t.delay);
        walk_program_stream(sub, 0, -1, |stream, has_pts, packet_pts, data| {
            if stream != stream_id {
                return true;
            }
            if has_pts {
                if !spu.is_empty() {
                    skipped += 1;
                }
                spu.clear();
                expected = -1;
                pts = packet_pts;
            } else if spu.is_empty() {
                return true;
            }
            spu.extend_from_slice(data);
            if expected < 0 && spu.len() >= 2 {
                expected = be16(&spu[0..2]);
            }
            if expected >= 0 && (spu.len() as i32) >= expected {
                if expected >= 4 {
                    spu.truncate(expected as usize);
                    raws.push((pts / 90 + offset, std::mem::take(&mut spu)));
                } else {
                    skipped += 1;
                    spu.clear();
                }
                expected = -1;
            }
            raws.len() < MAX_CUES
        });
    }

    let raw_count = raws.len();
    let mut cues: Vec<RawCue> = Vec::new();
    let mut open_ended: Vec<bool> = Vec::new();
    let mut bytes = 0i64;
    let mut work = 0i64;
    let mut full = false;

    for (raw_ms, raw_spu) in raws {
        let Some(pic) = decode_spu(&raw_spu, &idx, &mut work) else {
            if work > MAX_PIXEL_WORK {
                break;
            }
            skipped += 1;
            continue;
        };
        let end_ms = if pic.stop_ms >= 0 { raw_ms + pic.stop_ms } else { -1 };
        let cue = RawCue {
            start_ms: raw_ms + pic.start_ms,
            end_ms,
            rect: pic.rect,
            video_w: idx.w,
            video_h: idx.h,
            forced: pic.forced,
            palette: pic.palette,
            pixels: pic.pixels,
        };
        let img_bytes = cue.image_bytes_qt();
        if cues.len() >= MAX_CUES || bytes + img_bytes > MAX_IMAGE_BYTES {
            full = true;
            break;
        }
        bytes += img_bytes;
        open_ended.push(cue.end_ms < 0);
        cues.push(cue);
    }

    let mut order: Vec<usize> = (0..cues.len()).collect();
    order.sort_by_key(|&i| cues[i].start_ms);
    for k in 0..order.len() {
        let cur_idx = order[k];
        let next_start = if k + 1 < order.len() { cues[order[k + 1]].start_ms } else { -1 };
        let start = cues[cur_idx].start_ms;
        if open_ended[cur_idx] {
            let mut end = start + DEFAULT_DURATION_MS;
            if next_start > start && next_start - start <= MAX_GAP_AS_DURATION_MS {
                end = next_start;
            }
            cues[cur_idx].end_ms = end;
        } else if next_start > start && cues[cur_idx].end_ms > next_start {
            cues[cur_idx].end_ms = next_start;
        }
    }

    let mut timeline = Timeline::new();
    let mut ordered: Vec<Option<RawCue>> = cues.into_iter().map(Some).collect();
    for i in order {
        if let Some(c) = ordered[i].take() {
            timeline.add(c);
        }
    }
    timeline.full = timeline.full || full;
    timeline.out.sort_by_key(|c| c.start_ms);
    if timeline.out.is_empty() {
        return Err(if raw_count == 0 {
            format!("no subpicture for stream 0x{:02x} in the .sub", stream_id)
        } else {
            format!("none of {} subpicture(s) decoded", raw_count)
        });
    }
    let mut reason = summary_text(skipped, &timeline, "subpicture(s)");
    if work > MAX_PIXEL_WORK {
        if !reason.is_empty() {
            reason.push_str("; ");
        }
        reason.push_str("stopped at the pixel-work limit");
    }
    Ok((timeline.out, reason))
}

pub fn encode_cues(cues: &[RawCue]) -> Vec<u8> {
    let mut out = Vec::with_capacity(4 + cues.len() * 64);
    out.extend_from_slice(&(cues.len() as u32).to_le_bytes());
    for c in cues {
        out.extend_from_slice(&c.start_ms.to_le_bytes());
        out.extend_from_slice(&c.end_ms.to_le_bytes());
        out.extend_from_slice(&c.rect.x.to_le_bytes());
        out.extend_from_slice(&c.rect.y.to_le_bytes());
        out.extend_from_slice(&c.rect.w.to_le_bytes());
        out.extend_from_slice(&c.rect.h.to_le_bytes());
        out.extend_from_slice(&c.video_w.to_le_bytes());
        out.extend_from_slice(&c.video_h.to_le_bytes());
        out.push(u8::from(c.forced));
        out.extend_from_slice(&(c.palette.len() as u16).to_le_bytes());
        for &entry in &c.palette {
            out.extend_from_slice(&entry.to_le_bytes());
        }
        out.extend_from_slice(&(c.pixels.len() as u32).to_le_bytes());
        out.extend_from_slice(&c.pixels);
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pgs_rejects_empty_and_non_pg() {
        assert_eq!(parse_pgs(&[]).err().as_deref(), Some("empty PGS stream"));
        assert!(parse_pgs(b"NOTPGS").unwrap_err().contains("PG"));
    }

    #[test]
    fn vobsub_rejects_empty_sub() {
        assert_eq!(parse_vobsub(b"size: 720x480\n", &[], -1).err().as_deref(), Some("empty VobSub .sub"));
    }
}
