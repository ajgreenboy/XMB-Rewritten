# Phase 1 — Game search, mirrors, and install (2026-11-17)

The forward handoff (`HANDOFF-PHASES.md`, "Phase 1 — Game search, mirrors, and install") names two paths into a game
download: the Download Games hub (Search / Browse / imported sources) and the search → game page → drive → confirm →
enqueue chain in `src/release_shell.cpp` / `src/release_jobs.cpp`. This note records the first increment of that phase,
scoped to the pieces that could be verified against the existing fixtures and the local gate without a live network or
a scratch physical drive.

## 1. What was found at the start

`runGameSearch()` (release_shell.cpp) built a result page whose rows carried `entry.gameTile=true` and
`entry.artworkUrl=game.coverUrl`, but **never resolved that URL to an image** — the worker returned only the
`GameResult` list, so every search result row rendered its fallback glyph instead of box art. `openBrowseGames()` was
the only path that fetched covers (into `BrowseOutcome::covers`, keyed by list position). The game page
(`openGameSources()`) carried the title (`page.title = game.name`) and a source list but no artwork at all.

## 2. Changes

All in `src/release_shell.cpp` plus one method declaration in `src/shell.h`:

- Factored the browse worker's inline cover-fetch loop into a file-local `fetchCovers(games, cacheRoot)`, and added
  `coverCachePath(url, cacheRoot)` (the stable `<cache>/hot-<sha256[0..16]>.png` name) and
  `cachedCoverPath(url, cacheRoot)` (the local path `fetchCover` wrote, or empty when the cover could not be cached).
- `openBrowseGames()` now calls `fetchCovers` — behaviour unchanged, just shared.
- `runGameSearch()` passes the cache root into its worker, calls `fetchCovers` on the merged imported+online result
  set, and assigns `entry.artwork = result.covers.value(i)` next to the existing `artworkUrl`. Cached imported covers
  remain usable when Online Browse fails, and the failing source is already surfaced as the `game:error` row plus the
  `result.error` detail — unchanged, now retained together with the art.
- `openGameSources()` calls the new `Shell::requestGamePageArt()` after pushing the page. That fetches the chosen
  game's cover (async, off the UI thread, through the same cache) and sets `backgroundPath` on every
  `Action::GameSourcePick` row, so the view's existing `focusedBackgroundPath()` path draws the cover behind the
  source list. A depth guard drops the result if the player has since navigated away.

Nothing in the enqueue/extract/register chain (`release_jobs.cpp`) or the view renderer was touched.

## 3. Verification

Build (Qt 6.12.0 MSVC kit; the 6.11.1 kit fails `QJsonObject::cend()` in `preferences.cpp`):

```
cmake -S native-xmb -B out/phase-candidate -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH=C:/Qt/6.12.0/msvc2022_64 -DCMAKE_MAKE_PROGRAM=<vs>/Ninja/ninja.exe
cmake --build out/phase-candidate --parallel          # exit 0, release_shell.cpp clean
windeployqt out/phase-candidate/re-ps3-native.exe
```

`bun work/ci-guard.ts` → `ok (24 options, 27 targets, 23 runners)`.

`work/release-check.ps1 -SkipBuild` → `RESULT PASS (5 passed, 0 failed, 2 skipped)` (keys, data, dump, docs, stale).

Layer gate `bun work/ab-layers.ts --all --exe out/phase-candidate/re-ps3-native.exe`:

- stored baseline pair **6/6 PASS**; canonical capture **6/6 PASS**;
- named states **27/28 PASS**; the one failure is `ingame-xmb`, whose full-frame region scored 52.43 dB / 0.9994
  FAIL while its content regions (`game frame`, `guide`) were `inf`/1.0000. That state captures a stand-in game
  window through the Windows DWM; the recipe documents the top title-bar rows drifting "up to 2 levels over time on
  this machine (the stored ingame-xmb frame shows the same 62 dB drift over its top 75 rows), which is noise, not a
  drawing change". None of the surfaces this increment touches are in any gated state, and the diff is confined to
  release search/browse, so the failure is the known machine-pinned compositor drift, not this change.

## 4. Scratch install destination (`XMB_GAME_LIBRARY_ROOT`)

The drive picker builds its list from `QDir::drives()` and always installs to `<drive>:\XMB Games`, so a replay on a
real machine would write to a real library. The handoff requires "a separate app data directory and scratch install
destination for each replay", and there was no seam for one. `beginGameDownload()` now resolves its list through a
file-local `gameLibraryRoots()`: when `XMB_GAME_LIBRARY_ROOT` names an existing absolute directory it replaces the
drive list with that single root (whose child `XMB Games` receives the install); otherwise the real drives are listed,
unchanged. This follows the `XMB_STEAM_ROOT` / `XMB_INSTALL_REFRESH_MS` convention of env-gated local-verification
seams. The existing `libraryVolume()` validation in `release_jobs.cpp` accepts a scratch root under any real volume
(e.g. `%TEMP%\...`), so enqueueing to it exercises the same code path as a real drive.

## 5. Scripted route (determined empirically)

`XMB_DUMP_ENTRIES` dumps the catalogue, and it pins the Game column layout: boot selects the Game column (index 6) at
row 0 (a Steam title), `Download Games` is row 17, `Manage` row 18. The route to the drive picker is therefore:

```
17 x DOWN -> Cross on Download Games (hub) -> Cross on Search Games (scope picker) ->
DOWN -> Cross on Imported feeds -> type <query> -> OSK accept -> Cross on the result ->
Cross on the source -> drive picker -> Cross -> RIGHT (No -> Yes) -> Cross (commit)
```

The search term is confirmed on the on-screen keyboard with `OSKENTER` (Ctrl+Return), a new script token added in
`main.cpp` because the script had no way to accept the OSK — a plain Return presses the focused key (the firmware
keyboard's Enter action is what commits, and Ctrl+Return maps to `oskClose(true)` in `view.cpp:5668`). It sits next to
the existing `PASTE`/`SHIFTINSERT` tokens.

## 6. End-to-end download -> register -> restart (verified)

A scratch Bun harness (`%TEMP%\xmb-p1-e2e\p1-e2e.ts`, not committed) stands up a loopback HTTP server serving a
28-byte ZIP whose single entry `OverlapGame.exe` is the stub `findGameExecutable` needs, imports a `file://` feed whose
direct URL points back at the loopback, and drives the route above with `XMB_GAME_LIBRARY_ROOT` pointing at a scratch
folder. Results, all in scratch, no real drive touched:

- Import: `Imported 1 releases from P1 E2E Feed`.
- Download → extract → register: `apps.json` gains
  `id=release-game-<uuid> title="Overlap Game Deluxe" kind=game exe=<scratch>/XMB Games/game-<uuid>/OverlapGame.exe`;
  the ZIP lands in `Download Archives/<uuid>/` and the install receipt `.xmb-release-install.json` beside the exe.
- Restart: a second boot dumps the catalogue; the Game column holds exactly one
  `custom:release-game-<uuid>` row titled "Overlap Game Deluxe" (row id prefixed `custom:`, shell.cpp:1441).

Extraction-failure recovery (`%TEMP%\xmb-p1-e2e\p1-fail.ts`, same harness against a non-archive body): the download
completes, `extractGameArchive` rejects the non-ZIP signature, `apps.json` is never written, and the downloaded file is
retained as `Download Archives/<uuid>/Broken Archive.zip`. The job's `Failed` row keeps `result.archive`, so the Phase
2 retry reuses the validated archive (the recovery path the acceptance asks to "record").

## 7. Open

What remains to reach the full Phase 1 acceptance: the multi-release fixture across all three scopes (an overlapping
game, distinct editions, multiple mirrors, an unknown size, and a failing source in one feed), the explicit
scope-and-query-retained-on-return check (the result page already persists in the stack with its `Search: <query>`
title), and the retained-artwork assertion (increment 1 fetches the cover; a feed entry carrying a cover URL would
exercise it — the e2e fixture omitted one). See `audit/PHASES-MANIFEST-DOWNLOAD-2026-11-11.md` for the
registration/integrity reference.
