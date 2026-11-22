# Game launch: fade to black, start behind black, fade into the game

Owner request: launching a title was jarring because Windows showed the display-mode switch, the title's window
creation and DWM's window animations while it booted. The wanted behaviour, as the owner describes the PS3: pick the
title, the XMB goes to black (the game-boot logo), the game starts and settles behind black, then the black fades
out revealing the game.

## What is firmware and what is not

| Part | Status |
|---|---|
| The game-boot sequence: black plane, `tex_ps3logo` fade in at 0.5 s over 863 ms, fade/scale out at 1.6 s over 174 ms, Finished at 2.8 s | **Traced** (custom_render_plugin.rco `anim_gameboot`; `coldboot.h` GameBoot, unchanged) |
| Holding black after 2.8 s until the title's window is up, then fading into it | **Design choice.** The traced notes stop at Finished; this is the owner's description, not a recovered firmware behaviour |
| Every number below (250 / 500 / 400 / 150 ms, 20 s / 45 s, the 4 s Steam grace, the hard cap, smoothstep) | **Design choice**, not firmware |

## How it works

`src/launch_fade.h` (pure, no Qt or Win32) is the timing; `src/launch_cover.{h,cpp}` is the window; `Shell::coverLaunch`
wires it to the three game-boot launch paths (an RPCS3 title, an imported game app, a Steam title). Web apps, services
and media never get the cover.

1. **Up before the process exists.** When the game boot's Finished event (or a skip) runs the launch, the screen is
   already black (`GameBoot::launching()`), so the cover is shown fully opaque, repainted and two `DwmFlush` calls wait
   for it to reach the glass; only then is the display mode applied and the process created. If the screen is not black
   (a Steam launch that first waited for the Steam client to sign in), the cover fades to black over 250 ms first.
2. **The window.** A separate Qt top-level: frameless, `WindowStaysOnTop`, `Tool`, `WindowDoesNotAcceptFocus`,
   `WindowTransparentForInput`, plus `WS_EX_TOPMOST|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW|WS_EX_TRANSPARENT`. It never takes
   focus or mouse input, so it cannot steal the foreground from the title or the in-game XMB. It spans the monitor of
   the XMB's own window (the monitor `display::Controller` changes), follows that monitor's rect after a mode switch,
   and re-asserts `HWND_TOPMOST` every 250 ms while black (a title's own later topmost window would otherwise sit above).
3. **Hold.** Every 100 ms the largest visible window of the title is sampled (`findMainWindow`, `classifyGameWindow`):
   for a tracked launch any process in the tree under the started process; for Steam the processes running from the
   title's install folder and their children (`steamquit::selectGameProcesses`, refreshed once a second).
4. **Settled.** The same window, rect and mode for 500 ms. A Steam window that does not cover its monitor (a launcher,
   a splash) must instead stay put for 4 s. On settling the foreground is handed to that window (the attach-input
   pattern View already uses) and the cover fades out over 400 ms (smoothstep in opacity).
5. **Exclusive fullscreen.** When the settled window is classified Exclusive the cover is removed without a fade: DWM does
   not compose a layered window over it, and a fading topmost window could knock it out of exclusive mode.

### Failure paths

| Event | Result |
|---|---|
| `waitForStarted` fails / `FailedToStart` / RPCS3 missing | 150 ms fade, the existing error page shows |
| The process (or the whole Steam title) ends while black | 150 ms fade, the existing return / crash message path runs |
| A key, pad button or PS press while black | 150 ms fade; key and pad presses are consumed, the PS press goes on as before |
| Steam's hidden-client sweep must show a sign-in or message window | the cover is withdrawn (150 ms) |
| No settled window within 20 s (45 s for Steam) | 400 ms fade, everything else as before the cover existed |
| Hard cap (fade-in + timeout + fade + 2 s) | removed at once by the state machine |
| A watchdog timer 1.5 s after the hard cap | hides the window whatever the state machine says, and runs a launch that never ran |

A cover withdrawn before it was opaque still runs the launch exactly once (tested).

### Switches

- `XMB_LAUNCH_COVER=0` turns it off; `=1` forces it on.
- `XMB_NO_GAMEBOOT=1` (the seam the pinned gate's launch recipes use) also turns it off unless `XMB_LAUNCH_COVER=1`, so
  those recipes behave exactly as before. `XMB_GAMEBOOT_AT` never launches, so it never shows a cover.
- `XMB_LAUNCH_COVER_TIMEOUT_MS=<500..600000>` replaces the no-window timeout (scripted runs).

Log lines, all prefixed `launch-cover:` and stamped `at=<ms since the Unix epoch>`: `begin`, `opaque=1; starting the
title`, `title started`, `window seen`, `fade-out reason=<window-stable|process-exited|start-failed|press|
no-window-timeout|hard-cap|aborted>`, `foreground handed`, `done`, plus `watchdog`, `press`, `withdrawn`,
`title ended` and `monitor is now`.

## Verification

- **Unit:** `xmb-launch-fade-tests` (`tests/launch_fade_tests.cpp`, option `XMB_LAUNCH_FADE_BUILD_TESTS`, run by
  `tests.yml`) drives every path on a fake clock: immediate vs faded start, launch-once, settle, re-settle after a
  windowed-to-fullscreen switch, minimised/flickering windows, exclusive, exit, start failure, press (ignored during
  the fade-in), no-window timeout, Steam's longer timeout and windowed grace, hard cap, withdraw-before-opaque,
  reset and monotonic opacity.
- **End to end:** `bun work/launch-fade-e2e.ts` launches `xmb-launch-fade-standin` (`tests/launch_fade_standin.cpp`)
  through the real shell, game boot included. The stand-in logs, from its first instant and every 50 ms, which window is
  topmost over the monitor centre (z-order walk, so the click-through cover counts) and that screen pixel.

Run on 2026-11-21 against a Release build of this branch, on a machine whose XMB window sat on a secondary 1920x1080
monitor left of the primary (the cover followed it: `rect=-1920,4 1920x1080`). 39/39 checks passed, five cases:

| Case | Stand-in | What was observed |
|---|---|---|
| after | window after 1.5 s, 1280x720 | cover opaque 36 ms before the stand-in's first instant; its first look: cover on top, pixel 0,0,0; the new window stayed under the cover for 7 probes; fade began 557 ms after the window showed; intermediate pixels 0/12/15 to 0/176/225 during a 410 ms fade; afterwards the stand-in on top in its own colour and holding the foreground |
| fullscreen | window after 0.6 s, popup fullscreen 0.3 s later | the windowed-to-fullscreen switch happened under the cover (13 probes); settled as `mode=borderless` 631 ms after the switch |
| never | no window, timeout 4 s | `no-window-timeout` 4001 ms after the cover came up; cover gone afterwards |
| exit | no window, exits at 1.2 s | `process-exited`, cover gone 167 ms after the exit |
| press | no window | a scripted key while black: `reason=press`, gone 2.76 s after launch |

In every case the cover was opaque (log line) before the stand-in's process started (its own first log line), and the
stand-in's very first probe saw the cover and black. The visual gate (`bun work/ab-layers.ts --all`) was run with the
same build: 6/6 layers and 28/28 named states, whole set PASS (the first full run had one capture, `steam-game-root`, exit
255 with no grab; that state passed bit-identically on its own and the full rerun passed). The gate's launch recipes use
`XMB_NO_GAMEBOOT=1` or `XMB_GAMEBOOT_AT`, so no gated frame contains the cover.

## Not verified

- **Real Steam.** Only the shell's Steam wiring was compiled; no real `steam -applaunch` ran under the cover, so the
  install-folder window search, the 4 s grace and the withdrawal on a Steam prompt are untested against Steam itself.
- **Real exclusive fullscreen.** The stand-in is a GDI window (windowed or a borderless popup); the exclusive branch is
  covered by the unit test only.
- **A real display-mode switch.** No apps.json `display` entry was exercised; the monitor-follow code is untested.
- **The PS3 itself.** No console reference exists for the hand-off; its look is the owner's description.
- **Multiple monitors** and a title that opens on a different monitor from the XMB's.
- **A real game** (RPCS3 or an imported title).
- **A skip at 0.4 s.** A press that ends the game boot early hides the half-faded logo at once (the cover is drawn
  opaque immediately); not exercised in the run above and not compared with the console.
