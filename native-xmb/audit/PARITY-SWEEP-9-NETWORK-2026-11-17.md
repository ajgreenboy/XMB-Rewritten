# Parity sweep 9 — Network column against the real 4.93 firmware in RPCS3 (2026-11-17)

Fifth live-console sweep (rig as in sweeps 6a/6b/7/8). Focus: the Network column only, row by row —
row order, row labels, icons and the focused-row help text — comparing the real XMB in RPCS3 against
the native build's Network column, and against what `resolveNetworkColumn()` intends.

## Method

- **Console.** RPCS3 v0.0.43 already running the real 4.93 XMB (`vsh.self`). I used the capture rig
  (`C:\dev\out\rpcs3-capture\rig.ps1`): `Rig-Key` for D-pad only (no Cross/Triangle, per the safety
  rule) and `Rig-Grab` for 1920x1065 frames into `C:\dev\out\rpcs3-capture\ref\`. Navigation was
  Circle x2 (back out), LEFT x12 (clamp to Users), RIGHT x6 (to Network), UP x12 (clamp to row 0),
  then DOWN with a grab after each step.
- **Native.** Built with `native-xmb\build.ps1 -OutputDirectory C:/dev/out/wt-sweep` and run headless
  with `--dev-flash C:/dev/rpcs3/bin/dev_flash --data-dir C:/dev/out/wt-sweep/data
  --vsh-modules C:/dev/xmb/fw493/vsh_module`, env `XMB_STEAM_ARTWORK=0 XMB_STEAM_ACTIVITY=0
  XMB_SINGLE_INSTANCE=0 XMB_POWER_DRY_RUN=1 XMB_AUDIO_DRY_RUN=1`. Two evidence channels:
  `XMB_DUMP_ENTRIES` (the resolved column: id/title/detail/action/source/texture per row) and
  `--script` with `grab=` tokens (each grab writes the 1920x1080 frame plus a `.json` state sidecar
  carrying `category`, `categoryIndex`, `entryId`, `entryTitle`, `entryCount`, `selection`).
- **Authority for the console side.** The console runs the real firmware, so its column is exactly
  what `dev_flash/vsh/resource/explore/xmb/category_network.xml` (7 `<Query>` rows) and
  `explore_category_network.rco`'s English string table define. I read both directly
  (`work/rco-table-dump.ts --text/--images`). The console row count was cross-checked by frame-diff
  (PSNR on a tight crop of the focused-row band) over the sweep-8 console frames (`ref/net-0..net-5`,
  vision-verified as the Network column): 4 distinct focus moves then a clamp = 5 rows.

> **Vision note.** This sweep's agent cannot view images, so pixel-level geometry (icon/label
> positions, focus enlargement, help-text line-wrapping) was **not** verified from frames alone. The
> comparison below rests on (a) the firmware's own XMBML + RCO strings (authoritative for the
> console), (b) the native's dump + sidecar (authoritative for the native), (c) sweep 8's
> vision-verified notes, and (d) frame-diff analysis for the console row count. Section "Not verified
> from frames" lists what remains open.

## The two columns

Console (real 4.93, this rig) — 5 rows, in display order:

| # | Label | Icon (RCO) | Focused-row help text |
|---|---|---|---|
| 0 | Online Instruction Manuals | `tex_onlinemanual` | *(none)* |
| 1 | Remote Play | `tex_premo` | `msg_remoteplay_explanation` (3 lines) |
| 2 | Internet Browser | `tex_browser` | *(none)* |
| 3 | Internet Search | `tex_kensaku` | *(none)* |
| 4 | Download Management | `tex_dl_manage` | *(none)* |

Native (`re-ps3-native`, this build) — 11 rows, from `XMB_DUMP_ENTRIES`:

| # | id | title | action | source | texture | detail |
|---|---|---|---|---|---|---|
| 0 | manual | Online Instruction Manuals | 0 (Unavailable) | xmbml | tex_onlinemanual | *(empty)* |
| 1 | premo | Remote Play | 0 (Unavailable) | rco | tex_premo | msg_remoteplay_explanation |
| 2 | browser | Internet Browser | 0 (Unavailable) | xmbml | tex_browser | *(empty)* |
| 3 | kensaku | Internet Search | 0 (Unavailable) | xmbml | tex_kensaku | *(empty)* |
| 4 | dlctrl | Download Management | 31 (SteamDownloads) | rco | tex_dl_manage | *(empty)* |
| 5 | tailscale | Tailscale | 70 | native | item_tex_plain_folder | "Connected as al-workstation - 100.118.251.42" |
| 6 | wol | Wake on LAN | 75 | native | item_tex_plain_folder | "Wake a computer on this network" |
| 7 | homeassistant | Home Assistant | 79 | native | item_tex_plain_folder | "Scenes, lights and media" |
| 8 | weather | Weather | 82 | native | item_tex_plain_folder | "Not set up yet." |
| 9 | phone-remote | Phone Remote | 97 | native | item_tex_plain_folder | *(empty)* |
| 10 | wifi | Wi-Fi Networks | 99 | native | item_tex_plain_folder | *(empty)* |

`--script` sidecars confirm the walk: selection 0..10 = manual→premo→browser→kensaku→dlctrl→tailscale
→wol→homeassistant→weather→phone-remote→wifi, then DOWN clamps at wifi (selection 10) for r11/r12.
The same 11-row list and order appeared in sweep 8's native capture (`nat/net-0..net-6` sidecars,
`entryCount=11`), so it is not new to this build.

## Differences

| # | Console | Native | Triage |
|---|---|---|---|
| 1 | 5 rows (the firmware's five) | 11 rows: the same five plus six native-only rows — Tailscale, Wake on LAN, Home Assistant, Weather, Phone Remote, Wi-Fi Networks (all `item_tex_plain_folder`) | **Native-only HTPC rows.** Same class as the media columns' native rows (sweep 8 recorded those as "NOT A DIFFERENCE"), but sweep 8 did *not* record the Network column's native rows; this sweep documents them. The console has no equivalent for any of the six. |
| 2 | Download Management opens the console's download queue (the `ese://download_control` provider) | Download Management is mapped to `Action::SteamDownloads` — it opens Steam's download queue, not a PS3 queue | **Known/intentional** (`STEAM-DOWNLOADS.md`). Label, icon and position match; only the action differs. |
| 3 | No Triangle menu on any Network row | Every Network row (and Friends row) offers an "Information" Triangle menu | **Recorded** in sweep 8 (item 1); unchanged this sweep. |
| 4 | Boots to the Users column | Boots to the Game column (`Shell::category_ = 6`, `src/shell.h:1429`) | **Outside the Network column**, noted for completeness. |

The six native-only rows come from `addInstalledApps()`'s service rows (Tailscale/Wake on LAN/Home
Assistant via `home_shell.cpp` + `tailscale_shell.cpp`, Weather via `weather_shell.cpp`, Phone Remote
via `phone_remote_shell.cpp`, Wi-Fi via `wifi_shell.cpp`), which append after the firmware rows. They
do not come from `resolveNetworkColumn()` (that resolves exactly 5 rows, logged
`xmbml: network column resolved 5 rows from category_network.xml`).

## Matches

- **Row order** of the five firmware rows (Online Instruction Manuals, Remote Play, Internet Browser,
  Internet Search, Download Management) — identical.
- **Row labels** — identical RCO English strings (`msg_onlinemanual`, `msg_psp_connection`,
  `msg_browser`, `msg_internet_search`, `msg_download_control`).
- **Icons** for the five firmware rows — identical RCO textures (`tex_onlinemanual`, `tex_premo`,
  `tex_browser`, `tex_kensaku`, `tex_dl_manage`), all 128x128 in `explore_category_network.rco` and
  all decoded by both sides.
- **Focused-row help text** — only Remote Play carries a description (`msg_remoteplay_explanation`,
  3 lines) on both sides; the other four firmware rows show no description on either. This was the
  sweep-8 fix ("Network column Remote Play row had no help text") and is still correct.

## Not verified from frames alone

- **Pixel geometry**: icon/label x-y positions, the focus enlargement, row pitch, and the help-text
  box width / line wrapping on either side (this agent has no image input). Sweep 8's vision pass
  already confirmed "icon shapes and positions, label x, row pitch, the focus enlargement" for the
  four rows it listed, and the help-box width fix (`view.cpp`) for Remote Play; those were not
  re-measured here.
- **Console row count** is 5 by frame-diff + firmware + sweep 8, but I could not visually re-confirm
  the Download Management row's presence/absence in my own `net-r0..r6` grabs (the sweep-8 ref frames
  give the cleaner signal, with a clear clamp after 4 moves).
- **Whether the console hides "Download Management" when the download queue is empty** — the firmware
  query is the dynamic `ese://` provider, so an empty queue could suppress it on some states; this rig
  showed it (5 rows), but the empty-queue visibility rule was not exercised.

## Captured frames and evidence

Console (`C:\dev\out\rpcs3-capture\ref\`): `net-state0.png` (pre-navigation check), `net-r0.png` …
`net-r6.png` (7 frames: rows 0-4 plus two clamped repeats).

Native (`C:\dev\out\wt-sweep\nat\`): `net-r0.png` … `net-r12.png` (13 frames + `.png.json` sidecars),
`diag-start.png`, `diag-left.png`, `diag-right1.png` … `diag-right8.png` (navigation map).

Dumps/logs (`C:\dev\out\wt-sweep\`): `net-dump.json` (resolved catalogue), `net-run.log`,
`nat-run.log`, `nat-diag.log` (xmbml resolution + `--script` state traces).

## Gate / states

No source was changed for any difference above (evidence-gathering only). The build in
`C:\dev\out\wt-sweep` is the committed `wt/sweep` source; `resolveNetworkColumn()`'s behaviour is
unchanged and matches `XMBML-NETWORK.md`.
