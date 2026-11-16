# Parity sweep 7 against the real firmware in RPCS3 (2026-11-15)

Third live-console sweep (rig as in [6a](PARITY-SWEEP-6A-RPCS3-2026-11-14.md) and [6b](PARITY-SWEEP-6B-RPCS3-2026-11-14.md)).
Focus: the settings pages 6a captured but did not read row by row (Display, Sound, Network, System, Security, Accessory,
Printer, Power Save, Remote Play) and the settings choice bands (6a/6b had one band frame).

## Method

- Existing 6a frames (`pg-<page>-0..5`, real and native) were compared page by page. Native fixes were measured on new
  native frames against the same real frames.
- New live capture: RPCS3 v0.0.43 with the 6b scratch config; `Default.yml` swapped for the keyboard mapping and restored
  (sha256 `e624da0d...ceee`), `config.yml` untouched (`22d8ffe2...`), `xRegistry.sys` rewritten by the console and restored
  byte for byte with its mtime (`20916738...c2aa`). Bands captured for Date and Time, Accessory, Sound, Display, Chat, Game,
  Power Save, Video and Music pages (Cross on rows, Circle back). Scratch output under `C:\dev\out\rpcs3-sweep7`.
- Several band frames in that batch are off target (the Accessory Cross opened Bluetooth and camera prompts, the Chat run
  landed on the Photo column, one frame was covered by another window); only frames confirmed on their page are used.

## Fixed (4)

| Item | Evidence | Where |
|---|---|---|
| Chat Settings and Printer Settings icons in the Settings column and on their pages were drawn from the flat 128 px masks (`tex_chat`, `tex_printer`), about 0.86 of the console's size, with a smaller wrench disc and a pale grey body. The console draws the same wrench-disc normal maps as the other rows: `qgl/icontex` `43n` (chat) and `11n` (printer). Real chat ink 85 px wide, native 74 before, 87 after | `ref/pg-printer-0`, `pg-chat-0`, `settings-r4`, `settings-r10` (6a) against `nat/chat1`, `nat/print1` | `shell.cpp` normalMaps |
| Settings choice band left edge: the 2 px bright line at x 1261-1262 is a vertical hump on the console (excess over the fill 0 at y 0-120, 26 at 240, 45 at 360, 56 at 480-600, 26 at 840, 9 at 960, 0 at 1020), identical on the Date, Sound and Display lists. The build had a constant step (+3 luma). After: 6, 28, 45, 53 at y 120, 240, 360, 480 | `ref/date-r3-band` against `nat/date-r3-band` and `nat2/date-r3-band` | `view.cpp` kChoiceBand |
| System Settings listed Install Other OS between Restore PS3 System and System Information; the 4.93 console frames go straight from Restore PS3 System to System Information | `pg-system-5` (6a) | `settings_menu.cpp` |
| Accessory Settings listed Keyboard Entry Method; the console has Keyboard Type, Key Repeat Interval, Key Repeat Speed, Mouse Type, Pointer Speed only | `pg-access-3` (6a) | `settings_menu.cpp` skipped list |

## Matches

Row order, labels and help wraps on Display, Sound, Network, Security, Accessory, Printer, Power Save, Remote Play and System
(apart from the host rows below); band geometry (edge x 1260, 40 px pitch, stored value on the focus row, text x 1276); the
band fill profile (G and B channels equal the native alpha profile, R is higher on the console because of the blurred
wallpaper under it, recorded 6b item 1).

## Recorded

| # | Difference | Triage |
|---|---|---|
| 1 | Values blank in native on most settings rows (console shows defaults) | RECORD (6a item 2 policy) |
| 2 | Native-only host rows: Display (Current Display Mode, HDMI-CEC x2), Sound (Host Volume, Host Mute), Power Save (four Desktop Mode rows), Accessory (Bluetooth Devices), System (System Check) | NOT A DIFFERENCE (HTPC features) |
| 3 | PS Plus badge beside Automatic Update on the System page is missing (about x 505-540) | RECORD (asset not wired) |
| 4 | Keyboard Type value "English Keyboard (US Standard)" native "English (United States)"; Key Repeat Speed, Media Server Connection, System Name show host values | RECORD (host readings) |
| 5 | Daylight Saving, Crossfade-style rows open an information page on native, a 2-row band on the console | RECORD (6b item 9) |
| 6 | Band panel is not blurred and carries particles on native (6b item 1); bands with more rows than fit (Video audio language, 13+ rows) show a "more" arrow at the bottom on the console; native not compared | RECORD |
| 7 | Accessory Cross rows on the console open Bluetooth registration and camera pages (Yes focused on the first) | not compared |

## Not reviewed

Real on-screen keyboard, Network sub-pages, native-only surfaces (Steam account page, Jellyfin setup rows, folder browser,
OSK paste key) have no console counterpart; the folder browser and OSK were not captured this sweep.

## Gate and changed states

Removing Install Other OS made the System page 33 rows. The recipe `settings-autologon-row` lost one `preGrab` DOWN and its `entryCount` went 34 to 33; `settings-autologon-info` got `rowIndex` 29. Reference updated for exactly one state, `settings-autologon-row` (list region only; the focused-row region is identical), backup in `C:\dev\out\ref-backup-sweep7\`. `settings-autologon-info` is pixel-identical. Chat/Printer icons and the band edge touch no pinned frame.

Gate on the final build: 23/28. The five failures fail identically on an unmodified build and are other agents' shared-reference updates not on this branch: video-options-panel, continue-watching, continue-watching-empty (library chooser) and jellyfin-signin, steam-download-page (Jellyfin icon work). Re-run after merging main.
