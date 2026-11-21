# Video player polish (2026-11-21)

Branch `wt/player-polish`. Scope: the loading presentation and the bottom chrome of the video player, from the owner's report
that the Jellyfin player looks janky and from the frame audit of the same day (1080p and 2160p bursts of a local clip with a
sidecar subtitle). There are **no console frames** for the player (see `VIDEO-PLAYER-PARITY-2026-11-16.md`), so nothing here
claims firmware parity. Every layout number below is a **design choice** of this build; the icons, plates, strings and fonts are
the existing firmware assets. The decision logic is `src/player_chrome.h`, tested by `xmb-player-chrome-tests`.

## What changed, by audit item

| # | Defect | Change | Evidence |
|---|---|---|---|
| 1 | The load card was the music player's (note tile, "1", pause glyph, "Track 1") with "Loading..." drawn over "Track 1" | `Shell::videoPending()` (a video item is open, the player has no video track yet; local items by suffix, Jellyfin by the queue's audio flag). While it holds, the music path is off (`View::mediaAudioOnly` excludes it) and one video card is drawn: title on black, loading icon, wait bar, `msg_buffering`, the circle key with `msg_quit_playback` | frames, 1080p |
| 2 | Black frame with a fully opaque control bar before the first picture | The card stays until the first decoded frame is on screen (`loadCardActive`; a decoder that reports video but never draws releases it after 8 s). The bar's 3 s hold starts at the first frame. Sequence is card -> video with controls | frames, 1080p |
| 3 | Doubled-looking text over video (XMB default shadow 4/5 px) and weak hints | Player chrome text uses a 1 x 1.5 px, near-black shadow (`kShadowDx/Dy/Grey`) and a bottom scrim (770..1080, up to alpha 165, smoothstep). Both are in 1920x1080 scene pixels, so they scale with the window like the text | frames, 1080p and 2160p |
| 4 | Subtitle cue under the matrix (Repeat icon, Native plate) | While the matrix is open the cue's bottom edge moves from y 850 to y 470, above the status row | frame, 1080p |
| 5 | Weak time bar, labels changing width | A dark rounded channel behind the track and a bright core line on the played part, over the unchanged firmware art. The labels were already 16 px per glyph cells; the left/right clock is now one function (`clockLabel`, always `HH:MM:SS`, tested) so the width cannot change across a minute or hour | bar frame; width by code and test (the old cells were not observed to change width) |
| 7 | Two different buffering screens on Jellyfin start | One routine (`View::drawVideoBuffering`) draws the loading icon, wait bar and `msg_buffering`: centred on the load card, bottom left as the re-buffer overlay once a picture exists. The hardcoded "Buffering..." string and the hand-drawn arc are gone | card frame; the Jellyfin path and the mid-playback overlay by code only (no server in the capture rig) |
| 8 | Dimmed icons not clearly unavailable; key row has no labels | Unavailable matrix icons draw at 0.3 (was 0.5). With the matrix open the eight key icons now carry their names: system_plugin `msg_back` / `msg_enter`, the video plugin's own previous/next/fast/display strings, and the first line of `msg_help_control_panel_home_menu` for START. Which word sits on which key is a design choice | matrix frame |

## Not done / still open

- **Item 6, end-of-clip behaviour (open, owner decision).** At the end of the queue the player stays on a black screen with
  "End of queue" pinned. This change does not touch it. Options for the owner: return to the library row, show the console's
  `videoplayer_playend_menu_list` style menu, or stay as is.
- Item 8, Audio Options note wrapping: the note under the Audio Options panel still wraps to two lines; in the 1080p frame its
  second line ends about 15 px above the matrix plate and does not overlap it. Not changed.
- The mid-playback re-buffer overlay and the Jellyfin start path were not captured (no Jellyfin server in the rig); they share the
  routine seen on the card frame.
- Local files whose suffix is not in the library's video list still show the music card until the player reports a video track.

## Verification

- `xmb-player-chrome-tests` (new): video suffix rule, load-card decision including the 8 s release, shadow bounds, scrim shape,
  subtitle bands against the queue line and the matrix status row, fixed-width clock. Registered in `CMakeLists.txt`,
  `.github/workflows/tests.yml` and `work/ci-config-build.ps1`; `bun work/ci-guard.ts` passes.
- Frames (private, not in git): before = the 0.3.21 release build, after = this branch, same script, 1920x1080 and 3840x2160,
  under `C:/dev/out/player-polish-captures` (`before1080`, `after1080`, `before2160`, `after2160`: start bursts, bar, matrix, Audio Options).
