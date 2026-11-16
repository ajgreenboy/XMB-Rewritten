# Frame pacing: why 60 fps did not feel smooth (2026-11-15)

Report: on a 4K TV HTPC the menu "doesn't feel locked to 60 fps". The average rate was never the problem; the
unevenness of motion was. Everything below was measured on the development PC (RTX 3070, one 1920x1080 display at
**239 Hz**, Windows 11, live clocks, frameless window, `XMB_FRAME_TRACE=<file>`), not on the HTPC.

## Tooling

- `bun work/frame-pacing.ts --exe <exe> --size 3840x2160 --swap 1 --pacing 1|0 --interval 100|250 [--gpu] [--slow <ms>]`
  scripts a scroll (6x RIGHT, 6x LEFT, 4x DOWN, 4x UP, three times, one key every `--interval` ms: 100 is a held
  d-pad, 250 single presses), and reports over the moving window: swap-to-swap gap, the animation step each frame
  advanced by, the **content error** (animation clock minus the clock of the swap that showed the frame, detrended:
  how far each frame's motion is from where it should be at the moment it is shown), paint CPU, and with `--gpu` the
  GPU time of the paint (GL timer query, read back a few frames later; the compose into the window is not in it).
  `--slow <ms>` instead prints the `XMB_FRAME_TRACE=1` slow-frame log with labels.
- Test hooks (default off): `XMB_PACING=0` restores the old wall-clock step; `XMB_SWAP_INTERVAL=<0..8>` (was 0/1);
  `XMB_SCRIPT_INTERVAL_MS` (script token cadence, 10..5000, default 300); `XMB_PACE_GPU=1`.
- `XMB_FRAME_TRACE=<file>` gained a `frame pace:` line per paint (step in ns, GPU ns); older parsers ignore it.

## Causes found

1. **A 5.6 ms GUI-thread stall inside every cursor move (measured, fixed).** `InteractionAudio::play(Move)` stopped
   and restarted a `QAudioSink` on the GUI thread; WASAPI stop+start cost 5.5-6.3 ms per call (labelled
   `key handler 5.7 ms` in the slow-frame log on every arrow press). It lands in the key press that starts the move,
   so the first frame of every move was late, exactly where motion begins. On an HDMI/TV audio endpoint this call is
   typically slower than on this PC's, so the HTPC pays at least this much; at 60 Hz that is a third or more of the
   16.7 ms budget before a single pixel is drawn.
2. **Animation time sampled at paint start (measured, fixed).** `dt` was the wall-clock gap between `paintGL`
   entries. With vsync, a paint begins a variable time after the vblank that released the previous swap (queued
   `frameSwapped` -> `update()` chain, the 16 ms pad-poll timer, other queued events, the stall above), while the
   frames reach the screen at refresh multiples, so the step jitters even when every frame is on time and every
   stall shows up twice (once as a long step, once as a short one). Seen as: step p95 4.7 ms / p99 10.9 ms against a
   4.17 ms display period.
3. **First-use glyph rasterisation on the first visit to a column (measured; fixed later, see the last section)** A category or column
   seen for the first time rasterises its labels' glyph tiles inside the move frame: 10-37 tiles at 0.33 ms each,
   i.e. 7-16 ms frames here (`scene 15.2 ms; 37 glyph tiles rasterised (12.3 ms)`), 2-3x that on a slower CPU.
   Tiles are keyed by sub-pixel phase (16 per glyph), so a blanket pre-rasterise of ASCII is not cheap; each
   one-off cost is paid once per column per session. Left as is; it is the next hitch to remove (warm the labels of
   the neighbouring categories while idle).
4. **Not causes here (measured):** GPU cost (paint 1.0 ms at 1080p, 1.6 ms p50 / 3.1 ms p95 at 3840x2160 on the RTX
   3070, so the full-resolution passes are not a problem on this GPU; the 4K UI layer is already rendered at 1x
   instead of 2x supersampling); the pad-poll timer and every idle periodic task (a 40 s idle run at 4K has no frame
   over 5 ms after start-up); `frameSwapped` re-arming (swap interval 1 is in effect; the 239 Hz display runs at
   236-239 fps). The prewarm stages run once, ~3-6 s after start, only when idle >= 0.8 s.

## Fixes

- `src/frame_pacer.h` (new, pure logic, `tests/frame_pacer_tests.cpp`): every `frameSwapped` is timestamped at
  emission (a `Qt::DirectConnection`, not the queued re-arm connection, which adds the event-queue wait). The refresh
  period is the median of the last 64 swap gaps; the pacer is "locked" when >= 70 % of gaps sit within 15 % of it.
  A frame painted after swap N advances every animation by the displayed interval of that swap, quantised to whole
  periods: exactly one period in steady state, two after a missed vblank (the animation catches up to wall time in
  one frame instead of running slow), and the background-wave clock follows the same timeline. A slow slew keeps it
  tied to the wall clock. Fallbacks to the old clamped (100 ms) wall-clock step: not yet locked (first ~12 swaps),
  vsync off (swap gaps are not on a grid), a paint with no swap since the last (grab, resize), a stale estimate
  (gap under 0.75 period), a stall or pause. Pinned captures (`secondsOverride_ >= 0`) and `XMB_PACING=0` use the old
  path unchanged, so gate frames cannot move.
- `src/interaction_audio.cpp`: the output device (sink, buffer, format probe) lives on its own thread; `play()` hands
  the cue over and returns at once. `playAt()` (the assertable path) waits for the start, so its `Decision` is still
  the device's own answer. Cue timing, guard windows, gain and theme overrides are unchanged.

Default-on, documented here: `XMB_PACING=0` is the only switch; there is no frame-rate cap.

## Results (3840x2160 and 1920x1080, swap interval 1 on the 239 Hz panel, moving window, live clocks)

Swap gap = swap-to-swap; "off-median" = frames more than 2 ms from the median. Before = wall-clock step with the audio
stall (`XMB_PACING=0`, old audio), after = this commit. One run each; the 239 Hz period is 4.17 ms.

| run | step p50 / p95 / p99 / max (ms) | step off-median | swap gap p95 / p99 / max (ms) | content error p95 / p99 (ms) |
|---|---|---|---|---|
| 4K, 100 ms keys, before | 4.16 / 4.72 / 10.92 / 20.9 | 73 of 1676 | 4.24 / 10.03 / 32.8 | 1.16 / 2.07 |
| 4K, 100 ms keys, after | 4.16 / 4.17 / 4.17 / 16.7 | 13 of 1740 | 4.21 / 5.10 / 18.3 | 0.09 / 1.72 |
| 4K, 250 ms keys, before | 4.17 / 4.53 / 10.31 / 21.4 | 70 of 3668 | 4.21 / 9.17 / 30.5 | 0.48 / 1.50 |
| 4K, 250 ms keys, after | 4.16 / 4.18 / 4.18 / 16.7 | 11 of 3732 | 4.24 / 4.29 / 18.1 | 0.07 / 0.33 |
| 1080p, 100 ms keys, before | 4.12 / 4.86 / 10.91 / 20.2 | 77 of 1736 | 4.74 / 9.86 / 36.1 | 1.53 / 2.05 |
| 1080p, 100 ms keys, after | 4.16 / 4.18 / 4.19 / 16.7 | 7 of 1747 | 4.63 / 4.77 / 17.9 | 0.43 / 1.61 |
| 1080p, 250 ms keys, before | 4.15 / 4.73 / 10.43 / 21.4 | 89 of 3729 | 4.64 / 8.92 / 32.5 | 1.26 / 2.00 |
| 1080p, 250 ms keys, after | 4.17 / 4.19 / 4.20 / 16.7 | 6 of 3737 | 4.63 / 4.76 / 17.3 | 0.41 / 0.60 |

- Frames that lost vblanks to the key-press stall (swap gap off-median): about 70 per 7 s before, 7-14 after (what is
  left are the first-visit glyph frames and the start of the run).
- The maximum 16-18 ms gaps after the fix are the first-visit glyph frames (cause 3); the `max` content error is
  those frames too.
- Paint CPU is unchanged (2.1 ms p50); GPU 1.0 ms (1080p) / 1.6 ms (4K) p50, 1.4 / 3.1 ms p95.

## What could not be verified

- **The display here is 239 Hz, the TV is not.** I did not change the display mode (a system setting). Emulating
  60 Hz with `XMB_SWAP_INTERVAL=4` does not produce a faithful 60 Hz grid under DWM (swap gaps of 3-5 vblanks, run to
  run variance too large to read), so no number above is a 60 Hz measurement. At 239 Hz the same absolute jitter is
  4x larger relative to the frame, so the improvement in `content error` is the better guide to direction, not size.
  Measure on the TV with `XMB_FRAME_TRACE=<file>` + `bun work/frame-report.ts` and the new `frame pace:` lines.
- Whether DWM composition, the TV's own processing (game mode off, motion interpolation, a 30 Hz HDMI mode at 4K) or
  the HDMI audio endpoint's latency add to it. The sink start cost on an HDMI endpoint is expected to be larger than
  the 5.6 ms measured here, which is a reason to expect more benefit there, not a measurement.
- GPU time on the HTPC's GPU. Here it is 1.6 ms at 4K, so a GPU 5x slower still fits 16.7 ms; a much weaker iGPU
  was not tested.
- The glyph first-use hitches (cause 3) on a slower CPU.
- Controller polling with a pad attached (no pad here: `pollController` showed under 0.5 ms).

## Gate

`bun work/v2-fixtures.ts` then `bun work/ab-layers.ts --all --exe C:/dev/out/pace/re-ps3-native.exe`: 28/28 states pass, whole set PASS (all PSNR inf, SSIM 1.0). Pinned clocks take the old step and the audio change does not touch frames. Build: MSVC /W4, 0 warnings; `tests/frame_pacer_tests.cpp` (CMake option XMB_FRAME_PACER_BUILD_TESTS) passes.

## First-visit column hitch (cause 3, fixed; 2026-11-15)

Cause: the labels of a column are rasterised the first time the column is drawn. A glyph tile is keyed by (face, glyph,
pen phase), 4 phases per glyph, and a row's title, value, second line and help text each need their own, so a first
slide into a category rasterised 8 to 37 tiles (0.3 to 0.4 ms each) inside the move frame: 8 to 19 ms frames.

Fix: `View::prewarmColumnText` is stage 10 of the idle prewarm (`prewarmOverlays`, same quiet gate: XMB shown, 800 ms
without input, nothing open, never under a pinned clock). Nearest category first, it queues the title (32 px), value
(21), second line (17), help text (19) and the category name (21) of every row, and `TextRenderer::warmGlyphs`
rasterises all four pen phases of each glyph, 1.2 ms per slice, one slice per 30 ms, into the atlas through the scratch
UI target. It stops while the atlas is over 8 of its 16 pages (so it cannot cause the atlas reset), restarts when the
atlas is dropped (scale change, `atlasGeneration()`), and looks again every 2 s for rows that arrive later
(libraries, friends). `XMB_COLUMN_PREWARM=0` restores the old behaviour. Pinned captures never run it, so the gate frames
cannot move. Whole prewarm here: 125 strings, finished a few seconds after the existing stages; every slice stays under 8 ms.

Measure: `bun work/col-hitch.ts --exe <exe> --size 3840x2160 --cats 13 --dir RIGHT|LEFT --slow-ms 8 --warm-s 25`
(live clock, `XMB_FRAME_TRACE=1`; after the prewarm has finished it scrolls 13 categories, pausing on each to press
DOWN, DOWN, UP, UP; counts frames over the threshold from then on). The 50 ms "choice model built" frame of the existing
stage 9 is excluded from both columns.

| first scroll, 13 categories | frames over 8 ms, before -> after | worst frame ms, before -> after |
|---|---|---|
| 1920x1080 RIGHT | 2 -> 0 | 17.7 -> under 5 |
| 3840x2160 RIGHT | 2 -> 0 | 17.4 -> 3.3 |
| 1920x1080 LEFT | 8 -> 3 | 19.4 -> 13.6 |
| 3840x2160 LEFT | 4 -> 1 | 18.8 -> 8.1 |

Every remaining frame is a `glass icons` frame (the category icons' GPU pass, 10 to 11 ms at 1080p on LEFT) or a first
image decode: not text, not touched here. After the change no frame in either direction carries a "glyph tiles
rasterised" label. Not covered: pages opened with Cross (Settings sub-pages, folders, game lists), whose rows exist
only once opened; they share most glyphs with the warmed titles but a rare glyph still costs 0.3 ms on first sight.

Unverified on the real TV: the 60 Hz frames (these are 239 Hz-display measurements on the dev PC, CPU time only); the
slice cost on the HTPC's slower CPU (about 1.2 ms budget per slice, so a slice there may take up to the same plus one
tile); the size of the real library columns (125 strings here, a large Game or Photo library queues more, spread over
more slices, still capped at half the atlas).

## First-open page hitch (pages opened with Cross; 2026-11-16)

Question: do pages whose rows exist only once opened (Settings sub-pages, message boxes, the on-screen keyboard, the folder
browser, library pages, Users pages) hitch on their first open the way columns did, and why.

Measure: `bun work/page-hitch.ts --exe <exe> --size 1920x1080|3840x2160 [--pages a,b] [--misses] [--env XMB_PAGE_PREWARM=0]
--lock --json out.json`. One live-clock session per run (`XMB_FRAME_TRACE=1`, fresh data dir seeded with one Jellyfin
registration), idle prewarm allowed to finish, then each page is opened once with Cross, scrolled, closed with Circle;
each page is bracketed by two `pause=` script markers so every slow frame is attributed to the page that was opening.
The trace gained labels: `page "<title>" depth N opened (FIRST)`, `focus <category>/<row> depth N` (navigation check),
`N text layouts built (ms)`, and a `glyph miss px=<size> tiles=<n> text=<string>` line for every string whose draw rasterised
tiles (what the prewarm missed). 41 pages were scripted; the table lists the 17 that cost anything. "over" = frames with paint
CPU over 8 ms inside the page's window, then the worst paint CPU in ms. Same PC (239 Hz panel, RTX 3070), one run each, 60 Hz
not measured; "before" = same exe with `XMB_PAGE_PREWARM=0`.

Ranking by first-open cost (1080p before): message pages with long text (30 ms, 73 glyph tiles), the first inline message
(17 ms, 20 tiles), the on-screen keyboard (14 ms at 1080p, 118 ms at 4K), Tailscale (13.5 ms plus a 107 ms key handler),
Accessory/Date/Display/Music/Sound Settings (9 to 12 ms, 4 to 40 tiles each), Users/Steam accounts (11 ms, 61 tiles),
Choose Media Folder (12 ms), the rest under 8 ms.

What the cost was: for most pages it was glyph tiles (0.3 ms each at 4 pen phases per glyph) for text no column holds: help
text, values (dates, device names, "87%"), choice-list items, message bodies, the keyboard's title and field. Not glyphs:
(1) `glass icons` 8 to 10 ms on the first open of some Settings pages (the category icon GPU pass, as in the column section;
untouched), (2) key-handler stalls when the page is built: Tailscale 47 to 107 ms (starts the `tailscale` CLI process on the GUI
thread), Display Settings 33 to 74 ms (display mode and HDR query), Choose Media Folder 7 to 28 ms (directory scan). They are
data scans / process starts, not drawing, and not small to move off the GUI thread, so they are listed, not fixed.

Fix: stage 10 of the idle prewarm (`View::prewarmOverlays`) now also runs `View::prewarmPageText` whenever the column queue is
empty: same quiet gate (XMB shown, 800 ms without input, nothing open, never under a pinned clock), same glyph-tile jobs
through `TextRenderer::warmGlyphs`, one 2.4 ms slice per 20 ms, never while a frame is late (the gate), stopped by the
atlas guard at 8 of 16 pages, restarted when the atlas is dropped. Queue order: (1) characters the previous sessions had to
rasterise per text style, saved at exit to `<data>/cache/text-warm.json` (never from a pinned capture); (2) title, second
line and help text of every Settings sub-page row (`Shell::settingsPageRowsForWarm`, built the way `openSettings` does,
pushed nowhere); (3) digits and punctuation; (4) a letters+digits+punctuation alphabet for the sizes that values, choice
lists, dialogs and the keyboard use (21, 23, 24, 22, 26, 27, 20 design px; most used first so the guard cuts the tail),
and "System Name". `XMB_PAGE_PREWARM=0` restores the old behaviour. The queue is 259 strings and ends with 9 atlas pages in
use at both sizes (the guard stops at the first page over 8). Rendered output is unchanged: the prewarm only fills the
atlas earlier, and pinned captures never run it.

| page | 1080p before (over / worst ms) | 1080p after | 4K before | 4K after |
|---|---|---|---|---|
| friends/discord-setup (message) | 1 / 30.3 | 0 / under 8 | 1 / 32.8 | 0 / under 8 |
| video/continue (message) | 5 / 16.6 | 1 / 8.2 | 1 / 8.9 | 1 / 8.0 |
| settings/system/osk | 3 / 14.3 | 0 / under 8 | 19 / 118.5 | 0 / 3.1 |
| network/tailscale | 2 / 13.5 | 0 / 7.6 | 0 / 6.7 | 0 / 5.0 |
| settings/device | 3 / 12.6 | 1 / 12.1 | 0 / 7.5 | 0 / 3.5 |
| tv/jellyfin-setup (message) | 1 / 12.4 | 0 / under 8 | 1 / 9.7 | 0 / under 8 |
| settings/date | 2 / 12.3 | 1 / 12.3 | 2 / 12.4 | 0 / 3.6 |
| settings/display | 4 / 12.3 | 0 / 3.3 | 0 / 3.4 | 0 / 4.0 |
| photo/library (Choose Media Folder) | 3 / 12.2 | 0 / 2.8 | 1 / 8.2 | 0 / 3.2 |
| users/new-user (accounts) | 3 / 11.4 | 0 / 7.0 | 3 / 12.5 | 0 / 7.1 |
| settings/music | 2 / 10.4 | 0 / under 8 | 0 / 7.9 | 0 / 3.3 |
| network/wifi (message) | 1 / 10.0 | 0 / under 8 | 0 / 7.7 | 0 / under 8 |
| settings/sound | 3 / 9.1 | 0 / under 8 | 0 / 3.0 | 0 / 3.3 |
| game/store-message | 1 / 9.1 | 0 / under 8 | 0 / 3.3 | 0 / under 8 |
| all 17 pages, total frames over 8 ms | 34 | 5 | 110 | 1 |
| worst frame of the set | 30.3 | 12.3 | 118.5 | 8.0 |

The 1080p frames still over 8 ms are `glass icons` (9.5 to 9.7 ms, the icon GPU pass on first open of System, Accessory and
Date Settings; the same pass costs nothing on 4K here, so it is a GPU wait, not text) and the "There are no content items."
message at 26 px (the alphabet for that size falls past the atlas guard). The 4K "before" 118 ms frames on System and the
keyboard did not recur after the change and were not reproduced separately, so their cause is not isolated (the glyph
labels there were 20 tiles; the rest was not labelled). Notes: the learned list (1) is empty in these runs (fresh data dir),
so the numbers are the cold-install path; a machine that has opened a page before warms its own characters too.

Unverified on the real 60 Hz TV: all numbers are CPU paint time on the 239 Hz dev display; the HTPC's slower CPU makes a tile
cost more, so the queue (about 2100 tiles for the alphabets, finished well inside the first minute here) takes longer there;
the GPU-side `glass icons` stall; Jellyfin library pages and the Steam store (need a server / account, only the sign-in and
setup pages were opened).
