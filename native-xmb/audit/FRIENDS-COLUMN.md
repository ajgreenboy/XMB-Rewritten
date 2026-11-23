# Friends column: visual system and truthful gating (2026-11-22)

Target look: the older-firmware recording m2 (VIDEO-GAP-ANALYSIS-2026-11-22.md section 6 and the owner's decision in
section 9). Geometry is measured on its 1280x720 frames (+/-1 px) and scaled by 1.5. Code: `src/friends_ui.h` (pure,
tested by `tests/friends_ui_tests.cpp`), drawing in `view.cpp`, rows and options in `shell.cpp`.

This build has no PSN. A friend row exists only for a person a real source lists (the signed-in Discord friend list).
Discord's relationships list carries no presence and no status line, so today its rows draw no dot and no bubble; the
drawing is data-driven (`Entry::presenceKnown`, `Entry::statusText`) and is exercised by the test-only fixture below.

| Item | Measured (m2) | Native |
|---|---|---|
| Row order | Block List, Add a Friend, Players Met, Start New Chat, Message Box, then friends (m2 45 s); no Chat Room row | Same; Chat Room (Text Only) is no longer listed |
| Avatar tile | 54 px unfocused, 107 focused (m2 42 s), about 163 with the Triangle panel open (m2 34 s). The default round "ball" avatar is 0.68 of the tile (36/54, 73/107) | 78 / 160 / 244 px at 1080p; the default avatar glyph is drawn at 0.68 of the tile; a real picture fills the tile with a rounded edge, soft top highlight and light border |
| Status dot | 14 px, left of the name, blue online, grey-blue offline | 21 px at name x -22.5; drawn only when `presenceKnown` |
| Name | 22 px, left at avatar centre + 141 px | same |
| "Me" diamond | white diamond on the user's own row | not drawn: there is no own-account row (the Discord client never fetches `/users/@me`) |
| Speech bubble | grey rounded, 299x48 at name x, bottom 48 px above the name centre, 15 px white text, after about 1 s of rest | drawn only with real `statusText`, after 900 ms rest (INFERRED delay); none for Discord rows |
| Selection plate | r 73/82/91 s only (newer firmware): x 320-851, y 279-395 around the avatar; m2 shows none | geometry kept (`plateRect`) but `kRestingPlate=false`, per the owner's decision. The star at its right end is a PSN badge and is never drawn |
| Triangle panel | Status (arrow), Comment, Profile; darker band x 305-880, y 255-415 behind the row | Same three rows in the existing item band; band drawn. Profile opens `https://discord.com/users/<id>`; Status is enabled only with a real presence; Comment says it needs PSN |

Test-only fixture: `XMB_TEST_FRIENDS=<json>` lists `[{"id","name","presence":"online|offline","status","avatarFile"}]`.
It is read once at start-up, sends nothing anywhere and is unset in every real run.

Known differences: the whole page dims (0.545) under the open panel, where m2 keeps the focused avatar bright; panel rows sit
about 10 px higher and 7 px left of m2; the Comment text-entry page and OSK are unchanged (they pre-exist).
