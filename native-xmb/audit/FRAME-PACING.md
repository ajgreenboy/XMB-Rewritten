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
3. **First-use glyph rasterisation on the first visit to a column (measured, NOT fixed).** A category or column
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
