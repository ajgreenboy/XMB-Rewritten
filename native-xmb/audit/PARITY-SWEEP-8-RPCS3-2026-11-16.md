# Parity sweep 8 against the real firmware in RPCS3 (2026-11-16)

Fourth live-console sweep (rig as in [6a](PARITY-SWEEP-6A-RPCS3-2026-11-14.md), [6b](PARITY-SWEEP-6B-RPCS3-2026-11-14.md) and
[7](PARITY-SWEEP-7-RPCS3-2026-11-15.md)). Focus: what 5 to 7 did not cover: the Photo, Music and Video columns and their
pages, the Network, Friends and Game columns' RCO rows, Triangle menus per column, message and confirm pages, and the depth-1
item page icon geometry (6a item 4).

## Method

- RPCS3 v0.0.43, scratch `capture-config.yml` (1920x1080, Null audio), `Default.yml` swapped for the keyboard mapping. Cross,
  Circle and Triangle were pressed only on rows identified first. Scratch output under `C:\dev\out\rpcs3-sweep8` (not in git).
- Real frames: every row of Photo (6), Music (6), Video (8 steps), Network (6), Friends (2), Game (9) with Triangle on each;
  Cross on Online Instruction Manuals, Game Data Utility, Memory Card Utility, Saved Data Utility (PS3), Music Playlists and
  Photo Search for Media Servers. Native frames with `--script` in scratch data dirs.
- Rig restored: `Default.yml` sha256 `e624da0d...ceee`, `config.yml` `22d8ffe2...`, `xRegistry.sys` rewritten by the console
  and restored byte for byte with its mtime (`20916738...c2aa`). No RPCS3 process left. I created and deleted nothing in the
  console's storage.
- Pitfall found: the copied `nat.ps1` still pointed at the sweep 7 exe. The native frames behind each fix below were retaken
  with the sweep 8 build; the row-list comparisons (Photo, Music, Video, Network) used the older exe, whose rows are the same.

## Fixed (3)

| Item | Evidence | Where |
|---|---|---|
| Search for Media Servers wait page body: the console shows dlna_plugin `msg_searching` ("Scanning..." / "Please wait.", two lines centred at x 960, y about 520); the build invented "Searching for media servers..." | `ref/msrv-x1` against `nat/msrv2-a` (after) | `dlna_shell.cpp` |
| Search for Media Servers "not found" page had no heading: the console keeps the row's icon and title ("Search for Media Servers", icon x 112, title y 178) on the result page as on the wait page; the build drew body text only | `ref/msrv-x2` against `nat/msrv-b` (before) and `nat/msrv2-b` (after). The body text differs on purpose: the rig has no Ethernet link ("An Ethernet cable is not connected."), the build says "No media servers were found." (the dlna_plugin string for a connected, empty LAN) | `dlna_shell.cpp`, `shell.h` |
| Network column Remote Play row had no help text. The console shows explore_category_network `msg_remoteplay_explanation` under the focused row, 3 lines. The row is the bare plugin item in the XMBML (no `info_rsc`), so none was set. The Network column's help box is also the narrow one (line 1 ends at "...operate this system over"); the build's advances run about 5 percent wider, so the box is 1005 (a fit, like the 965 used for the Settings column) and the three line breaks now match | `ref/net-1` against `nat/net-rp` (before) and `nat/net-rp3` (after) | `shell.cpp` (resolveNetworkColumn), `view.cpp` |

## Matches

- Photo, Music, Video and Network column rows and order of the firmware rows (Photo: Search for Media Servers, Photo Gallery,
  Playlists; Music: Search for Media Servers, Playlists; Video: BD Data Utility, Search for Media Servers, Video Editor &
  Uploader; Network: Online Instruction Manuals, Remote Play, Internet Browser, Internet Search); icon shapes and positions,
  label x, row pitch, the focus enlargement. Photo Gallery's row icon is the firmware's. BD Data Utility Triangle: Delete and
  Information on the console; the build has no BD data store, so only Information (6a item 3).
- Game column firmware rows (6b): order and icons; Game Data Utility opens the "There is no game data." page on both.
- Search for Media Servers page chrome: rules at y 199 and 879, `O Back` legend, centred body.

## Recorded

| # | Difference | Triage |
|---|---|---|
| 1 | Triangle menus: the console has none on any Photo, Music, Network or Friends row (nor Photo Gallery, Playlists, Search for Media Servers, Video Editor); BD Data Utility offers Delete/Information; the Game column's Corrupted Data row offers Sort By, Group Content, Start, Delete, Information. The build offers Information (plus Start or Open) on every row, including Network and Friends | RECORD (6a item 3; the Information page is this build's explanation channel, needs a decision) |
| 2 | Depth-1 item page icon geometry (6a item 4). Measured again on Music > Playlists and Game > Memory Card Utility: console focused icon ink about 119 px (box 128), centre x about 629, label x 740, parent arrow about 20 px left of the build's; build 54 px at centre 565, label 625. Not changed: unfocused rows at depth 1 cannot be captured (every depth-1 page the console offers here has one row; a second would need a created playlist or memory card), so only the focused row's class is known. Changing the focused row alone would put the focused label at 740 and its neighbours at 625, and the level-0 class (50 px unfocused, label centre+113) is only an analogy for the unfocused rows. It would also move text, not only icons, in the All Games, Not Installed, Store and Video Library options frames | RECORD (needs a console page with 2 or more depth-1 rows. With one, the change would be: level>0 non-bullet rows use the level-0 sizes (50+78*focus), centre x +64, label +111, arrow -20) |
| 3 | Photo Gallery row: after about 1 s focused the console swaps the background for the gallery welcome panel (black field, sample photos, camera, "Photo Gallery" with a 5-line explanation). Seen again; the build shows nothing | RECORD (6a item 5, needs firmware scene images) |
| 4 | Online Instruction Manuals: the console opens its Internet Browser page ("The page cannot be displayed. (80710102)" offline; closing asks "Do you want to close the browser?" with Yes focused). The build lists the row as unavailable with a reason | RECORD (no browser. The dialog class is 6b item 3: dark page, rules y 199/879, `X Enter` / `O Back`, text y 502, Yes x 882, No x 1040, y 576) |
| 5 | Saved Data Utility (PS3) opens a page whose first row is Online Storage (cloud/PS Plus icon, "- MB / - MB", 3-line explanation, Options hint); the build's page has no such row | RECORD (PS Plus asset path not wired) |
| 6 | Friends column: empty on the console (offline); the build lists its host rows (Add a Friend ... Discord Connection), so the RCO rows cannot be compared in place | RECORD (6b item 6) |
| 7 | Game column on the console ends with a "Corrupted Data" row (RPCS3 storage left from earlier sessions) | NOT A DIFFERENCE (state of the rig) |
| 8 | Wait pages: after a failed scan the console dims the background further than the build does on the same page | RECORD (small, one frame only) |
| 9 | Native-only: Choose Media Folder, Continue Watching, Library, Settings rows in the media columns; the folder chooser page's first row "Use This Folder" (check glyph, path as subtitle) | NOT A DIFFERENCE (HTPC features; the console has no such rows) |

## Gate and changed states

The three fixes touch the Search for Media Servers pages and the Network column's focused Remote Play row; none is a pinned
state, so no reference was updated. Gate result is in the commit message.
