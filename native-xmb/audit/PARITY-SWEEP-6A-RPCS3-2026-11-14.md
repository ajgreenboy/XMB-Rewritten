# Parity sweep 6a against the real firmware in RPCS3 (2026-11-14)

First sweep that compares the native build with frames of the real 4.93 XMB captured in the same session, for the Users,
Settings, Photo, Music and Video columns. (Sweeps 2 to 5 compared the native build with itself and with older captures.)

## Method

- RPCS3 v0.0.43 (local build) booted with `rpcs3.exe --no-gui --config <scratch capture-config.yml> vsh.self`. The scratch
  config is a copy of `config.yml` with `Resolution: 1920x1080`, `Exclusive Fullscreen Mode: Disable` and the audio renderer
  set to Null (nothing played, this PC's audio was not touched). Client area exactly 1920x1080.
- Keys: the global `Default.yml` was swapped for a Keyboard mapping (Cross PgDn, Circle PgUp, Triangle Insert, Square Delete)
  and restored afterwards from a copy (sha256 `e624da0d...ceee`, verified). `config.yml` untouched (`22d8ffe2...`).
- Rig written for this sweep under `C:\dev\out\rpcs3-sweep6\tools` (`rig.ps1`: SendInput keys, `CopyFromScreen` stills, timed
  bursts; `nat.ps1`: scripted native run; `sheet.ps1`: contact sheets). The 2026-10-28 capture roots and tools no longer existed.
- Native: build of `3cbbc98` plus the fix below, `--script` runs in scratch data dirs with the dry-run env vars. Cross was only
  pressed on rows whose ids a look-only pass had printed (Settings page rows, Playlists, BD Data Utility, Video Editor).
  Nothing that acts on the network or system was confirmed (DLNA scan, power rows, Steam, Spotify).
- Captured and compared: every row of Users, Settings, Photo, Music, Video (cursor walk, 5 to 18 frames per column); the top
  and scrolled frames of all 15 Settings sub-pages (90 frames each side); Triangle on 11 real rows and 16 native rows;
  Cross on Playlists, Search for Media Servers, BD Data Utility, Video Editor (real); idle-dim timelines; one row-focus motion
  burst on each side; the Users information page.

## Fixed

| Item | Evidence | Commit |
|---|---|---|
| Settings column help line wrapped too wide. Remote Play Settings: real line 1 ends "...supports remote play" (ink x 682..1583), native ran to x 1854 and broke after "(such as a PS Vita or a" | `ref/settings-r14.png` vs `nat/settings-r14.png` before, `nat/fix1-after2.png` after: break now identical | `c0004d4` |

## Matches (measured or seen)

- Row sets and order of the Settings column (16 rows), and of all 15 sub-pages for the firmware rows; row pitch, label x 625,
  value column x 1300, parent icon, wrench bullets, focus description size and position (Game, Video, Music, Chat, Theme,
  Date and Time compared frame by frame).
- Row-focus motion, Music column DOWN: native settles within 1 px about 320 ms after the key; the console within 1 px about 340
  ms after the key (including its input latency). Shapes agree, difference under 35 ms: not a difference.
- Idle dim: on the console the labels of unfocused rows fade after about 4 s (Video column; visible at 3 s, gone by 8 s, faint
  at 5 s), and do not fade in the Settings or Users columns within 40 s. This confirms FIDELITY-ROOT-BAR I12 / FIDELITY-SURFACES 2.5
  (4 s + 0.8 s ramp) for the media columns. Not re-checked: whether native also leaves the Users column undimmed.
- Item menu geometry for the single Information row (band left x ~1330, text x 1335, first row y ~549) and the *User menu
  contents (Information only).
- Playlists page content ("Create New Playlist" only), "Create New Playlist" label text.

## Differences found and triage (11)

| # | Difference | Triage |
|---|---|---|
| 1 | Settings column help wrap width (above) | FIXED |
| 2 | Value column: rows without an honest host reading show no value; the console shows its defaults (Video page: English, Confirm, Automatic, Letterbox, Off, Linear PCM; Music: AAC 128 kbps, 3 seconds, Type 1, Off; Chat: Send Video) | RECORD. Policy in `host.cpp` ("no value rather than a plausible one", SETTINGS-COVERAGE section 4); the optical-output row is blank on the console too |
| 3 | Triangle menu: the console has none on Settings, Photo, Music rows or on Search for Media Servers / Video Editor / Turn Off / Create New User; BD Data Utility offers Delete and Information, *User offers Information. Native offers Start or Open plus Information on every row | RECORD. The Information page is this build's explanation channel; needs a decision |
| 4 | Depth-1 item pages (Create New Playlist / Create New Video / Create New Internal Memory Card): console focused icon is about 118 px ink (128 box) with centre x 629 and label x 740 (icon centre + 111); native draws 54 px at centre 565 with label x 625 (`view.cpp` level>0 size 40+16*focus, label +60). Three console pages agree; unfocused rows at depth 1 not captured | RECORD. Changing it moves the folder pages in several pinned states (All Games, Not Installed, Store, Video Library options); needs the reference decision |
| 5 | Photo Gallery row: after about 1 s focused on an empty gallery the console swaps the background for a black field with sample photos, a large blue camera and the "Create a space to enjoy your photos..." panel; native shows nothing | RECORD (new surface, needs firmware assets) |
| 6 | User Information page: console shows an avatar card (rules y 199 and 879, "User" and "*User", `X Edit`, `O Back`); native opens the Icon / User Name list directly | RECORD |
| 7 | Users column label "*User" on the console, "User" native | RECORD (meaning of the asterisk unverified) |
| 8 | Native-only description line under focused media rows (Search for Media Servers, BD Data Utility, Video Editor); the console has none | RECORD (explains unavailable rows) |
| 9 | Native-only rows: Users (Restart, Sleep, Switch/Exit Desktop, Steam Account), Photo/Music/Video (Library, Choose Media Folder, Settings, Spotify, Continue Watching), Video page (Repeat, Shuffle, Volume), Settings pages (host rows), TV column | NOT A DIFFERENCE (HTPC features; the TV column needs hardware the console rig lacks) |
| 10 | Clock text format, Set Automatically value, time zone text | NOT A DIFFERENCE (rig locale and xRegistry values) |
| 11 | Search for Media Servers: console shows a "Scanning... Please wait." wait page with title rule and `O Back` | not compared: native Cross runs a real LAN scan |

## Not reviewed

Choice bands (Display > Screensaver etc.), message boxes and the Options hint timing (the one real band frame was missed by
navigation), clock-bar fade of rows passing under it, column-switch bursts, other Settings pages' scrolled tails beyond the
frames taken, Create New User keyboard, the Game/Network/PSN/Friends columns (sweep 6b), any sound. The page-by-page sheets were
read for Game, Video, Music, Chat, Theme, Date and Time; the rest were captured but not examined row by row.

## Rig notes

- RPCS3 booted and was driven without trouble; one instance, stopped by pid; `Get-Process rpcs3` is empty.
- The console rewrote `dev_flash2/etc/xRegistry.sys` by itself (Settings pages open); it was backed up first and restored
  byte for byte with its mtime (sha256 `20916738...c2aa`). I pressed Cross on a few Game column rows by mistake (Memory Card
  Utility etc.); only Circle followed, nothing was created.
- `dev_hdd0` was not written by me; any writes by RPCS3 itself were not checked.
