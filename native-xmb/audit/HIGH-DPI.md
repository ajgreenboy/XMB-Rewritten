# Resolution scaling (high display scale, 4K, any size and aspect)

Branch `wt/dpi`, 2026-11-14. Report on the 4K TV at 300% Windows display scale where the XMB (v0.3.0) was cropped
and soft. Everything here was measured on the 1080p development PC with an emulated device ratio; nothing was run on
the real TV (see "Still needs the real TV").

## The scaling model

- **One design space.** Every layer is laid out on the console's 1920x1080 canvas (`design_space.h`, `DesignSpace`).
  A window of W x H physical pixels maps to it with one uniform factor `s = min(W/1920, H/1080)`, centred. The 16:9
  area is always fully visible and undistorted; a window that is not 16:9 gets black bars (pillarbox or letterbox),
  as chosen in the par2 sweep. The backgrounds are not drawn full-bleed (the wave sits inside the 16:9 area), so the
  bars stay black. One exception by design: the clock bar plate runs on to the window edge on ultrawide, like the
  console's bar.
- **Physical pixels everywhere.** `View` is a `QOpenGLWidget`. Each pass takes `device = width() * devicePixelRatioF()`
  and builds its own render target, viewport and `DesignSpace` from that physical size: background wave, cold boot,
  particles, glass icons, the in-game backdrop, the gallery and the UI layer (text, dialogs, OSK, clock bar). The text
  renderer rasterises glyphs for the layer's own scale. Nothing renders a 1080p buffer and stretches it. A fixed
  1920x1080 window at DPR 1 is the same code path with `s = 1`, which is why the gate frames cannot move.
- **UI layer quality.** Up to 2560x1440 pixels the UI layer is drawn 2x supersampled (4x MSAA at twice the device size,
  resolved by the bilinear half-scale composite). Above that (3440x1440, 4K, 8K) it is drawn at device size with 4x MSAA
  only (`view.cpp`, `uiSupersample`): at 4K that is a 7680x4320 target avoided, with no visible loss (below).
- **Mouse.** The XMB itself has no pointer hit-testing: `View` takes key and pad input only, and the mouse just shows or
  hides the cursor in kiosk (`CursorHider`, in global coordinates). There is nothing to map. Desktop mode hands the
  screen to Windows and uses its own pad-to-mouse path in native coordinates.
- **Other windows.** The notification pop-up (`notify_overlay.cpp`) is a separate top-level widget sized from the
  screen's logical rectangle through the same `DesignSpace` and painted by Qt at the widget's own ratio, so it scales
  with the screen. The in-game live overlay renders into the main view's device-size framebuffer, no geometry of its own.

## Root cause of the crop

`view.cpp` `View::View`: `setMinimumSize(800,450)`. Qt sizes are logical pixels, so the floor is 800x450 logical =
2400x1350 physical at 300%. A fullscreen kiosk window must be exactly as large as its screen, but whenever the
screen's logical size is below 800x450 (3840x2160 at 500%, 1080p at 250% and above) the minimum wins and the window is
larger than the screen: the XMB's right and bottom parts are off screen. Reproduced with `QT_SCALE_FACTOR=3` on this
1080p PC (a 640x360 logical screen), kiosk and the gate's `full` token:

| | window / grab size | screen |
|---|---|---|
| before (`72638c2`) | 2400x1350 (sidecar `windowWidth` 800, `windowHeight` 450) | 1920x1080 physical |
| after | 1920x1080, pixel-identical to the DPR 1 frame (PSNR inf, SSIM 1.0000) | 1920x1080 physical |

Honest limit: at 300% on a 3840x2160 TV the logical screen is 1280x720, which is above the 800x450 floor, so this
mechanism alone does not explain a crop there. The code path itself (device-size FBOs, viewports, `DesignSpace`) rendered
correctly in every emulated case. Two things on the real machine could not be told from here and are covered by the fix
anyway: (1) the process not being Per-Monitor-V2 aware (Windows then reports a 1280x720 desktop and bitmap-stretches it,
which is exactly "a bit blurry"; Qt 6 asks for V2 but silently keeps an earlier awareness set by a compatibility entry),
(2) Qt's logical-to-native conversion leaving the frameless window slightly off the monitor rectangle.

## Fixes (commits on `wt/dpi`)

1. `dee2b20` Fix cropped fullscreen at high display scale: `View` minimum size 160x90; `SetProcessDpiAwarenessContext`
   (Per-Monitor-V2) before `QApplication`; after `showFullScreen` in kiosk the native window rectangle is compared with
   the monitor's physical rectangle and corrected with `SetWindowPos`, again 1.5 s later. Each check logs one line,
   for example `dpi: kiosk shown: awareness per-monitor (request 1), device ratio 3, window native 1920x1080 at 0,0,
   monitor 1920x1080 at 0,0`. On the HTPC that line (in `XMB_LOG`) says in one look whether awareness was granted,
   what ratio Qt used and whether a correction was needed (" - corrected").
2. `b80da8f`, `753700f` Perf: `XMB_SWAP_INTERVAL=0` turns vsync off for throughput measurement; the UI layer is not supersampled above
   2560x1440 pixels.
3. `c090152` Desktop mode's return to kiosk uses the same monitor fit (`host::fitWindowToMonitor`).
4. `2161357` Tests and tools: gate state `steam-game-root-dpr3`, `work/dpi-grab.ts`, `work/dpi-compare.ts`,
   `XMB_SCRIPT_FRAMELESS=1` (see below).

The aspect-ratio handling (contain-fit, bars) already existed and needed no change; no separate commit.

## Measurements

Frames from `work/dpi-grab.ts` (scripted, pinned clocks, the canonical Game root), scored against the 1920x1080 DPR 1
frame by `work/dpi-compare.ts`: the 16:9 area is box-averaged down to 1920x1080, then PSNR and SSIM. "Edge ratio" is the
Laplacian energy of the text area against the same 1080p frame bilinearly stretched to that size (1.00 means no more
detail than a stretched 1080p image).

| window (physical), DPR | frame | 16:9 area | PSNR dB | SSIM | edge ratio |
|---|---|---|---|---|---|
| 1920x1080, 1 (gate frame) | 1920x1080 | full | inf | 1.0000 | 1.00 |
| 1280x720, 1 | 1280x720 | full | 31.56 | 0.9783 | 1.53 |
| 1366x768, 1 | 1366x768 | 1365x768 | 31.09 | 0.9669 | 1.47 |
| 2560x1440, 1 | 2560x1440 | full | 35.73 | 0.9884 | 3.56 |
| 3440x1440, 1 | 3440x1440 | 2560x1440 centred | 35.61 | 0.9883 | 3.56 |
| 3840x2160, 1 | 3840x2160 | full | 44.34 | 0.9954 | 9.00 |
| 7680x4320, 1 | 7680x4320 | full | 42.94 | 0.9955 | 25.42 |
| 1600x1200 (4:3), 1 | 1600x1200 | 1600x900 centred | 33.07 | 0.9824 | 1.68 |
| 1920x1200 (16:10), 1 | 1920x1200 | 1920x1080 centred | 52.68 | 0.9957 | 1.00 |
| 900x1400 (portrait), 1 | 900x1400 | 900x506 centred | 31.77 | 0.9771 | 1.09 |
| 1280x720 logical, DPR 2 | 2560x1440 | full | 35.73 | 0.9884 | 3.56 |
| 1280x720 logical, DPR 3 | 3840x2160 | full | 43.35 (before the supersample change) | 0.9951 | 8.94 |
| 1280x720 logical, DPR 1.5 | 1920x1080 | full | inf | 1.0000 | 1.00 |
| 1536x864 logical, DPR 1.25 | 1920x1080 | full | inf | 1.0000 | 1.00 |

- The DPR 2 and DPR 3 frames are byte-equivalent to the DPR 1 frames of the same physical size (the same PSNR and
  edge numbers), so the ratio changes nothing but the logical window size. Fractional 1.5 and 1.25 land on exactly
  the 1080p frame.
- Whole XMB visible in every size: the clock bar, category row, item list and the bottom hint are inside the 16:9 area and
  the bars are black (non-black samples outside the area: 0, except 51 on 3440x1440, the clock bar plate running to the
  edge, and one edge column at 1366 from the half-pixel centring).
- The low PSNR at the smaller sizes is the comparison itself (a 1280x720 frame upsampled to 1920 against a 1080p-native
  one), not a defect; the larger frames carry real extra detail (edge ratio 3.6 at 1440p, 9 at 4K, 25 at 8K).
- 4K before/after the supersample change: 44.22 dB, SSIM 0.9990 between them; against the 1080p reference the new
  frame is marginally closer (44.34 against 43.35 dB).
- Framed (non-fullscreen) windows cannot exceed their monitor, so large sizes were grabbed with
  `XMB_SCRIPT_FRAMELESS=1` (opt-in, used by the `size=` token only; the gate never sets it).
- One known sub-pixel difference above DPR 1: the ": Options" hint plaque at the bottom right is one design pixel
  narrower, because its width comes from `QFontMetricsF` whose hinted advance depends on the ratio. It is excluded from
  the scored regions of the new gate state. (Changing it to the text renderer's measure would move the 1080p plate by under
  a pixel and so the pinned frames; left alone on purpose.)

### Performance

`native frames: N fps` from live (unpinned) runs, vsync off, frameless window, this PC's GPU; "motion" is Right/Left
presses every 300 ms so the UI layer redraws most frames. Before = commit `dee2b20` plus the vsync switch.

| size | idle fps | motion fps, before | motion fps, after |
|---|---|---|---|
| 1920x1080 | about 780 | about 370 | unchanged (2x path) |
| 2560x1440 | about 705 | not measured | unchanged (2x path) |
| 3840x2160 | about 410 | 295 to 312 | 315 to 333 |
| 7680x4320 | 6.3 then 1.7 | not measured | about 105 |

4K holds 60 fps with large margin here (about 5x). 8K was the cost: a 15360x8640 4x-MSAA UI target (about 1 GB of
samples) took it to 6 fps and 1.7 fps; drawing the layer at device size fixes that. With vsync on the window locks to
the monitor refresh (143.9 to 240 Hz on this PC). No `renderScale` option was added: it is not clearly needed. If the
HTPC's 3060 Ti ever misses 60 fps at 4K, the next lever would be a `renderScale` key in `xmb-config.json`
(not implemented).

## Regression check

Gate state `steam-game-root-dpr3` in `work/ab-layers.thresholds.json`: the Game root with `QT_SCALE_FACTOR=3` (a 640x360
logical screen), `full` token, sidecar must report 1920x1080, frame scored against its own reference. Reference
`out/native-reference/layers/steam-game-root-dpr3.png` is a **new file**, a byte copy of `steam-game-root.png`; no
existing reference was touched or re-saved. Against the pre-fix build the state fails (sidecar 2400x1350); against this
build it passes at inf / 1.0000 (regions: the frame above the hint, and the item column).

## Reproducing

```
bun work/dpi-grab.ts --exe <exe> --dpr 3 --size 1280x720 --out C:/dev/out/dpi/d3.png     (set XMB_SCRIPT_FRAMELESS=1 for sizes above the monitor)
bun work/dpi-grab.ts --exe <exe> --dpr 3 --full --appargs "--kiosk --display 0" --out ...
bun work/dpi-compare.ts C:/dev/out/dpi/ref1080.png C:/dev/out/dpi/d3.png --sharp
bun work/dpi-grab.ts --exe <exe> --size 3840x2160 --live --env XMB_SWAP_INTERVAL=0 --tokens "pause=12000" --out x.png
```

## Still needs the real TV

- That the HTPC log shows `dpi: kiosk shown: awareness per-monitor (request 1 ...` and a window equal to the 3840x2160
  monitor. `request 0` would mean an awareness was already forced on the process (compatibility settings on the exe).
- That the crop and the softness are gone on the TV at 300%; the crop mechanism proven here (minimum size) does not
  trigger at 1280x720 logical, so if it persists the `dpi:` lines (device ratio, native and monitor rectangle) are the
  evidence to read.
- 60 fps with vsync at 4K on the RTX 3060 Ti (measured here only on this PC's GPU, about 5x headroom).
- Mixed-DPI setups (TV at 300% beside a 100% monitor) were not emulated; `QT_SCALE_FACTOR` scales every screen alike.
