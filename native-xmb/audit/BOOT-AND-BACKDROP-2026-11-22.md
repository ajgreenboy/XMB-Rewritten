# Boot screens and focused-game backdrop timing (2026-11-22)

Evidence: the RPCS3 recording `rpcs3_based_full_xmb_psn_now_available.mp4` (called R in VIDEO-GAP-ANALYSIS-2026-11-22.md) and the
older `m2` recording, resampled with `xmb-reference-frames.exe` (100 ms and 33 ms). Video frames are about 30 fps, so every time
carries +/- 33 ms. Regions were averaged with a small System.Drawing script; no recording frame is committed.

## A. Boot

### What was already right (the gap analysis called it missing)

The cold boot already shipped the photosensitive-epilepsy notice (`Coldboot::renderNotice`, the firmware's own
`msg_health_warning_start_scee` string from `sysconf_plugin`, four lines, left-aligned at x 322.5 / 1920, cap height 18, 5 s hold,
200 ms close). BEHAVIOUR-GAP-AUDIT E19 and FIDELITY-BEHAVIOUR B3 are stale on this. Position, size, wrapping and line breaks
match the recording's frame at 63.2 s to within a pixel in a zoomed crop.

### Measured on R (boot origin fixed by the logo's fade: ghost at 56.6 s and 59.5 s = our 2.1 s and 5.0 s, so t = R - 54.45 s)

| Item | Recording | Ours before | Ours now |
|---|---|---|---|
| Notice appears | jumps on at 60.6-60.7 s = t 6.15-6.25 s (0.65-0.75 s after ShowGUI 5.5 s) | 5.5 s | 6.15 s (`Coldboot::noticeDelay`) |
| Notice closes | text gone 65.5-65.7 s = t 11.05-11.25 s, 200 ms fade | 10.5-10.7 s | 11.15-11.35 s |
| Notice shadow | soft dark drop shadow under every glyph, about 2 px down/right at 1080p | none | 3x3 stamp blur, 2 px offset, peak about 35 % (blur and strength are a fit) |
| Dust particles during the notice | none (first specks on the first XMB frame, 65.9 s) | visible from 4 s | off while the intro/notice hold the GUI (`introHoldsGui`) |
| PS3 logo, background ramp, ribbon | logo fades in from 56.6 s, gone by 59.5 s; terracotta ramp | same timeline | unchanged (traced earlier, agrees) |

Side-by-side sheets (reference left, ours right, same scale, mapping t = R - 54.45 s): `C:\dev\out\boot-backdrop\boot-sbs-1.png`,
`boot-sbs-2.png`, notice crop `notice-crop-new.png`.

### The "Sony Computer Entertainment" wordmark stage (added, default on; `XMB_BOOT_WORDMARK=0` turns it off)

The first pass left it out because the 4.93 reference (R) has none. The owner asked for it following the older recording m2, so
it is now a stage of our cold boot. Measured on the m2 frames (1280x720, 200 ms, 11.0-19.5 s; no frame is committed):

| Item | m2 measurement |
|---|---|
| Ribbon first visible | about 11.1 s (peak grey 3 at 11.2 s, 16 at 11.6 s, then about +50 grey levels per second) |
| Text first visible / full | 11.67 s / 12.39 s, linear (mean grey of 79 stable ink pixels 41, 101, 162, 203 at 11.8 .. 12.4 s, flat after) |
| Hold, then out | full until 14.12 s; linear out to 14.84 s (98 %, 88 %, 63 %, 37 %, 6 % at 14.0 .. 14.8 s, measured against the ribbon behind) |
| Background | black until about 11.75 s, then a near-linear ramp of the corner mean (3,0,1) at 11.8 s, (42,19,11) at 13.0 s, (94,55,37) at 14.8 s, (125,74,53) from 17.0 s on (time of day 09:38 on the XMB clock) |
| XMB | icons appear at about 18.9 s (frame difference drops to 0.3 at 18.8 s, jumps at 19.0 s); there is no PS3 logo and no notice |
| Ink extent | x 721..1082, y 350..369 of 1280x720: Sony 721-779, Computer 784-904, Entertainment 911-1082 (50 % threshold, matches); cap top 350, x-height top 355, baseline 366, descenders to 369 |
| Design space (x 1.5) | words at x 1081.5 / 1176 / 1366.5, ink widths 88.5 / 181.5 / 258, baseline 549, cap height 24, x-height about 16.5 |
| Look | wide humanist/geometric sans, regular-medium weight (stems about 2 px at 720p), very tight word gaps (about 4-6 px), no shadow; ink over black reads 233/212/209 at full opacity |

Asset catalogue: `tex_scelogo` in `custom_render_plugin` is "Sony Interactive Entertainment", 497x34 ink, held at alpha 0 by
`anim_coldboot2`; plain-text search of dev_flash for "Sony Computer" finds only the PS1 BIOS image and the OSS notice, no UI string
(the RCO text and texture tables were not searched by decoding every container; the texture was not re-rendered in this pass, it
rests on the previous finding). Its text and typeface do not match the recording, so it is not reused: the three words are drawn
with Verdana (closest installed wide sans, tried against Tahoma, Segoe UI, Bahnschrift, Arial, Trebuchet, Candara, Corbel,
Franklin Gothic) when installed, else the firmware UI font. Each word is stretched horizontally to its measured ink width, which
fixes the tracking and word gaps. Code: `src/boot_wordmark.h` (curves, schedule, placement, switch), `Coldboot::renderWordmark`.

Placement in our sequence: m2 has the wordmark where 4.93 has the PS3 logo (the logo sits in the same right-of-centre spot), and
the two cannot both stand there. The rule: every existing stage keeps its time (logo from about 1.5 s with its light, first visible
about 2.1 s, ShowGUI 5.5 s, notice 6.15-11.35 s, background and ribbon untouched), and the wordmark takes the time before the
logo: start 0.2 s, linear in 0.72 s, hold 0.46 s, linear out 0.72 s, gone at 2.1 s. The two fades are the measured ones; only the
hold (1.73 s on m2) gives way. Skip, `--no-intro`, `--script`, `--background-clock`, kiosk and safe mode never create the intro, so
they and every gate that does not boot are unchanged.

Side-by-side sheets (reference left, ours right, both at 960x540, paired at equal wordmark opacity: m2 11.8 / 12.2 / 13.0 / 14.4 /
14.8 s against ours 0.34 / 0.73 / 1.20 / 1.66 / 2.05 s): `C:\dev\out\sce-wordmark\sbs-wordmark-full.png` and
`sbs-wordmark-crop.png` (text band, 2.5x).

### The ramp under the wordmark (background and ribbon, added with the stage; follows `XMB_BOOT_WORDMARK`)

The first wordmark pass left the 4.93 background fade in place, so the wordmark stood on black with a bright grey ribbon. m2 shows
something else, measured on all its frames (1280x720, 200 ms, 4x3 cell means; no frame is committed):

| Item | m2 measurement |
|---|---|
| Background | black until about 11.55 s, then a near-linear brightening of the whole frame, in step with its plateau: mean luma (top and bottom rows) over plateau 0.02 / 0.12 / 0.27 / 0.43 / 0.54 / 0.72 / 0.82 / 0.92 / 0.97 / 1.00 at 11.8 / 12.4 / 13.0 / 13.6 / 14.0 / 14.8 / 15.4 / 16.0 / 17.0 / 17.6 s |
| Channels | red first, green and blue later: at 14.0 s red 0.58, green 0.51, blue 0.42 of their plateaus; at 12.4 s 0.18 / 0.06 / 0.03 (the dark ramp is brown, not grey) |
| Whole frame | top-left (94,55,37) at 14.8 s, plateau (130,75,55) from 17.0 s; lower-right glow (171,88,68) at the plateau, reaching it with the rest; the glow is the brightest cell from the first visible frame |
| Ribbon | first visible about 11.1 s, no flash; peak (top 1 % of pixels, left half) 10 / 30 / 51 / 95 / 151 / 194 / 217 / 224 grey-red at 11.4 / 11.8 / 12.2 / 13.0 / 13.8 / 14.6 / 15.4 / 15.8 s (green channel 1 / 16 / 33 / 65 / 106 / 137 / 150 / 155); after subtracting the background under it the ribbon is neutral grey, so its brown look is the background's |

Implementation (`src/boot_wordmark.h` `backgroundAlpha`, `ribbonGain`; `View::paintGL`, `Ribbon::setBootGain`; on only while the
wordmark stage is):

- One linear clock from our boot to m2's: our 0.2 s is m2 11.67 s (the text's first visible moment), our 2.1 s is m2 14.84 s
  (its last trace), factor 3.17 / 1.9 = 1.67 m2 seconds per second. The ramp is stretched over the whole stage; only the text's hold gives way.
- Background: the shader's `_Alpha` follows a knot table (m2 11.4 .. 18.2 s). Our alpha is scene radiance before the tone map and
  the ribbon's glare adds to the background cells, so the table is FITTED, not read: iterated until our own capture with the clock
  pinned to the recording's time of day (`XMB_BOOT_CLOCK=2026-11-22T09:38:00`, measurement-only) matched m2's luma ratio within
  about 0.03 at every sample from 0.3 to 3.0 s (red/green/blue within 0.02 / 0.03 / 0.07: our blue rises a little earlier than m2's).
  It reaches 1 (the existing background, unscaled) at 3.4 s, ahead of the logo's end and far ahead of ShowGUI (5.5 s).
- Ribbon: a gain on the ribbon's additive composite, 0 at the first frame (this is the flash: coldboot1's preset is bright and dims
  as it gives way to coldboot2, so the native ribbon is at its brightest in the first 0.3 s; it was not a bug in the cloth), 0.14 at
  0.2 s, 0.60 at 1.0 s, 1 from 1.6 s, fitted so the ribbon's peak (green, top 1 %) follows m2's within about 10 %.
- Hand-off: no existing stage moves. The logo (first visible 2.1 s), ShowGUI (5.5 s), the notice (6.15 .. 11.35 s) and the preset
  weights (coldboot1 to coldboot2 to clock) keep their times. The ramp continues behind the logo, so the logo now sits on dark
  brown (alpha 0.72 at 2.1 s, 1 from 3.4 s) instead of black. DELIBERATE DEPARTURE from the 4.93-only boot, where the background
  is smoothstep(t / 7.5 s) (0.19 at 2.1 s, 0.82 at 5.5 s). The notice's black plate is the full background from 3.4 s (it was
  0.82 .. 1.0 over 5.5 .. 7.5 s). No XMB frame changes: the first one is at 11.35 s or later, when both ramps are 1.
- `XMB_BOOT_WORDMARK=0` restores the previous boot exactly: smoothstep alpha, ribbon gain 1 (the shader multiplies by 1.0), no
  wordmark. Skip, `--no-intro`, `--script`, `--background-clock`, kiosk and safe mode never create the intro and are untouched.

Capture method (reuse): `XMB_INTRO_GRAB=<dir>@t1,t2,...` (intro clock, one PNG per time, windowed run), `XMB_BOOT_CLOCK` as above,
`--dev-flash`, `--vsh-modules`, `--data-dir <scratch>`; reference frames from `xmb-reference-frames.exe`; sheets with the earlier
System.Drawing script (reference left, ours right, both 960x540). Sheets: `C:\dev\out\boot-ramp\sbs-ramp-full.png` (m2 11.8 / 12.2 /
13.0 / 14.4 / 14.8 s against ours 0.34 / 0.73 / 1.20 / 1.66 / 2.05 s) and `sbs-ramp-handoff.png` (m2 15.6 / 16.6 s against 2.5 / 3.0 s,
ours with the logo).
Unit tests: `tests/boot_wordmark_tests.cpp` (clock, alpha 0 before / 1 after / monotone, ribbon gain 0 at the first frame / 1 from
1.6 s / monotone, knot interpolation).

Remaining differences under the ramp: the plateau colour and the glow's position follow our clock and month (m2's was a different
month at 09:38, our plateau is 14 % brighter in red); the top-centre glow of November is stronger than m2's top-left one; late ribbon
(after 2.5 s) peaks about 5 % dimmer in green than m2's; the ramp is fitted at 21 to 33 samples per run at 126 fps, with 0.01 s of
timing jitter.

Differences that remain (not fixed):

- Background and ribbon under the wordmark: fixed by the ramp section above (was: black background, grey ribbon, bright flash in the first 0.3 s).
- The hold is 0.46 s against 1.73 s (the stage is 1.9 s long instead of 3.17 s).
- Glyph shapes are Verdana's; the original wordmark's letterforms (a wider, rounder face) differ, most visibly the `y` and `C`.
- Verdana is a Windows font; another machine falls back to the firmware UI font and will look different.
- Our captures are at 960x540 (windowed run), the reference is 1280x720 JPEG-compressed video; ink edges are compared at 960x540.
- Ours grabs pass the stage time as the intro clock, which runs from the first presented frame; m2's origin was placed by the
  text's own appearance, not by an independent boot origin.

### Remaining differences (not fixed)

- (Fixed with the wordmark stage on, by the ribbon gain; still true with XMB_BOOT_WORDMARK=0.) Ours shows a brighter ribbon in the first ~0.3 s (t 0-0.3 s) than the recording's first visible frames; the recording is black
  until 55.0 s, so the first 0.6 s of our timeline have no counterpart. Cause not investigated.
- Background colour differs by time of day (the recording was 13:47, our run 18:2x); `--background-clock` implies no intro so it
  cannot be pinned for boot captures.
- Our XMB shows first-run rows on a scratch data dir right after the notice; the recording's bar appears later.
- The notice delay is measured on RPCS3; real hardware may differ.

## B. PIC1 backdrop (focused game row)

Measured on GTA V (83.5-85.1 s) and Black Ops II (110.4-111.8 s), mean RGB of a PIC1 region at 33 ms:

| Quantity | Measurement | Old | New (`src/artwork_backdrop.h`) |
|---|---|---|---|
| Dwell (focus change to first change) | category move first visible 83.50 s, backdrop first changed 84.73 s = 1.2 s; BOII row change 110.5 s, change by 111.7 s | 2000 ms | 1200 ms |
| Fade in | linear, 9/27/46/64/83/100 % over six frames = 200 ms | 350 ms | 200 ms |
| Fade out leaving the row | full at 85.066 s, gone at 85.100 s: under one frame (the same frame the next column starts) | 350 ms | 33 ms (cut) |

Only one exit was observed (a category change); an up/down row change inside the column was not sampled, so the 33 ms cut is
generalised from it. Our run (fixture titles with a synthetic PIC1, burst at 33 ms after DOWN): previous backdrop gone in the first
41 ms frame, new one unchanged at 1195 ms, 17 % at 1230 ms, full at about 1390 ms. Sheets: `backdrop-sbs-fadein.png`,
`backdrop-sbs-fadeout.png` under `C:\dev\out\boot-backdrop`. The Jellyfin page backdrop shares the slot and so the same constants.
Gate: `ab-layers --all` whole set PASS, no state changed (the gate pins focus time 2000 ms, past both dwells).
Unit tests: `tests/artwork_backdrop_tests.cpp` (`xmb-artwork-backdrop-tests`).
