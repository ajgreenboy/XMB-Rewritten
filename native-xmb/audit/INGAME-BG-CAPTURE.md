# In-game XMB backdrop: the last good game frame, latched window, persistent capture

Owner report: when the in-game XMB (PS menu over a running game) opens, the background is often not the game and looks
wrong. On the PS3 the game frame is the backdrop. This note records what is traced, inferred and not verified, what changed,
and how it is proved locally. Files: `src/ingame_backdrop.h` (pure rules), `src/game_tap.{h,cpp}` (capture session),
`src/view.cpp` (`pollTitleLatch`, `enterInGame`, `liveGateTick`), `tests/ingame_backdrop_tests.cpp`,
`tests/overlay_standin.cpp`, `work/ingame-bg-test.ts`, `work/ingame-bg-sampler.ps1`.

## 1. Traced, inferred, not verified

| Claim | Status | Source |
|---|---|---|
| The firmware's in-game XMB resource has an empty `page_xmb_bg`: nothing in it draws the home wave, the game frame is what shows behind the menu. | TRACED | `INGAME-XMB-DESIGN.md` F3, F15 |
| The game keeps running under the menu (the PS3 does not pause a title for the XMB). | TRACED | the 2.40 filmed-TV demo, `INGAME-FIDELITY-SWEEP.md` |
| The game is dimmed to roughly 20 percent of its luminance (`XMB_INGAME_DIM` default 0.8 black). | INFERRED (exposure-normalised filmed demo; not a recovered blend constant) | `INGAME-FIDELITY-SWEEP.md` |
| The Windows build can use the title's own frame as the backdrop on every entry. | Shown with the stand-in (section 5) | this change |
| Real games: unity, SDL, UE, Steam titles. They differ in what they do on focus loss and in how many windows they own. | NOT VERIFIED | needs the HTPC with real titles |
| Exclusive fullscreen outside DWM. WGC returns nothing or black; the tap sees no good frame and the old path (one capture attempt, else plain dark) applies unchanged. | NOT VERIFIED | needs a real exclusive title |
| HDR windows (the WGC surface is BGRA8; a tone-mapped or clipped frame is possible). | NOT VERIFIED | |
| DRM-protected or capture-excluded windows (`SetWindowDisplayAffinity`): WGC yields black or nothing, so no good frame and the old path applies. | NOT VERIFIED | |
| Steam's own overlay windows (Shift+Tab, Big Picture) are separate windows of steam processes; they are never latched, but what a Steam title does with its window when its overlay opens is untested. | NOT VERIFIED | |

## 2. What was wrong

`View::enterInGame` took no capture by default and relied on the DirectComposition live overlay showing the GL output at
0.8 dim over whatever DWM composes once the game loses focus. A title that minimises, cloaks or blanks on focus loss (SDL's
default for fullscreen titles) leaves the wrong picture behind the overlay. The title's window was guessed at the PS press
(foreground, else the largest window), which for Steam titles can be the wrong window or none. A minimised or missing
window fell to plain dark (8,8,12). The frozen fallback was one synchronous Windows Graphics Capture frame (372 ms
measured, 1.5 s bound) used only for Exclusive mode, overlay failure or `XMB_INGAME_LIVE=0`.

## 3. What it does now

1. **Latch** (`pollTitleLatch`, 300 ms tick while a title runs). The first time a window of the title holds the foreground
   (process in the tracked tree; for a Steam title a monitor-covering window that is not `steam.exe`, `steamwebhelper.exe`
   or `gameoverlayui.exe`) its hwnd and pid are latched. It is kept while the title runs and the window exists, and replaced
   only by a larger window of the same title or after the window is destroyed (rules and tests: `ingame_backdrop.h`,
   `WindowLatch`). `enterInGame` uses the latch first, and only then the old foreground/largest-window guess.
2. **Tap** (`GameFrameTap`). A free-threaded Windows Graphics Capture frame pool of 2 on the latched window, capped at 20
   frames a second (`MinUpdateInterval`), border and cursor off where the OS allows. Per delivered frame: copy into one of a
   3-slot GPU ring, `GenerateMips`, copy a ~60x33 mip into a tiny staging texture. A frame later (or when asked) that probe is
   mapped without waiting and classified by `levelIsBlank` (no channel above 2 = blank). A good slot is promoted to the
   resident "last good" texture by a pointer swap. No CPU copy of a full frame is made while the title runs. Measured with
   the stand-in at 1080p: 144-157 us average CPU per delivered frame on the capture thread (first frame about 1.5 ms),
   well under 1 ms.
3. **Entry**. If the tap has a good frame, one CPU copy (`grabLastGood`, 3-5 ms at 1080p) becomes the OPAQUE backdrop,
   dimmed by `inGameDim()` as before, whatever the title did on focus loss. If it never had one (no tap, WGC unsupported,
   black, excluded, exclusive) the previous behaviour runs unchanged: live overlay or one blocking capture or plain dark.
4. **Live only when proven** (`liveGateTick`, 100 ms). Once the XMB is up the frozen frame is replaced by the DirectComposition
   live overlay only if, at least 300 ms after the raise, at least 2 good frames arrived since the raise, the newest is under
   250 ms old, the last classified frame is not blank, the window is not iconic or cloaked and the mode is windowed or
   borderless (`shouldGoLive`). The Qt window stays opaque until the overlay's first present, so there is no undimmed flash.
   A live overlay is dropped back to the frozen frame (refreshed from the tap) when the window goes iconic or cloaked or the
   newest frame is blank (`shouldDropLive`); silence alone (a paused title) keeps the overlay. One promotion attempt per entry,
   abandoned after 2 s.
5. **Scripted runs are unchanged.** The latch and tap are off in a `--script` run (`pad::scriptOnly()`) unless
   `XMB_INGAME_TAP=1`; `XMB_INGAME_TAP=0` turns them off everywhere. The gate's `ingame-xmb` states therefore take the old path.

## 4. Memory

GPU, B8G8R8A8 at 4 bytes a pixel: a 3-slot ring and the last-good texture, each with mips down to the probe level (x1.33), plus
the 2-buffer WGC pool: about 7.3 frames of w x h x 4. 1080p: 8.3 MB a frame, about 61 MB (the log reports 58.0 MB).
4K: 33.2 MB a frame, about 243 MB. `grabLastGood` adds one transient staging texture and the returned image (8.3 MB at 1080p,
33.2 MB at 4K) which lives until the XMB returns to the game. All of it is released when the latch is released (title ended
or window destroyed).

## 5. How it is proved locally

`tests/overlay_standin.cpp` is the title: a borderless flip-model D3D11 window whose colour field and sweeping bar change
every frame, with `--blur none|minimize|pause|black` (what it does on focus loss), `--frame-log` (every presented frame: time,
field colour, bar x, plus the focus-loss time) and `--at X,Y` (which monitor). `work/ingame-bg-test.ts` launches it as the
XMB's tracked title with an explicit scratch `--steam-root` (the XMB never looks at Steam), taps PS in a scripted run, and
`work/ingame-bg-sampler.ps1` reads screen pixels at points where the in-game XMB draws nothing plus a 1-pixel strip in which it
finds the dimmed bar. The bar moves 0.25 px a millisecond, so its x behind the XMB says which game frame is on screen, and the
field colour at the points must be that frame's, dimmed to 20 percent. The checks: every sample is explained by a real game
frame, never black, the desktop or another window; for minimise, pause and black the backdrop is the last frame and does not
move; for a normal title it starts on the entry frame and follows the game once the live overlay is promoted.

The test uses `--kiosk --display N` so the XMB and stand-in run on a monitor no other XMB window is on, and treats a run in
which the stand-in lost focus to anything other than the in-game XMB as void and repeats it. Evidence frames:
`C:\dev\out\ingame-bg-captures\{before,after}-{none,minimize,pause,black}\shots`.
