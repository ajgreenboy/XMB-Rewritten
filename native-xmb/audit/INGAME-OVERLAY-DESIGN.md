# True in-game system menu: design note (research only)

Written 2026-11-15 against `main` at tag v0.3.4 (99b3353). Nothing in `src/` was changed and nothing was run for this note.
Labels: **READ** = read in this tree during this task (file and function cited); **DOC** = stated by an existing audit
document, not re-checked; **EXTERNAL** = documented or remembered Windows/vendor behaviour, not reproduced here;
**INFERRED** = my reasoning from READ facts; **UNVERIFIED** = not checked at all.

## 1. Goal

When a Steam game runs and the user presses or holds the PS (Guide) button, the XMB system menu should appear as a
translucent overlay over the running game, instead of the XMB window being brought forward as an opaque window.

## 2. What the repo already does (READ)

The headline: most of the options in the brief are already partly built. The design question is not "which approach" but
"which of the existing paths becomes the default, and what stops it from being one".

| Piece | Where | What it does today |
|---|---|---|
| PS detection without focus | `src/ps_watcher.cpp` (20 ms poll of the shared HID slot plus the XInput Guide bit, tap/hold state machine); `View::psPressed/psHeldEvent/psTappedEvent` (`view.cpp` ~4751-4811) | Works while a game owns the foreground. Hold opens the black PS menu, tap enters the in-game XMB (`Shell::psHeld/psTapped`). No hook. A launcher-owns-tap policy exists (`input/psArbitration=launcher`, `Shell::launcherOwnsPsTap`). |
| Window identity and classification | `game_capture.cpp` `classifyGameWindow`, `findMainWindow`; `game_capture.h` `GameWindowMode` {Invalid, Minimized, Windowed, Borderless, Exclusive} | Exclusive is only a heuristic: monitor-sized, no caption, foreground, and `SHQueryUserNotificationState == QUNS_RUNNING_D3D_FULL_SCREEN` (`game_capture.cpp` ~279-283). |
| One-frame capture (option b) | `game_capture.cpp` `captureGameWindow` (Windows.Graphics.Capture, D3D11 staging readback, `looksBlack` sampler) | Synchronous: the GUI thread joins an MTA worker. The 1500 ms bound covers only the first-frame wait (comment in `game_capture.h`). Measured 153-307 ms on a 1080p stub (DOC: `INGAME-RUNTIME-FIX.md`, `INGAME-XMB-DESIGN.md` section 5). |
| Enter / return | `View::enterInGame`, `returnToGame`, `syncInGameWindow` (`view.cpp` 4881-5028) | Order today: pick the title window (foreground if in the tracked tree, or for a Steam title a monitor-covering foreground window, else `findMainWindow(root)`), classify, **capture synchronously**, `shell_.setInGame(true)`, `bringXmbForward` (activates the XMB, `AttachThreadInput` + `SetForegroundWindow`), then `setTopmost(true)` once. Return: `takeForeground(recorded hwnd)` plus one 400 ms retry. |
| Default render (option b, shipped) | `View::drawInGameBackdrop` (`view.cpp` ~5035); `inGameDim()` default 0.8 | Opaque XMB window, captured frame as a texture, dimmed; plain `(8,8,12)` when the capture is black, failed or the window was minimised. A frozen picture, not the live game. |
| Live overlay (option a, shipped but opt-in) | `src/live_overlay.cpp/.h`, `View` live branch (`view.cpp` ~1475-1503, 4927-4937); env `XMB_INGAME_LIVE=1` | A second top-level window `WS_EX_NOREDIRECTIONBITMAP \| TOPMOST \| NOACTIVATE \| TOOLWINDOW` (`live_overlay.cpp:62`) with a DirectComposition visual holding a premultiplied-alpha flip-model swap chain. The GL renderer draws columns plus a `(0,0,0,dim)` clear into an alpha-capable surface (`View::View`, `setAlphaBufferSize(8)`), the MSAA framebuffer is resolved, **read back to the CPU, flipped, uploaded with `UpdateSubresource`, and presented**. The Qt window stays the activated owner at `windowOpacity 0`. Declined for Exclusive/Minimized/Invalid; any failure falls back to the captured frame. |
| Notification overlay | `src/notify_overlay.cpp` (`WS_EX_TOPMOST\|NOACTIVATE\|TOOLWINDOW\|TRANSPARENT\|LAYERED`, Qt `WA_TranslucentBackground`, software painter) | A click-through layered plaque while a title runs. |
| Quit | `Shell::quitSteamTitle`, `src/steam_quit.cpp` (`audit/STEAM-QUIT.md`) | Process-tree WM_CLOSE, terminate, kill. Not part of this design. |
| Steam test seams | `Shell::steamFake()` (`shell.cpp:2956`), `XMB_STEAM_FAKE`, `XMB_STEAM_ROOT`, `XMB_STEAM_FAKE_EXE/ARGS/FORCE_RUNNING` (`shell.cpp` ~747-766) | A scratch fake library whose "game" is a stand-in exe; "running" means a process lives in the install folder. |
| Render pause | `ps_watcher.h` `TitleSession`, `renderPaused()`, `render_gate.cpp` (a layered/NOREDIRECTIONBITMAP window never counts as occluding, line 61) | The XMB stops its frame loop while a title has the foreground. |

Facts from the docs worth carrying (DOC unless stated):

- Live overlay on a 1080p borderless D3D11 stub: CPU paint median 6.33 ms (p95 8.10) against 0.19 ms for the frozen path; the
  GL-to-D3D bridge alone is 5.86 ms median. `INGAME-FIDELITY-SWEEP.md` section 7 concludes "keep the flag off by default" and names
  a zero-copy GL/D3D shared-texture bridge as the next step. The desktop-composite oracle showed 29,260 changed pixels in the game
  ROI over 1.58 s (live) against zero (frozen). This was not measured against a GPU-bound game.
- A true-FSE stub (`SetFullscreenState(TRUE)`, qUNS=3) still captured fine because Fullscreen Optimizations keeps it DWM-composed
  (`INGAME-XMB-DESIGN.md` section 5, step 4). A real non-FSO exclusive title was never tested.
- An earlier layered-window live attempt (`WA_TranslucentBackground` on the `QOpenGLWidget`) was reverted because the FBO blit
  drops alpha (`INGAME-XMB-DESIGN.md` section 5, row 9); the DirectComposition route superseded it.
- Stale text to fix when this is edited: `INGAME-FIDELITY-SWEEP.md` section 4 still says the DirectComposition live overlay is
  "out of scope" while its section 7 lands it. I also found no heading called "Known gaps" in `README.md` (searched
  case-insensitively for "known gap"); the nearest user-facing text is the Guide-button paragraph at `README.md:334`, which
  repeats `STEAM-QUIT.md`'s last section.
- The d3d flip-model stub (`stubgame-d3d`, `C:\dev\out\ingame-test\d3dstub`) that the earlier measurements used is **not on this
  machine any more**: `C:\dev\out\ingame-test` does not exist; only `C:\dev\out\v2\stubgame.exe` was found. Its source is not in the
  repo. The only in-repo stand-in, `tests/steam_quit_standin.cpp`, creates a `WS_OVERLAPPEDWINDOW` (line 97), so it classifies as
  Windowed, not Borderless.

## 3. Options evaluated

### (a) Topmost transparent overlay window (the existing live overlay is this)

How it works on Windows (EXTERNAL unless noted):

- Two ways to be see-through: `WS_EX_LAYERED` + `UpdateLayeredWindow` (GDI-style per-pixel alpha, software path, fine for a
  plaque, poor for a 4K animated menu), or `WS_EX_NOREDIRECTIONBITMAP` + a DirectComposition premultiplied-alpha swap chain
  (what `live_overlay.cpp` does; the GPU-native route, no GDI surface at all).
- Over windowed, borderless and flip-model games, DWM composes the overlay over the game. A flip-model game that was in independent
  flip (or on an MPO plane) is moved to composed presentation or a hardware overlay plane while the overlay overlaps it. Expect a
  small latency or power change only while the menu is up. Not measured here.
- Over exclusive fullscreen the compositor is bypassed, so nothing can sit above the frame without leaving exclusive mode. On
  Windows 10/11 most D3D9-12 "exclusive" titles are presented through Fullscreen Optimizations as composed flip, so the overlay
  does show (consistent with the `fse` stub result above). A title that opts out of FSO, or Vulkan/OpenGL exclusive, is the
  unverified case. The code already declines live mode when the heuristic says Exclusive.
- Topmost races: many borderless games are themselves topmost, and z-order among topmost windows follows last raise. `enterInGame`
  raises once (`setTopmost`, SWP_NOACTIVATE); nothing re-asserts it while the menu is open (DOC: `INGAME-FIDELITY-SWEEP.md`
  section 4 says topmost is set on entry only). A game that re-raises itself, or a launcher splash (the EA splash seen in
  `PS-INGAME-HOOK.md`), can cover the overlay while navigation continues invisibly.
- Focus and input. There are only two honest choices on a PC without injecting:
  1. Overlay does not activate (NOACTIVATE, pad read by `ps_watcher`/`pad.cpp` through the shared HID slot). The game keeps the
     foreground and **reads the same presses**. The character moves while the user navigates the menu. Unusable as a primary mode
     for any pad-driven game.
  2. The XMB activates (today's behaviour, and the live overlay keeps its Qt owner active at opacity 0). Well-behaved titles stop
     reading input or pause on deactivation; others do not (EXTERNAL, per-title). Steam Input may switch to its desktop layout when
     no game is focused (EXTERNAL; README:334 and `STEAM-QUIT.md` already tell the owner to turn off "Guide Button Focuses Steam").
  There is no third choice that swallows input from the game without a driver or a hook.
- Giving focus back: `takeForeground(recorded hwnd)` with an `SW_RESTORE` for iconic windows and one retry (`returnToGame`). The
  foreground lock can still refuse; the fallback is Alt-Tab. FSO titles do not mode-switch on focus loss, true FSE ones do.
- Anti-cheat (EXTERNAL, UNVERIFIED for any specific title): nothing enters the game process, so module-injection detections do not
  apply. One residual risk is specific to this approach: a screen-sized, topmost, NOACTIVATE, NOREDIRECTIONBITMAP window is also
  how cheat ESP overlays are built, and some anti-cheat systems enumerate such windows. A false positive is possible; I could
  not check it. Mitigations: create the window only on user input (already true), destroy it on return, and offer a per-title
  "never overlay" switch.
- Cost: the current bridge (resolve, `glReadPixels`, `QImage::flipped` copy, `UpdateSubresource`) scales with pixels. 1080p is
  5.9 ms median (DOC). At 3840x2160 the pixel count is 4x, so INFERRED about 20-25 ms, which would miss a 16.7 ms frame at 4K.
  The XMB design space is 1920x1080 and `LiveOverlay::present` already stretches the image to the monitor with a visual
  transform (READ, `live_overlay.cpp` ~102), so rendering the overlay at 1080p on a 4K screen is cheap in theory. Whether the
  View can render at a fixed size independent of the widget size is UNVERIFIED.

### (b) Capture the frame and draw the XMB over a frozen copy in our own window (shipped, default)

- Windows Graphics Capture through DWM, the same API OBS window capture uses. Not injection, no hook, no process access (READ,
  `game_capture.h` header comment). Works for windowed, borderless and FSO games, which includes most DX9/11/12 and many Vulkan
  titles because they are DWM-composed.
- Black frames: `WDA_EXCLUDEFROMCAPTURE` windows and DRM/protected video capture black by design; true FSE bypasses DWM and gives
  black or stale. `looksBlack` (maxChannel <= 3 over a 64x64 grid) turns that into the plain dark backdrop (READ).
- Cost and latency: the entry blocks the GUI thread for the full capture (153-307 ms at 1080p, DOC); at 4K INFERRED more. The
  capture is a fresh D3D device and staging readback per call (`captureImpl`). Because it runs before `bringXmbForward`,
  the delay lands between button press and any visible response. After entry the cost is one texture upload and a triangle.
- Privacy/UI: WGC may show the yellow capture border or a capture indicator on some builds; the code requests
  `IsBorderRequired=false` (`GameCaptureResult::borderOff` is logged). Whether an indicator still appears on the HTPC's Windows
  build is UNVERIFIED.
- It opaquely covers the game, so the game stops visibly running and the user sees a still, which is the console's dimmed backdrop
  minus the live motion.

### (c) Inject into the game's swapchain (Steam/Discord/RTSS style hook)

Recommend against, and the existing design already rejected it (`INGAME-XMB-DESIGN.md` section 2.1). Reasons, all EXTERNAL:
anti-cheat (EAC, BattlEye, Vanguard, Ricochet) can flag or ban unknown modules; each of DX9/10/11/12/Vulkan/OpenGL needs its own
hook and they break across driver and game updates; crashes land in the game process; it conflicts with Steam's own overlay hook
(`GameOverlayRenderer64.dll`) on exactly the games this feature targets; and Steam is allow-listed by anti-cheat vendors, this
application is not. Nothing here justifies it. The only thing it buys, drawing over a true exclusive frame and swallowing the
game's input, is better solved by the owner turning on FSO or the game's borderless mode.

## 4. Recommendation

**Make (a) the in-game presentation, with (b) as its automatic fallback, and never do (c).** Concretely:

1. The overlay is the existing DirectComposition window (`live_overlay.cpp`), not a layered window and not a second GL context.
   It already exists and has a verified fallback ladder: live, then frozen capture, then plain dark.
2. The XMB keeps activating on entry (focus choice 2 above). Say so in the docs: the game sees the press, and deactivation is the
   only input arbitration available.
3. The default stays frozen capture until the live path passes a 4K frame-time check on the HTPC. The switch to default-on is
   limited to `GameWindowMode::Borderless` and `Windowed`; Exclusive stays on the frozen path with a plain dark backdrop.
4. Remove the two costs that make the live path unusable at 4K: the per-frame CPU bridge and the blocking entry capture.

The cheapest first improvement is therefore not a new overlay but making the existing one affordable.

## 5. Phased plan

Each milestone is independently shippable and leaves the current default behaviour unchanged unless stated.

**M1 (smallest useful): make the live path cheap and non-blocking, behind the existing flag.**
- Order of operations in `enterInGame`: try to create the live overlay first; capture the game frame only when live is declined
  or fails. This removes the 150-300 ms synchronous capture from the live entry path (it remains for the fallback).
- Render the live overlay at a fixed 1920x1080 target regardless of the window size, relying on the existing visual `SetTransform`
  stretch. Remove the CPU `flipped(Qt::Vertical)` copy by flipping in the GL blit or the projection. Target: bridge under 2 ms at
  1080p, flat at 4K. Fallback if the View cannot render at a fixed size without disturbing the 6/6 layer gate: an async PBO
  readback.
- Add the entry/return log lines needed to judge it on the HTPC: entry latency (press to first presented frame), bridge ms,
  `inGameMode`, which rung of the fallback ladder was used.
- Acceptance: `bun work/ab-layers.ts --all` unchanged (frozen path untouched); a stand-in run shows the live state and a
  bridge-ms number; HTPC section 8 items 1-3.

**M2: robustness of a topmost overlay.**
- Re-assert topmost on a slow timer while in-game (the overlay window and the owner), and log when something else is above it.
- Handle display changes and device loss: monitor change or resolution switch, `DXGI_ERROR_DEVICE_REMOVED` (already detected in
  `present`), game window destroyed or re-created (`returnToGame` already falls back to `findMainWindow`).
- Per-title "do not overlay, use the plain dark backdrop" preference for anti-cheat titles.

**M3: Steam title window discovery.**
- For a Steam title the window is found only by being a monitor-covering foreground window (`enterInGame` lines 4900-4904). A
  windowed Steam game, or one that does not hold the foreground at the moment of the press, gets no capture and no live overlay
  (INFERRED from that code; whether `runningTitlePid()` is the game's pid for Steam titles is UNVERIFIED). Use the Steam install
  folder logic that `steam_quit.cpp` (`selectGameProcesses`) already has to find the game's windows by process-tree membership.

**M4: zero-copy path, only if M1 misses the 4K budget.**
- Share the GL texture with the D3D11 device (`WGL_NV_DX_interop2`) and copy GPU-to-GPU into the swap chain buffer. EXTERNAL:
  supported on the NVIDIA drivers the HTPC targets, vendor-specific elsewhere. UNVERIFIED on the actual machine.

**M5: default-on for Borderless/Windowed once M1-M3 pass on the HTPC.** Keep an off switch (`XMB_INGAME_LIVE=0` or a settings row).
Deliberately not planned: pausing or suspending the game (`INGAME-XMB-DESIGN.md` section 2.1 rejects it), injection, and swallowing
the game's input.

## 6. New states the gate needs

The harness (`audit/VERIFICATION-HARNESS.md`, named states pinned by `bun work/ab-layers.ts --state <name>`) has `ingame-xmb`,
`ingame-enter-text`, `ingame-enter-text-keyboard` today (READ: stored in `out/native-reference/layers/*.state.json`, recipes
described in the harness doc). I did not locate the recipe definition file; treat names below as proposals.

Sidecar fields to add (INFERRED from the existing `titleRunning/inGame/runningTitle` fields): `inGameBackdrop` = live | frozen |
plain, `liveOverlay` bool, `inGameMode`, `bridgeMs`.

Lifecycle states the code should name explicitly (today these are implicit in `inGameWindowActive_`, `liveOverlay_`,
`inGameBackdrop_`): `Idle -> Entering -> Live | Frozen | Plain -> Returning -> Idle`, with `Live -> Frozen` on device loss or
failure and `any -> Idle` on title exit (`syncInGameWindow`).

Proposed named gate states:

| State | Pins |
|---|---|
| `ingame-live-layer` | Live on, borderless stub: the scripted framebuffer grab. Scripted grabs see the alpha XMB layer, not DWM's composite (DOC), so assert alpha range 204..255 and the sidecar `liveOverlay=1`. Regions below y 140 as for the other in-game states. |
| `ingame-live-fse-fallback` | A stub reporting qUNS=3 gives `inGameMode=exclusive`, `liveOverlay=0`, backdrop frozen or plain. |
| `ingame-live-loss` | Force a device-removed or overlay failure (needs a test env switch): falls to frozen without leaving a transparent owner. Log assertions only. |
| `ingame-live-title-exit` | The stub exits while the overlay is up: overlay stopped, opacity restored, topmost cleared (the 2026-11-10 `style=0x8 topmost=8` regression probe). |
| `ingame-live-steam-fake` | Same as `ingame-live-layer` but through `XMB_STEAM_FAKE` (needs the borderless stand-in, section 7). |

The live desktop composite (changed-pixel oracle) stays a separate script, not a PSNR gate, because DWM composites the final
image and the title bar drifts (same reason the existing in-game states score from y 140).

## 7. Testing without a real game

- Stand-in fullscreen test window: extend `tests/steam_quit_standin.cpp` or add a sibling with `--borderless` (monitor-sized
  `WS_POPUP`, no caption) and a moving colour bar drawn through a flip-model D3D11 swap chain, plus `--fse` (`SetFullscreenState`).
  The previous stub's source is not in the repo and not on this machine (section 2), so rebuild it and commit it under `tests/`.
  Log `foreground=0/1` once a second so return-to-game is measurable, as the previous stub did.
- Steam path: `XMB_STEAM_FAKE=1`, `XMB_STEAM_ROOT=<scratch library>`, `XMB_STEAM_FAKE_EXE=<borderless stand-in>`,
  `XMB_STEAM_FAKE_ARGS="--borderless --bar"`. This exercises `runningTitleIsSteam()` and the monitor-covering-window rule in
  `enterInGame`, which a windowed stand-in does not. `XMB_STEAM_FAKE_FORCE_RUNNING=1` for the pending/confirmed lifecycle.
- Script: `--script full,...,ENTER,pause=3000,ps,...,grab=...` plus `pshold`, as used by `PS-INGAME-HOOK.md` and
  `work/zf2-ingame-test.ts`. Env: `XMB_PS_LOG=1`, `XMB_LOG`, `XMB_OVERLAY_COST=1`, `XMB_FRAME_TRACE`, `XMB_INGAME_DIM`,
  `XMB_INGAME_LIVE`.
- What this can prove: window classification, fallback ladder, lifecycle and cleanup (no stranded topmost or transparent owner),
  bridge cost at 1080p on this dev PC, the alpha layer, that the stand-in's own bar keeps moving in two desktop captures.
- No Steam may be run on the development PC (STEAM-QUIT.md); everything above respects that.

## 8. Only verifiable on the HTPC

(Hardware as given in the brief: RTX-class GPU, 4K TV, real Steam games. I did not verify the machine's specification.)

1. The 4K frame-time budget of the live bridge, XMB and game together, with PresentMon or similar (the existing numbers are a 1080p
   static stub on this PC, not a GPU-bound game).
2. Real titles per API: one DX11, one DX12, one Vulkan, one DX9, each borderless and "fullscreen": which `GameWindowMode` is
   reported, whether the overlay shows, whether the game paused or kept reading the pad, how long the return takes.
3. True exclusive fullscreen with FSO disabled, and the Vulkan exclusive path. Never tested anywhere.
4. Guide/PS behaviour with Steam Input on and off, DualSense vs Xbox pad, the Steam overlay racing the XMB on a tap.
5. HDR and VRR (G-Sync) behaviour while the overlay appears (a mode or cadence change would be visible on the TV), display scaling
   at 4K, and independent-flip/MPO loss and recovery.
6. An anti-cheat title, to see whether a screen-sized topmost NOREDIRECTIONBITMAP window is tolerated. This is the one risk that
   cannot be argued away from documentation.
7. Whether WGC shows a capture border or indicator on that Windows build, and TV latency of the first overlay frame.

## 9. Open items I could not settle

- Whether the View can render the live overlay at a fixed 1920x1080 under a 4K window (UNVERIFIED; needs a read of the render-scale
  and `DesignSpace` code).
- Whether `Shell::runningTitlePid()` returns anything for a Steam title (UNVERIFIED).
- The location of the named-state recipe definitions for `ab-layers.ts` (not found by name search).
- Any specific game's anti-cheat reaction (UNVERIFIED, EXTERNAL).
