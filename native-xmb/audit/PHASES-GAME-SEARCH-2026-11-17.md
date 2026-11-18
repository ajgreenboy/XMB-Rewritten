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

## 4. Open

Remaining Phase 1 acceptance items still to reach: the full end-to-end "overlapping game, distinct editions, multiple
mirrors, an unknown size, and a failing source" fixture pass across all three scopes, back navigation with the scope
and query retained on return, and a real download to a scratch drive with one registered Game entry, retained artwork,
and a restart that shows it once. Those need local feed fixtures and a scratch install destination, which are the next
increment (see `audit/PHASES-MANIFEST-DOWNLOAD-2026-11-11.md` for the registration/integrity reference).
