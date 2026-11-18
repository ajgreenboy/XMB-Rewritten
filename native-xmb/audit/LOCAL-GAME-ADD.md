# Add a Game — controller-driven local-game add

## What this is

The Game column's **Manage** hub lists **Refresh Game Library**, **Import Games and Apps** and now
**Add a Game**. The first two are the desktop path (Import opens the native `AppEditor` Qt dialog, which a
pad cannot drive). **Add a Game** is the couch path: an ordinary XMB page driven with the pad, so a game can
be added without a mouse or keyboard.

A planning agent earlier equated this with the Download Games hub's **Import Source** row
(`release_shell.cpp`, the hydra/Seerr *release index* URL). They are unrelated: Import Source imports a
download *source*, while Add a Game registers one already-installed local executable.

## The flow (`add_game_shell.cpp`)

| Row | Cross does |
|---|---|
| Executable | opens the in-XMB folder browser in `.exe` mode (`folder_browse_shell.cpp`, `filePatterns` "game") |
| Name | the XMB keyboard (the one field-edit signal the shell already routes, key `addgame:title`) |
| Artwork Path | the XMB keyboard, to type or paste an existing picture path (`addgame:artwork`) |
| Choose Artwork | the in-XMB folder browser in image mode (`filePatterns` "artwork") |
| Add Game | writes the registration to `<data>/apps.json` via `ContentRegistry::upsert`, then refreshes the column |
| Cancel | leaves without writing anything (Circle does the same) |

The draft lives in memory (`Shell::addGameDraft_`) and nothing is written until Add Game. A cancelled or
aborted browse leaves the draft exactly as it was. Saving produces the same `ContentApp`
(kind `game`, category `game`) the column already lists and launches through `Action::LaunchApp` /
`Shell::startTracked`, so the saved executable persists in `apps.json` and launches on the existing
local-game API — no new launch path was added.

## Acceptance tests

Run from `C:\dev\xmb` against a build of this branch (`re-ps3-native.exe`), with `XMB_FOLDER_START` pointing
at a scratch folder holding a stub `Game.exe` and a `cover.png`:

1. **Browsing drives EXE selection.** Navigate Game → Manage → Add a Game → Executable; the folder browser
   opens (pad up/down/Cross). Cross on `Game.exe` returns to the page with the path shown and the Name
   defaulted to `Game`.
2. **Rename.** Cross on Name, type a new title on the XMB keyboard, accept; the Name row shows it.
3. **Artwork — browse or path.** Cross on Choose Artwork and pick `cover.png`, *or* Cross on Artwork Path and
   type/paste the picture's full path. Either populates the registration's artwork.
4. **Cancel returns safely.** From the page press Circle (or Cancel), and from inside either browser press
   Circle back to This PC then Circle again: the page closes, no `apps.json` change, no crash.
5. **Persist and launch.** Cross on Add Game. The Game column shows the new title; Cross on it launches
   `Game.exe` (the tracked `startTracked` path), and it appears again after a restart (it is in `apps.json`).

Targeted unit test: `xmb-folder-browser-tests` now also asserts the `.exe` filter is case-insensitive and
lists only executables in natural order (`tests/folder_browser_tests.cpp`).

## E2E run (controller-scripted, 2026-11-18)

The flow was driven end-to-end by a scripted controller (`--script`, isolated `--data-dir`) against a build at
`e82f6a4`. Evidence in `C:\dev\out\wt-addgame-e2e\run\` (`run3.log`, `entries-after-save.json`,
`run4-circle.log`, `run5-cancelrow.log`).

- The Executable browser listed only `TestGame.exe`; the scratch folder also held `cover.png`, which the
  `.exe` filter excluded.
- The OSK rename and Choose Artwork (picking `cover.png`) both populated the draft.
- Add Game wrote `apps.json` with a custom id (`828aabca-56b7-4afc-8b47-bd667e4d7858`), the artwork path, and
  the renamed title; the Game column count went 19 → 20 with the new `custom:` row plus artwork.
- Circle and the Cancel row both left `apps.json` unchanged (no save).

This was a synthetic pad (scripted inputs), not a physical controller, and it did not launch the saved game.

## Remaining gaps (honest)

- **Editing an existing game.** The flow only *adds*; there is no controller-driven edit/remove of a game once
  saved (the desktop `AppEditor` still handles that).
- **No launch argument or working-directory entry** on the couch page; the desktop editor has them.
- **Artwork preview** is not shown on the Add a Game page itself (only on the column row after saving).
- **`.exe` filter is shell-side only.** `folder_browser.cpp` is unchanged and generic; the `.exe`/image
  patterns live in `folder_browse_shell.cpp` and are covered by the folder-browser tests only as a
  case-insensitive `*.exe` match, not through the shell wiring (which is exercised by the manual run above).
- **No render-gate frame** for the new page (none was requested; the page is an ordinary list surface).
