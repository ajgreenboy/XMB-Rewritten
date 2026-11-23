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

### The "Sony Computer Entertainment" wordmark stage: not implemented, on purpose

It appears only in the older-firmware recording M2 (11.6-14.6 s, wordmark x 720-1082, y 358 of 720, over a ribbon that brightens
from dark brown to terracotta), where it replaces the PS3 logo and there is no notice. The 4.93 firmware here ships `tex_scelogo`
(1024x64, ink 497x34), but it reads "Sony Interactive Entertainment" in a different typeface, and `anim_coldboot2` holds it at alpha
0 (PIPELINE.md "Cold boot"); no "Sony Computer" string exists in dev_flash. The newer recording R, the same firmware generation as
ours, shows no wordmark at all. Adding it would move our boot away from the 4.93 reference, so it is left out (no
`XMB_BOOT_EXTRAS` switch was needed).

### Remaining differences (not fixed)

- Ours shows a brighter ribbon in the first ~0.3 s (t 0-0.3 s) than the recording's first visible frames; the recording is black
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
