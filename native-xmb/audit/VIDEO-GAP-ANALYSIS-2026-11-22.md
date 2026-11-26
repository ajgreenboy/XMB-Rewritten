# Video gap analysis: two RPCS3-run XMB recordings vs the native XMB (2026-11-22)

Read-only comparison. No source, harness or other audit was modified. Evidence is frame inspection only (no audio,
no frame-accurate timing except one 100 ms burst), so every "sound", "easing" and "timing" statement is UNKNOWN
unless a frame shows it.

## Sources and method

| Tag | Recording | Frames used |
|---|---|---|
| **M2** | `m2-res_1080p.mp4` (about 58 s; 0-11 s RPCS3 launcher, 12-14 s boot, XMB from 18 s). Older-style firmware look: classic purple PSN globe icon, no dust particles, no info-bar plate. | `C:\dev\out\gapvid\m2\` (1 fps, 1280x720 frames) |
| **R** | `rpcs3_based_full_xmb_psn_now_available.mp4` (about 286 s). Newer-style firmware look: blue PlayStation PSN icon, dust particles, info-bar plate with ticker, nine columns, a PSN service ("RePSN") signed in. Seconds 0-60 are launcher, database rebuild and boot; 66-112 s is the XMB tour; 113 s onward is Call of Duty (game content, out of scope). | `C:\dev\out\gapvid\rpcs3\` (1 fps) and `C:\dev\out\gapvid\fine1\` (100 ms, 83.8-85.6 s) |

Coordinates are in the 1280x720 frames, read by eye (about +/-5 px). Multiply by 1.5 for 1080p. Timestamps are
"M2 26 s" / "R 85 s".

Native-side status comes from `native-xmb/audit/*.md` (FIDELITY-*, BEHAVIOUR-GAP-AUDIT, INFO-BAR, NOTIFICATIONS,
OSK-FIRMWARE, LEGACY-STORE, PARITY-SCORECARD, FIRMWARE-PARITY-GAPS, FEATURE-STATUS-2026-11-04) and from grep of
`native-xmb/src`. Status key: **PRESENT** (implemented and documented), **PARTIAL** (exists but differs or is a stub),
**MISSING** (no string/handler found by grep and no audit claim), **UNKNOWN** (frames do not show enough, or the
native side was not verified). I did not run the native build, so PRESENT means "claimed by audit and visible in
source", not "pixel verified in this pass".

Important context from the audits: the native build deliberately has **no PSN backend**. Items the recordings show
that depend on a signed-in PSN service are therefore MISSING by design, and the question is only whether to fake
the visuals (see Top gaps).

## Top gaps (priority order)

| # | Gap | Priority |
|---|---|---|
| 1 | PSN column: signed-in state (What's New, Trophy Collection, PlayStation Plus, Store, Account Management tree, ticker, avatar, message count, "Signed in/out" toasts) | P1 if PSN-look is wanted, otherwise by design |
| 2 | Game-tile presentation: focused-only labels, date line and "PS3" pill, ICON0 drawn flat with no plate | P1 (verify native; likely close) |
| 3 | PIC1 background timing: recording fades in/out in roughly 150-250 ms; native uses a 2000 ms dwell and 350 ms fade (a documented "native fit") | P1 |
| 4 | Friends column visuals: glossy avatars, status dots, "me" diamond, speech-bubble comment, grey selection plate, Status/Comment/Profile side panel | P2 |
| 5 | Boot: "Sony Computer Entertainment" wordmark stage, epilepsy notice, database-rebuild screens | P2 (epilepsy, SCE), P3 (rebuild) |
| 6 | TV column: native draws one, neither recording does (nine columns, Video then Game) | P2 (known, decision) |
| 7 | PSN sign-in page, "Signing in... Please wait.", Avatar picker dialog, "Update completed." dialog | P3 |
| 8 | Dynamic store background (blue/teal, glow, tilted glass cards) and the "Our Pick" promotional page | P3 |
| 9 | Info-bar message icon and avatar slot have no data source in native | P2 |

## 1. Boot and transitions

| Item | What the recordings show | Native | Priority |
|---|---|---|---|
| "Sony Computer Entertainment" stage | M2 12-14 s: the wordmark at x720-1082, y358 (white, about 22 px cap height, light grey) over a near-black brown background; the ribbon crosses the same band at y330-480 and brightens from dark to terracotta over 3 s; text and ribbon fade together. M2 15-17 s: the wordmark is gone, background fully terracotta, ribbon bright. | UNKNOWN / likely MISSING. No "Sony Computer" string in `src/`. `coldboot.cpp` has the PS3-logo plane; `sce01` there is a shader name, not the text. | P2 |
| PS3 logo stage | R 22-27 s: ribbon fades up out of black (22-23 s), PS family mark + "PS3" + "PlayStation.3" at x760-1190, y325-420 (about 430 px wide), fades in 24 s, ghosts by 26 s while the background warms from dark brown. R 55-59 s repeats it. Logo sits on the ribbon band, not centred. | PRESENT (`coldboot.cpp`, PIPELINE.md, `coldboot_ps3logo` plane). Exact match not re-measured here. | - |
| Epilepsy notice | R 61-65 s: after the logo the background is full-bright terracotta; four uppercase lines "PHOTOSENSITIVE EPILEPSY / IF YOU HAVE A HISTORY..." left-aligned at x214, y325-395, about 16 px white with a soft dark drop shadow, no box, no hairlines. About 5 s. Then the XMB appears (R 66 s) with the ribbon and particles already running. | MISSING. BEHAVIOUR-GAP-AUDIT E19 and FIDELITY-BEHAVIOUR B3 record it as absent (`grep epilepsy src` is empty). | P2 |
| Database rebuild | R 27-45 s: dim background, full-width 1 px hairlines at y133 and y586, text "The database on the system storage will be rebuilt." at y335, glowing "OK" at y383, footer "X Enter" at x490, y604. Then "Rebuilding database... / Do not turn off the system." centred at y~280, thin progress bar (about 660 px wide, olive/yellow fill on grey track) with a percentage under it, 0 to 100% over about 15 s, on a bright terracotta background. | MISSING (no string in `src/`). This is a firmware first-run/recovery flow. | P3 |
| Game launch | R 110-113 s: focused game row with PIC1 as the background, then a full-black screen, then a white-text 3D-safety notice on black (R 114-122 s, game-supplied), then the game. | PRESENT in intent (`launch_cover`, `launch_fade`, GAME-LAUNCH-FADE.md). Sequence not re-compared. | - |
| Column slide / cross-fade | R 86 s, R 88 s, M2 22 s: during a column change the old column's rows are still visible, ghosted, shifted about one column pitch (about 130 px) to the right of the new column's rows while both are partly transparent (for example "Photo Gallery" and "Search for Media Servers" overlapped). | PRESENT (PAGE-MOVE.md, TRANSITION-HARNESS.md, ANIMATION-SMOOTHNESS.md). | - |
| Easing curves, durations | 1 fps frames cannot give curves. | UNKNOWN from these videos; existing audits hold the traced values. | - |

## 2. Root bar: columns, icons, labels, ribbon

R 86 s shows the full bar (the best frame): Users (house with smiley) x66, Settings (toolbox) x200, Photo x338
(focused), Music x477, Video x612, Game x747, Network x879, PSN x1012, Friends x1146; icon centre line y193; column
pitch about 133 px (200 px at 1080p). Focused icon about 58-75 px, neighbours about 42-52 px. Only the focused
column shows a label (y234, about 15 px, white); the label of the column being left fades out (R 86 s "Music" faint).

| Category | In the recordings | Native | Priority |
|---|---|---|---|
| Users | Icon only visible (R 86 s). Column contents never opened. | UNKNOWN for fidelity. Native has a real Users column (FEATURE-STATUS 2026-11-04: create/rename/delete/avatar/log out). | - |
| Settings | Toolbox icon only (R 86 s). Never opened. | UNKNOWN (native: SETTINGS-COVERAGE.md, SETTINGS-FIRMWARE.md). | - |
| Photo | R 86 s: first row "Photo Gallery" (three stacked photos, about 108x100 at x280-395, y288-385) with label x415, then "Playlists" at y453 and "Search for Media Servers". Rows below focus start at y337 (focus), then about +116 px to the next, then about 53 px pitch. | PRESENT (gallery, playlists, DLNA search; PHOTO-GALLERY*.md). Icon art not compared. | - |
| Music | R 87 s: "Search for Media Servers" focused (network-server with magnifier, about 85 px at x338-420), "Playlists" y453. | PRESENT (`dlna_shell.cpp`, `playlists.cpp`). | - |
| Video | R 85.3 s: rows "Video Editor & Uploader" (focus), "Search for Media Servers", "BD Data Utility" above. | PARTIAL. Search/Playlists style rows exist; "Video Editor & Uploader" and "BD Data Utility" have no counterpart (hardware/disc specific). | P3 |
| TV | Not present in either recording. R 86 s shows Video directly followed by Game. | PARTIAL / DIFFERS. Native draws a TV column (FIDELITY-ROOT-BAR C1, X5: "nine columns, TV omitted"). | P2 (decision: hide TV by default) |
| Game | See section 3. | PARTIAL (see section 3). | - |
| Network | M2 22 s, 55 s, R 83 s, 88 s: Online Instruction Manuals ("?" icon), Remote Play (handheld), Internet Browser (dotted "WWW"), Internet Search (magnifier), Information Board (orange RSS tile with symbols), Life with PlayStation (dark orb). | PRESENT (strings in `shell.cpp`). Native also has Download Management, which is extra. | - |
| PSN | See section 5. | PARTIAL (stub). | P1/by design |
| Friends | See section 6. | PARTIAL (Discord-backed). | P2 |
| Ribbon / wave | Wave crest crosses y270-460 at x0-1280, soft white-peach highlight lines, brightest around x300-900. Terracotta vertical gradient (about #6b3220 top to #c0654f bottom right) with a radial brighter patch bottom right. | PRESENT (`ribbon.cpp`, RIBBON-A2/A3.md, many parity sweeps). | - |
| Dust particles | R 66-86 s: 1-3 px bright specks scattered over the whole screen, denser along the ribbon, drifting right and up. M2 shows none. | PRESENT (`particles.cpp`, PARTICLES.md). Density not compared. | - |
| Time-of-day / month colour | Both recordings are terracotta (same month). Store item turns the whole backdrop teal-blue (section 5). | PRESENT for monthly colour (THEMES.md); the content-driven backdrop swap is MISSING (section 5). | P3 |

## 3. Game column and ICON0/PIC1 tiles (Call of Duty, GTA V)

| Item | What the recordings show | Native | Priority |
|---|---|---|---|
| ICON0 look | R 84-86 s, R 110 s: the game art is drawn flat, no frame, no gloss, no drop shadow, no reflection. GTA V logo 128x112 px at x309-437, y280-392 (about 192x168 at 1080p); Call of Duty: Black Ops II logo about 90x26 when unfocused (x330-420, y440-466) and about 200x55 when focused (x278-475, y312-367). | PARTIAL / UNKNOWN. ICON-ARTWORK-SCROLL-2026-11-04.md treats ICON0 as row art; no side-by-side done in this pass. | P1 (verify) |
| Label rule | The title appears only on the focused game row: "Grand Theft Auto V" at x506, y337 (about 21 px white). The unfocused CoD row (R 86 s, y453) has no label at all. Utility rows above the bar (Saved Data Utility, PlayStation Store) keep labels. | UNKNOWN. | P1 (verify) |
| Date line and PS3 pill | Under the focused title: a grey date/time line "21/11/2026 7:34" at y355 (about 12 px, 60 % white) and a "PS3" pill (dark grey rounded rectangle, light border, about 53x14, text "PS3") at x785-838, y355. | MISSING for non-PS3 sources. `grep "PS3" view.cpp shell.cpp` shows only the Saved Data rows. Steam last-played data exists (`steam_activity`) so a date line is feasible. | P1 |
| Rows above | "Saved Data Utility (PS3)" with a tiny "*User" sub-line and "PlayStation Store" (bag icon), 53 px pitch, dimmed. | PRESENT (`shell.cpp` lines near 1878-1881). | - |
| "Triangle : Options" hint | R 84.5-85.6 s: dark translucent rounded pill at x1103-1211, y637-663 (about 108x26), green triangle glyph plus ": Options" in white 16 px. Fades in about 0.2 s after the game row settles. Not shown on utility rows (R 83 s). | PRESENT (`view.cpp` 1853-1924, ink at 1702-1793 x 967-986 at 1080p matches). Show/hide rule per row type UNKNOWN. | - |
| PIC1 full-screen background | R 100 ms burst: Options hint visible from about 84.5 s; PIC1 begins cross-fading in about 84.7 s, about half at 84.8 s, full at 84.9 s (so a fade of roughly 150-250 ms); on leaving the row it is mostly gone by 85.1 s. PIC1 is full-bleed 16:9, behind the bar; the category icons keep their colour but lose contrast; the info plate stays on top. Dwell before the fade is not measurable because the row focus time is before 83.8 s. | PARTIAL. PIC1 support exists (`shell.cpp` 281, `view.h` 334) but with `artworkDwellMs=2000`, `artworkFadeMs=350`, marked "native fit until a frame-timed capture". The recording suggests a much shorter fade (and likely a shorter dwell). | P1 |
| Tile backdrop contrast | R 112 s: on a dark PIC1 (CoD) the bar icons and labels stay white; the focus icon keeps a soft glow. | UNKNOWN. | P3 |

## 4. Info bar, clock, status

| Item | What the recordings show | Native | Priority |
|---|---|---|---|
| Plate geometry | R: translucent rounded plate with a 1 px light border, x887 to the right edge, y51-106 (matches the native `infoBar(1329,75,591,58)` at 1080p). M2 (signed out, older look): no plate, just text. | PRESENT (INFO-BAR.md, FIDELITY-ROOT-BAR F1). | - |
| Clock | R: "21/11 13:46" at x1060-1175, y70 (about 19 px white), analog clock ring icon (about 20 px, white) at x1193. M2: "22/11 9:38" (24 h, no zero pad) and a small dark ring icon at x1190 in place of the clock. | PRESENT (clock format preferences, US default; DD/MM and 24 h selectable). M2's ring icon meaning UNKNOWN. | - |
| Ticker | R 73-109 s: the plate's second line (y90) scrolls the text "Welcome to RePSN: friends, messages, trophies and online play for PS3 games." right to left, clipped to the plate; the plate grows a second row only while it runs. | PARTIAL / UNKNOWN. `view.cpp` mentions a ticker, but INFO-BAR.md states the periodic writer was not found, so no source of ticker text exists. | P2 |
| Avatar and message count | R 73 s: at the left of the plate row, a round mini avatar then a small face icon with the count "1"; after leaving the Friends column the avatar disappears (R 112 s shows only the "1"). | MISSING. `view.cpp` 3745-3779 notes `tex_indi_message` has "no unread-message source" and the avatar slot is "still not drawn". | P2 |
| "Signed in." / "Signed out." toast | M2 20-24 s ("Signed in."), M2 50-54 s ("Signed out."), R 70-77 s: dark translucent grey box (about #3c3c3c at 85 % alpha) x1058-1212, y55-126 (about 154x71), square-ish corners, Friends icon (white pair of faces, 36 px) at left, white text 17 px; it covers the clock; R shows a small ring (countdown) at the top right of the box; lasts about 4-5 s. | PARTIAL. The notification queue and composite exist (NOTIFICATIONS.md, `notify_overlay.cpp`); no PSN producer, and the strings "Signed in." / "Signed out." are not in `src/`. | P2 |
| Battery / power / network indicators | None visible in either recording. | N/A (PS3 has none). | - |

## 5. PSN / Store column

Two different looks. **M2** (older): PSN icon is the purple globe; label "PlayStation(R) Network" in two lines; signed in
rows are Account Management (magnifier-over-face icon, description line "Adjust settings for your
PlayStation(R)Network account or to purchase seats at PlayStation(R)Store"), PlayStation(R)Home, PlayStation(R)Store.
Signed out the first row is "Sign In" (M2 50 s). **R** (newer): blue PlayStation icon, label "RePSN" (service
branding), rows Account Management (R 75 s, 96 s), What's New (orange glossy tile with the four-symbol watermark,
about 106x82 at x323-430, y295-378), Trophy Collection (trophy), PlayStation Plus (yellow plus), PlayStation Store (bag).

| Item | Recording | Native | Priority |
|---|---|---|---|
| Column rows | See above. | PARTIAL / stub. Native PSN column has Sign In, Trophy Collection, PlayStation Store; `shell.cpp` 75-77 shows explicit "does not connect to PlayStation Network" messages. No What's New (FIDELITY-SURFACES-MAP 243 "Nothing in src mentions What's New"), no PlayStation Plus row (strings exist in `shell.cpp`/`view.cpp`, usage not verified), no Account Management. | P1 if PSN look wanted |
| Account Management tree | R 96-99 s: opening it slides the focus icon left (about 85 px icon at x195, y337 with a "<" marker at x283) and lays out a sub-list at icon x415, label x494: Account Information, Redeem Codes (with a one-line description "Redeem a PSN Card number or promotion code." at y357), Transaction Management, System Activation, Facebook, Privacy Settings. Account Information opens Billing Information, Sign-In ID (E-mail Address), Password, Name, Address, Avatar, Profile, Notification Preferences. | MISSING (no such strings in `src/`). | P3 |
| Sign-in page | M2 51 s: hairlines y133/y586; "Sign-In ID (E-mail Address):" label and dark inset field x371-908, y252-277; "Password" with masked field; two blue check boxes "Sign In Automatically (Auto Sign-In)" and "Save Password"; "Sign In" button (glassy, focused with white glow) and "Forgot your password?" button; footer "X Enter / O Back". M2 52 s: centred "Signing in... Please wait." with "O Cancel" footer. | PARTIAL. A Jellyfin sign-in page with similar rows exists (`shell.cpp` ~7243); the PSN wording ("Forgot your password") is MISSING. | P3 |
| Avatar picker dialog | R 100-105 s: title "Avatar" at x232, y68; panel x235-1045, y90-563 (charcoal/brown translucent, 6 px corners, blue glows in the top-left and bottom-right corners, blue 1 px rules at y215 and y503); header with the current avatar (65 px) and arrow, cyan "Select an avatar." (about 22 px), dropdown "PS3 Avatars" (rounded outline, blue circular arrow); grid 6 columns, rows about 120 px apart, items 66 px, focus = white rounded frame; scroll bar at x248; "Cancel" (blue filled, focused) and "Confirm" (dim) at y534; a faint reflection strip below the panel (y570-595); footer band below y605 with "O Back  X Enter" and "SELECT Contact Us / Privacy Policy / Terms of Service". Result: "Update completed." dialog in the same panel with an "OK" button (R 106 s). | MISSING (no "Update completed" or avatar-picker panel; native has a plain row-page icon chooser for local users). | P3 |
| Store backdrop | R 72 s, 77-80 s, 89 s, 95 s: focusing What's New fades the whole backdrop from terracotta to a teal-blue gradient (about #2d6a85 at the top to a bright #7fd0e6 radial glow lower right), the ribbon is not visible, three tilted translucent glass cards with the symbol watermark float at x640-990, y288-480. This cross-fades over about 1 s and reverses on leaving. | PARTIAL. `store_wave.cpp` and LEGACY-STORE.md implement a blue Gen-2 store wave for the in-app store pages; there is no focus-driven backdrop swap on a PSN row. | P3 |
| "Our Pick" promo page | R 79-80 s: opening What's New shows an outlined 333x278 card, a yellow-gradient band x349-1188, y235-463 holding three slots (the first is a 330x190 blue "RePSN" tile with an orange "NEW" badge circle of radius about 24 at x319, y259; the other two are empty rounded outlines 220x185), "Our Pick" label bottom-right, and a grey rounded caption box under the card with two lines of text. | MISSING. | P3 |
| PlayStation Store rows | Present as a bag icon in Game and PSN columns (R 84 s, 112 s). | PRESENT as an icon row; the content is the app's own Gen-2 catalogue (LEGACY-STORE.md), not Sony's. | - |

## 6. Friends column

| Item | Recording | Native | Priority |
|---|---|---|---|
| Static layout | M2 26 s, R 73 s: rows above focus "Start New Chat" and "Message Box" (icons at x377, labels x447). Friend rows: focused avatar about 73 px (M2) or 110 px (R 73 s, x320-430) at x340-413, status dot (about 14 px, blue = online, grey = offline) at x455, name at x470 (about 22 px white, focused bright, unfocused dim). Unfocused friend avatar is a plain 52 px tile (orange square with "B"). | PARTIAL. Native rows: Add a Friend, Players Met, Chat Room (Text Only), Start New Chat, Message Box, Block List (`shell.cpp` ~1890), with Discord friends added when signed in. Avatars/status-dot look not compared. | P2 |
| Row order | M2 45 s: Block List, Add a Friend, Players Met, Start New Chat, Message Box, then the friends (the user's own entry first). The first friend row is the user's own account, marked by a small white diamond at x340 (M2) / x290 (R 73 s). | PARTIAL. Same fixed rows (native includes Chat Room, which these recordings do not show); the "me" diamond is MISSING. | P2 |
| Selection plate | R 73 s, 82 s, 91 s: the focused friend sits on a translucent grey-white rounded plate x320-850, y279-395 with a light 1 px border and a small star at its right end (R 82 s). M2 shows no plate, only a darker band while the Triangle menu opens (M2 33 s). | UNKNOWN / MISSING. | P3 |
| Speech bubble | M2 42 s, R 90 s: when the bot friend is focused for about a second a grey-blue rounded speech bubble with a tail pointing at the name appears above it (x471-770, y258-306), white 15 px text "Send me a message and I will echo it back." | MISSING (`bubble`/`speech` only appear in `web_launch.cpp`). | P3 |
| Triangle menu | M2 33-35 s: a full-height panel slides in from the right: left edge a 1 px white line at x881, fill a lighter vertical-gradient glass, rows "Status >", "Comment", "Profile" at x890, y367/393/420 (27 px pitch, white 16 px, a small arrow at the selected row). The focused avatar enlarges and the rest dims. | PARTIAL. The options panel mechanism is PRESENT (OPTIONS-MENU.md); the Friends rows' content (Status/Comment/Profile) is MISSING. | P2 |
| Comment entry | M2 36 s: dimmed dark page with hairlines y133 and y586 and a centred title "Comment" (about y665 at the start, centred) and "X Edit / O Back" footer. | PRESENT in form (text-entry pages with hairlines); Comment content N/A. | - |
| On-screen keyboard | M2 37-39 s: window x330-950, y193-527; light-grey title bar (pencil icon + "Comment"), dark input row with a "<" cursor marker, 10-column key grid on light grey keys (1234567890, qwertyuiop, asdfghjkl', zxcvbnm,.?), the focused key white with glow; function rows with colour-coded keys (yellow symbol key "*%#", L2 "a/a", R2 "a/A", blue Backspace and Space, arrows, L1/R1, green Input Mode (SELECT) and Enter (START), "Paste"); on the right a grey Prediction panel with footer "Prediction a 0"; background darkens to near-black brown with the ribbon still faintly visible. Footer "X Edit  O Back". | PRESENT (OSK-FIRMWARE.md recovered the 10-column layout, prediction column, Input Mode, copy/paste window; `osk_layout.*`, `osk_paste.cpp`). Prediction candidates content UNKNOWN. | P3 |
| Presence/chat flows | No chat window, message list or friend profile page is opened in either recording. | UNKNOWN. | - |

## 7. Submenus, context menus, dialogs, OSK

| Item | Recording | Native | Priority |
|---|---|---|---|
| Full-screen message page | M2 36 s, 40 s, 51-52 s, R 28 s: dim background, full-width hairlines at y133 and y586 (200 and 880 at 1080p), content centred between, glowing white "OK"/button text, button-guide footer at y604 ("X Enter", "O Back", "O Cancel"). | PRESENT (`dialogs.cpp`, FIDELITY-SURFACES.md). | - |
| Framed dialog (Avatar) | See section 5. | MISSING as a style. | P3 |
| Triangle options panel | Section 6 (right panel, x881 to edge). | PRESENT. | - |
| Column push (depth-2 pages) | R 96-99 s (Account Management): parent column icon shrinks left, child rows aligned at x415/x494. | PRESENT (PAGE-MOVE.md). | - |
| Notifications | Section 4. | PARTIAL. | P2 |
| Trophies | Only the "Trophy Collection" row is visible; no trophy pop-up, no trophy list. | UNKNOWN from video; native is a stub row. | P3 |

## 8. Sound

No audio was analysed. UNKNOWN for all sounds (cursor, confirm, back, boot, notification). INTERACTION-AUDIO.md
and `coldboot` audio handle this on the native side.

## 9. What not to chase

- The launcher, shader-compile overlays and Call of Duty menus (R 0-20 s, 113-286 s) are RPCS3 and game content.
- DECISION (owner, 2026-11-22): the target look for the PSN column and its pages is the older M2 look (purple
  globe icon). Rows and pages seen only in R (What's New, PlayStation Plus, the "Our Pick" promo page, the store
  backdrop swap, the ticker) are therefore NOT gaps to build. M2-only evidence (Account Management, Sign-In page,
  Friends rows/Triangle panel, OSK) is the reference. Particles and the info-bar plate already in native are
  kept: they follow the 4.93 firmware reference, not M2; do not remove them on the strength of M2 alone.
- M2 and R are different firmware looks (PSN icon, particles, info-bar plate). FIDELITY-VERDICT.md already tracks the
  4.93 firmware as the reference.

## 10. Suggested next frame samples if more evidence is wanted

- R 66-70 s at 100 ms (first XMB frame, column-in animation).
- R 83-85 s at 33 ms (Options hint and PIC1 dwell; needs the focus-change frame, so start at about 82 s).
- M2 18-20 s at 100 ms (first column fade-in, Game column spawn).
- R 72-73 s and R 77-80 s at 100 ms (backdrop swap and store card animation).
