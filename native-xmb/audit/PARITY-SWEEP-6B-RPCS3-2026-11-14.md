# Parity sweep 6b against the real firmware in RPCS3 (2026-11-14)

Second half of sweep 6 (see [PARITY-SWEEP-6A-RPCS3-2026-11-14.md](PARITY-SWEEP-6A-RPCS3-2026-11-14.md) for the rig). Real
frames of the 4.93 XMB were captured in the same session as the native frames, for the Game, Network, PSN and Friends
columns and for several of the surfaces 6a did not compare.

## Method

- Same rig as 6a: RPCS3 v0.0.43 booted with the scratch `capture-config.yml` (1920x1080 client, Null audio), the global
  `Default.yml` swapped for a Keyboard mapping and restored byte for byte afterwards, driver tools copied from the 6a
  scratch folder and extended (`Rig-Walk`, `Rig-Burst`, a motion and region-mean measurer, `Rig-MoveTo`). Scratch output only
  under `C:\dev\out\rpcs3-sweep6b\`.
- Another agent's `re-ps3-native --firmware-install` run kept an error message box on the primary display, on top of the
  RPCS3 window, for most of the sweep. Early frames show it over the focused row. It was avoided by moving the RPCS3 window
  to the second display (a borderless window, client still exactly 1920x1080); later frames are clean.
- Cross was pressed only on rows identified first: the Users column's Turn Off System (the confirm opens with **Yes**
  focused, so it was cancelled with Circle at once), Create New User (opens the name page; the field was not edited, since
  Edit versus OK focus could not be proven and OK would create a user) and the Music Settings page and its Crossfade
  Playback list (cancelled with Circle). Triangle was pressed on every Game column row.
- Native: build of the sweep branch, `--script` runs in scratch data dirs with the dry-run env vars. One script reached the
  native Restart System row instead of a Users row it was aiming at; its confirm box starts on No and was not confirmed.
- Measured numbers come from timed full-frame bursts (about 16.5 ms frame spacing) and region means.

## Captured and compared

Game column (all 7 real rows, Triangle on each), Network (4 rows), PSN (5 rows, clean frames), Friends (empty on the
console), the Users column and its Turn Off System confirm, the Create New User name page, the Options (Triangle) menu on
the one Game row that has one, Options hint timing, a column-switch burst, the Photo Gallery intro timing, one Settings
choice band.

## Fixed (3)

| Item | Evidence | Commit |
|---|---|---|
| Users column Turn Off System: the console asks "Do you want to turn off the system?" with Yes focused; the build opened the PS-hold menu's black "Turn Off the System?" screen with a native-only line ("X turns the XMB off; the PC stays on") | `ref/off-x-clean.png` against `nat/u-off1.png` before and `nat/u-off1-after.png` after. Text and default answer now match; the surface style is still the build's confirm box (recorded 3) | `c8e1304` |
| Create New User name page had no rules; the console draws the page-class pair at y 199 and 879 | `ref/cnu-x-1500.png` against `nat/cnu-x.png` before and `nat/cnu-x-after.png` after | `21931c6` |
| Trophy Collection carried a "*User" second line; the console draws none (the line stays on Saved Data (PS3) and the Vita and minis/PSP rows) | `ref/psn2-2.png`, `ref/psn2-3.png` against `nat/p-1.png` before and `nat/psn-trophy-after.png` after | `ccee8cd` |

The first fix changes one thing worth knowing: the console's default answer is Yes, so on the HTPC a second Cross on Turn Off
System confirms (in `--kiosk`, shuts the PC down). That is the console's behaviour.

## Matches (measured)

- Options hint timing: on the console the "Triangle : Options" plate fades in between 0.7 and 0.9 s into a burst that began
  about 0.3 s after the key and out between 3.9 and 4.1 s, each ramp about 0.2 s. The build's traced timers (1000 ms, 200 ms
  fade, 3000 ms life, 200 ms fade) agree within the burst's start uncertainty. NOT A DIFFERENCE.
- Idle dim of rows above the focus (about 4 s) seen again on the Game column, same as 6a.
- Column switch (Game to Network, icon strip): the console and the build settle to within 1 grey level of the final frame about
  390 ms after the key and the curves have the same shape; the build leads by about 20 ms (the rig's input latency). NOT A
  DIFFERENCE.
- Network column: rows, order, labels, focused Remote Play help (3 lines). The build adds Download Management and HTPC rows
  (not a difference).
- Game column rows both sides have (Game Data Utility, Memory Card Utility, Saved Data Utility (PS3), PlayStation Store):
  order, icons, labels; Triangle gives no menu on any of them on the console.
- Name page: field, OK button, heading and legend positions.

## Differences recorded (9)

| # | Difference | Triage |
|---|---|---|
| 1 | Backdrop blur: behind every modal surface the console blurs the wallpaper (the wave is soft, particles are gone): confirm and message pages, the name page, and the Options panel. The build draws the wave crisp with particles. In the Options panel the console region is almost uniform (R 146, G 95, B 75 at x 1400, all y) while the build's plate adds a whitish veil (R 163, G 128, B 111 at the top, x 1340 to 1500) | RECORD. Needs a framebuffer blur path; would change the pinned video-options-panel and dialog frames |
| 2 | Options menu on a game tile: console rows Sort By, Group Content (with the purple square glyph), then Start, Delete, Information. The build shows Sort By but no Group Content on the root Game column (`optionGroupable` is false for that column's mixed rows) and adds Steam rows | RECORD |
| 3 | Confirm page style: the console's Turn Off confirm is a centred page without the item plate (text centred on x 960 at y 502, Yes at x 882, No at x 1040, y 576). The build's box is the plate and text-column box traced from another firmware dialog, so the text wraps to two lines at x 1280 | RECORD (needs a second box layout) |
| 4 | Welcome panels: the PSN rows Sign Up (purple field with sample photos, text `msg_reg_explanation_hd`), What's New (teal, `msg_your_channel_offline_explanation_hd`) and PlayStation Store (blue, `msg_psstore_explanation_hd`) replace the background about 1.0 s after focus (a 200 ms cross fade; the Photo Gallery intro measured the same way, whole-frame mean 126/80/60 to 85/99/109). The strings are in explore_plugin_full; the build shows none | RECORD (new surface, needs the RCO scene images) |
| 5 | PSN rows: console Sign Up, What's New, Trophy Collection, PlayStation Plus, PlayStation Store; build Sign In, Trophy Collection, PlayStation Store (Sign In is native) | RECORD (Plus and What's New have no offline content) |
| 6 | The Friends column is empty on the console (offline, signed out); the build lists six unavailable rows with reasons plus Discord rows | RECORD (6a policy: a reason on every unavailable row) |
| 7 | Game column: the console has PS Vita System Application Utility and Saved Data Utility (minis/PSP) and has no Saved Data Utility (PS2), Artwork Settings, Download Games or Manage rows; the build drops the first two on purpose (see the comment in `shell.cpp`) and keeps PS2 | RECORD |
| 8 | Users column: the console labels the user row "*User" and draws a bullet left of the icon; the build draws the bullet and "User". The asterisk string is in no RCO (searched every vsh/resource container), so code draws it; it is the same user-scope marker the Game rows carry | RECORD (6a item 7 closed as far as the strings go; behaviour with two users unverified) |
| 9 | Settings value rows such as Crossfade Playback open an information page in the build ("No crossfade is applied"); on the console they open a choice band (Off to 10 seconds, band edge x 1260, rows from y 387 at a 40 px pitch, the stored value focused) | RECORD (6a policy, no honest host reading) |

## Not reviewed

The real on-screen keyboard (see Method), Sleep and Restart confirm screens (the console has only Turn Off System; Restart and
Sleep are native rows), clock-bar fade of rows passing under the bar, the depth-1 item page icon geometry (left alone: it would
change pinned frames), Triangle on every Network and PSN row, the contents of the Photo Gallery intro (timing only), any
sub-page of Network or PSN, Photo/Music/Video choice bands other than the one Settings list.

## Rig notes

- First boot showed "The database on the system storage will be rebuilt" (captured as `ref/boot.png`); Cross accepted it and
  RPCS3 rebuilt the database in `dev_hdd0`, then the console quit and was relaunched. That is a write to `dev_hdd0` beyond 6a's
  notes and was not undone.
- `dev_flash2/etc/xRegistry.sys` was rewritten by the console again and restored from its backup byte for byte with its mtime
  (sha256 `20916738...c2aa`). `Default.yml` restored (`e624da0d...ceee`). `config.yml` untouched (`22d8ffe2...`).
- One RPCS3 instance, stopped by its own pid; `Get-Process rpcs3` is empty.
