# Video player parity against the PS3 4.93 firmware resources (2026-11-16)

Branch `wt/vplayer`. Scope: the playback screen of this build (control bar and OSD, the time and information displays, the
Triangle control panel, pause/stop/resume) against what the installed firmware's own video player resources define.
Earlier work this builds on: `VIDEO-CONTROL-PANEL.md` (the 24-icon matrix), `PLAYER-PANELS-SPEC.md`, `MEDIA-PANELS-FIRMWARE.md`,
`MEDIA-CONTROLS.md`, `JELLYFIN-PLAYER.md`.

## Method and what counts as evidence

- **traced**: read out of `videoplayer_plugin.rco` (412,136 bytes, 147 objects, 159 images, 160 strings) and its sibling
  `videoplayer_util.rco` with `bun work/media-rco-dump.ts --rco videoplayer_plugin --tree|--texts|--images` and the app's own
  `--rco-tree` / `--rco-text`. Object names, offsets, grid cells, texture names and sizes, and every string are firmware facts.
- **fit**: a placement, timing or behaviour this build chose because the container carries no number for it.
- **unprovable**: lives in `videoplayer_plugin.prx` (not decompiled locally) or needs a console frame. Not claimed.
- Console frames: **none**. RPCS3 plus a playable test file was **not attempted**. `REFERENCE-GAPS.md` 3.1 records the prerequisite
  (a file written into the emulated HDD, a keyboard-config swap and a registry restore, all under the shared gate lock) and no
  earlier sweep ever reached a video; with the shared gate queue that was beyond the bounded effort for this pass, and a
  failure would not have been informative (it could not tell "the 4.93 player does not run under RPCS3" from "my rig was
  wrong"). Everything below is therefore firmware-resource evidence, not frame comparison. No pixel-parity claim is made.

## What the console's player Triangle menu is (traced)

Triangle opens `videoplayer_control_panel_page` (`videoplayer_cp_group` -> `videoplayer_cp_button_group`, 25 button objects @0xc1e0..0xd740,
`videoplayer_cp_button_name` Text width 400, `videoplayer_cp_help` Plane). It is a **button matrix, not a list menu**. The container has
exactly three list objects, none of them for audio, subtitle, repeat, speed or chapters: `videoplayer_scene_search_list` (XList, under
Scene Search), `videoplayer_av_settings_menu_list` (MList, under AV Settings) and `videoplayer_playend_menu_list` (MList, the end-of-title
store menu). So the console's player Triangle menu contains these controls, in this object order (cell x,y; +y is up):

| # | row (y=+1, left to right) | cell | title (string key -> English) | icon |
|---|---|---|---|---|
| 1 | (iconless dummy, never drawn) | -6,+1 | - | - |
| 2 | Scene Search | -5,+1 | `msg_scene_search` Scene Search | `tex_cp_scene_search` |
| 3 | Go To | -4,+1 | `msg_goto` Go To | `tex_cp_jump` |
| 4 | Audio | -3,+1 | `msg_audio_change` Audio Options | `tex_cp_audio` |
| 5 | Subtitle | -2,+1 | `msg_subtitle_change` Subtitle Options | `tex_cp_subtitle` |
| 6 | Volume | -1,+1 | `msg_volume_control` Volume Control | `tex_cp_volume` |
| 7 | AV Settings | 0,+1 | `msg_audio_video_settings` AV Settings | `tex_cp_av_settings` |
| 8 | Screen Mode | +1,+1 | `msg_viewmode` Screen Mode | `tex_cp_screen_mode` |
| 9 | Change Icon | +2,+1 | `msg_icon_change` Change Icon | `tex_cp_change_icon` |
| 10 | Delete | +3,+1 | `msg_del` Delete | `tex_cp_delete` |
| 11 | Display | +4,+1 | `msg_display_display_with_icon` Display | `tex_cp_display` |

| # | row (y=0) | cell | title | icon |
|---|---|---|---|---|
| 12-24 | Return to Beginning, Next, Fast Reverse, Fast Forward, Play, Pause, Stop, Instant Replay, Instant Advance, Slow (Reverse), Slow (Forward), Frame Reverse, Frame Advance | -6..+6 | `msg_beginning_with_icon`, `msg_next_with_icon`, `msg_fast_r_with_icon`, `msg_fast_f_with_icon`, `msg_play_with_icon`, `msg_pause_with_icon`, `msg_stop`, `msg_flash_r`, `msg_flash_f`, `msg_slow_r`, `msg_slow_f`, `msg_oneframe_r`, `msg_oneframe_f` | `tex_cp_prev, next, search_b, search_f, play, pause, stop, flash_b, flash_f, slow_b, slow_f, step_b, step` |
| 25 | Repeat | 0,-1 | `msg_repeat` Repeat (98x70) | `tex_cp_repeat` |

Each button has `image`, `shadow` and `focus` 70x70 textures (repeat 98x70). That is 24 visible icons. A small hint plane (`videoplayer_cp_help`) and
eight 86x30 key textures (`tex_cp_help_circle/cross/l1/l2_left/r1/r2_right/select/start`) belong to the panel. Strings that show how the controls
behave, though the behaviour is in the .prx: `msg_error_no_audio` "There is no audio.", `msg_error_no_subtitle` "There are no subtitle options
available.", `msg_error_no_execute` "This operation is not available here.", `msg_error_overlimit_goto` "The range you can specify has been
exceeded.", `msg_error_cannot_create_short_icon`, `msg_del_playingtitle_ask`, `msg_repeat_off` / `msg_repeat_on` / `msg_repeat_title` /
`msg_folder_repeat` / `msg_repeat_ab`, the screen-mode names `msg_normal`, `msg_original`, `msg_full_screen`, `msg_zoom`, `msg_double`, the
AV-settings terms (`msg_fnoise`, `msg_bnoise`, `msg_mosquito_noise_reduction`, `msg_minus1..4`, `msg_plus1..4`) and the key help
`msg_help_control_panel_home_menu` = Triangle glyph "Control Panel" / second glyph "Home Menu".

Which controls are shown or greyed for which title is **unprovable**: the container publishes the first eleven buttons with scale 0 (hidden until
the plugin shows them) and carries no availability rule.

## Per element: console, this build before, this build now

Classes: **matches**, **fixed here**, **fixable-small** (not done), **recorded-large**, **unprovable**.

| Element | Console (evidence) | This build now | Class |
|---|---|---|---|
| Control matrix: slots, cells, order, titles, icons | 24 icons in the object order above; titles from `videoplayer_plugin` strings; focus art `*_focus` (traced) | same table, now a pure model (`src/video_panel.h`, tested): order, cells and keys unchanged, titles read from the string table, missing key shows its id | matches (placement of the 100 px pitch and (960,650) anchor is a fit) |
| Native rows vs console rows | n/a | The only non-console slot is a single "Native" plate at cell (+2,-1), after every console cell. It opens "Native Extras" | native-only, last |
| Go To | `videoplayer_display_time_jump_hour/minute/second` (three ItemSpin objects) in the time page, `tex_progress_bar_jump_indi` marker, `msg_error_overlimit_goto` | was refused as "no time-entry". Now a time entry: Up/Down turn the field, Left/Right pick it (opens on minutes), Cross jumps, Circle cancels; the jump marker follows the target; past the end shows the console sentence | fixed here (spin wrap, start field, key set are fit) |
| Screen Mode | `msg_normal`, `msg_original`, `msg_full_screen`, `msg_zoom` (and `msg_double`, `msg_720p_display_normal` for the AV side); information icon `tex_i_screen_mode` 40x40 | was refused. Now cycles Normal, Original, Full Screen, Zoom with the information page; Normal is the old letterbox | fixed here (geometry per mode is fit: Original = own pixel size, never upscaled; Full Screen = stretch; Zoom = fill and crop). `msg_double` not offered |
| Audio / Subtitle titles | `msg_audio_change` "Audio Options", `msg_subtitle_change` "Subtitle Options"; selection mechanism not in the container | Two separate panels titled with those strings; the subtitle list's first row reads `msg_off` "Off"; the old mixed "Playback" list is gone | fixed here (the mechanism, list vs cycling, is unprovable) |
| Audio / Subtitle with nothing to choose | `msg_error_no_audio` / `msg_error_no_subtitle` | The console sentence is shown as the panel note, followed by this build's reason (for example no sidecar found) | fixed here |
| Information page after an audio / subtitle / screen-mode choice | `videoplayer_information_page`: `tex_i_carpet` plate, icon plane (`tex_i_sound`, `tex_i_subtitle`, `tex_i_screen_mode`, 40x40), icon string, string | A plate with the icon and the chosen name, 3000 ms plus 250 ms fade, right-aligned at the top | fixed here (position and timing are fit) |
| Repeat | button cycles; status page shows `tex_i_repeat` (98x74) and `tex_i_title_repeat` (72x36); strings `msg_repeat_off/on/title` | cycle Off, One, All unchanged; status icons unchanged; the status text now shows the container's string after a change | fixed here (cycle order Off, Title, On is this build's; the container's string order hints at the same cycle but proves nothing) |
| Repeat / Shuffle rows in the old list | Repeat is a matrix button; Shuffle does not exist in the video panel | Repeat row removed from the list (it is the matrix button and R); Shuffle moved to Native Extras | fixed here |
| Quality, Chapters, Speed, Play Next Episode, Shuffle (Jellyfin phase 1/2) | none of these exist in the container | Kept, in the "Native Extras" panel behind the native slot, behaviour untouched | native-only, after the console rows |
| Display | `videoplayer_display_infobar_page`: `tex_i_upper_bar` plate, `tex_vc_*` / `tex_ac_*` codec pictures, channels (`msg_ch_type`), sample rate (`msg_khz`), bit rates (`msg_kbps`, `msg_mbps`) | Display (SELECT, I, the matrix button) still pins the chrome and now also shows the plate, the codec pictures that exist for the stream, channels (Jellyfin) and bit rates (local files). Sample rate is not reported by either source and is omitted | fixed here, partly (positions are fit; H.265, AV1, FLAC, Opus have no picture in the container and get none) |
| Console key hint | `msg_help_control_panel_home_menu` (Triangle: Control Panel, second glyph: Home Menu) | Drawn right-aligned above the time bar while the matrix is closed, with the glyphs from the image font, as the music page already does | fixed here (placement fit; the container does not say when the console shows it) |
| Scene Search | XList + thumbnails + interval strings (`msg_interval_*`) | refused with a reason | recorded-large (needs thumbnail generation per interval) |
| AV Settings | MList with noise reductions and offsets | refused with a reason | recorded-large (no AV pipeline) |
| Change Icon / Delete | `msg_icon_change_video_ask`, `msg_del_playingtitle_ask` confirm pages | refused with a reason; no file is ever deleted from the player | recorded-large (no icon database; deletion stays out of the player on purpose) |
| Fast Reverse/Forward as speed scan, Slow, Frame step | speed badge art `tex_i_10/15/30/120`, `tex_i_f15..f180`, strings `msg_x1half/x10/x30/x120` | 10-second bounded seeks; slow and frame rows refused | recorded-large (Qt has no scan transport; badge-to-speed mapping untraced, so unprovable) |
| Chapter selector in the time page | `videoplayer_display_chapter_*` (`msg_chapter`) | not drawn in the time page. The control matrix gets a native "Chapters" slot (cell (+3,-1), after the Native slot) that opens a list of the server's chapter marks (numbered name, start time as the note, the current chapter marked); Cross seeks to its start. The slot is drawn and focusable only for a Jellyfin video that is seekable and has two or more marks (`videopanel::chapterSelectorVisible`); a local file has no readable chapter data and never gets it, nothing is faked from intervals. The Native Extras row is kept. Chapter images are skipped | native-only, **placement and wording are not firmware-verified** (the slot cell, its "Chapters" plate text and the note are fit; the console's own selector layout is not in the container) |
| Control bar timing | the container has zero animations; the fade lives in the .prx | 3000 ms hold + 250 ms fade (unchanged fit) | unprovable (fit kept) |
| Time cells / progress | 8 + 8 cells of 16x16, `tex_progress_bar`, `tex_progress_bar_in`, jump marker | unchanged; Go To replaces the playing-time cells while active | matches (cell sizes traced) |
| Pause / stop / resume | `msg_quit_playback` "Quit Playback" (Circle), `msg_stop`; "Play from Beginning" and "Resume" are in the XMB item menu (`explore_plugin_full`, `explore_category_video`) | unchanged: Circle quits and remembers the position, the item menu already offers Play from Beginning | matches as far as traced; resume prompt behaviour unprovable |
| Sounds | no `snd_*` string in `videoplayer_plugin.prx` (traced negative) | no cue on panel open or move | matches (negative) |

## Verification

- **Unit tests**: `xmb-video-panel-tests` (new, `tests/video_panel_tests.cpp`, wired in CMake, `tests.yml` and `work/ci-config-build.ps1`): the 26-slot
  order and cells, the single native slot last, 24 distinct console icons, the focus walk (every console slot reachable from Play, the dummy
  never a target, the native slot only reached from Repeat or from below Instant Advance), Go To spin/wrap/range rules, the four Screen Mode
  rectangles, the repeat status keys, the console's two "nothing to choose" keys and the codec-picture mapping. `bun work/ci-guard.ts` and
  `work/ci-config-build.ps1` pass (21 targets built, all runnable tests ok).
- **Frames** (scratch states derived from `subtitle-cue`, real build, 1920x1080, under `C:/dev/out/vplayer-ev/frames`, not in git): closed bar with the
  console key hint; matrix with the native plate last; Audio Options and Subtitle Options panels; Native Extras; Go To (spinner field boxed, jump
  marker, past-the-end sentence, jump to 10 s); Screen Mode Original and Full Screen with the information plate; Repeat with "Title Repeat"; Display
  with the upper bar, `AVC` and `AAC` pictures and `1.3 Mbps 96 kbps` (the clip's own metadata). Local test clip: `Cue Test.mp4`.
- **Gate** (queued behind the shared lock; MSVC /W4, 0 warnings): baseline on the unmodified `main` build 27/28 (only `ingame-xmb`). After the change and
  `git merge main`: 26/28 before saving, the two failures being `subtitle-cue` (frame 34.56 dB / 0.9962: the new Triangle hint text) and `ingame-xmb`
  (52.43 dB / 0.9994, the same numbers as on the unmodified build, also failing alone with `--state ingame-xmb`; not touched by this change).
  `subtitle-cue` was re-saved alone (old reference and sidecar backed up to `C:/dev/out/ref-backup-vplayer`) and then passes. `video-options-panel` is the Video Library
  item menu (not the player) and did not change. No other pinned state moved: the player surfaces draw only while media is open.
- **Chapter selector tests**: `xmb-video-panel-tests` covers the 27-slot table (two native slots, the chapters slot last), the focus walk with and without the slot (hidden: never a target; shown: reachable from the extras slot), the visibility rule (Jellyfin, video, seekable, at least two marks; local file, one mark, no marks, negative count all hidden), target-index validity and the focus fallback. `xmb-jellyfin-playback-tests` now also drives the loopback Jellyfin stand-in with a PlaybackInfo that carries three marks (selector visible, the unnamed middle mark labelled by number) and one that carries none (empty list, selector hidden). No real Jellyfin server was used and no frame of the new slot was captured or compared.
- **Not verified**: a real controller (the pad paths for Go To and the extras slot mirror the keyboard ones but were not driven by hardware); Jellyfin
  streams through the new panels (the shell code paths are the old ones behind a kind switch; local file only was captured, the Jellyfin unit
  tests pass); codec art for HLS/transcoded streams; any console frame (see the method section).

## Left for later (not done here)

- A pad/remote check of Go To on the HTPC; the Jellyfin watched/favourite toggles stay in the XMB item menu (they were never in the player).
- Scene Search thumbnails, AV Settings, Change Icon, scan speeds and slow motion need backends this build does not have (recorded above).
- The chapter selector in the time page (`msg_chapter`) and a resume prompt style: no layout evidence; left alone.
