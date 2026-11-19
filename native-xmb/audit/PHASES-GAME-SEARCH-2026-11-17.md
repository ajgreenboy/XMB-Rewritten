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

## 7. Multi-release fixture (verified)

A second scratch harness (`%TEMP%\xmb-p1-multi\`, not committed) imports a feed with two editions of one game, a second
Deluxe entry with no size and a dead URL, and a mirror-group release, then searches "overlap" under the imported scope.
Observed:

- `Imported 4 releases from P1 Multi Feed`; searching "overlap" yields two results — "Overlap Game — Deluxe Edition"
  (two options: the 2 GB direct and the unknown-size/dead-URL one, merged by name) and "Overlap Game — Standard
  Edition" (one option). Editions are therefore kept apart when the release title carries the edition, exactly the
  "distinguish similarly named games" case.
- The game page (depth 4) lists `P1 Multi Feed / 127.0.0.1 · Mirror 1` — mirror groups render with the `· Mirror N`
  suffix (`release_shell.cpp` `openGameSources`), and an option whose feed entry has no `fileSize` shows `Size unknown`
  in its value column (the `option.fileSize.isEmpty()` fallback).

The one acceptance element not separately driven is a live `all`/`online` scope against the Hydra library (needs the
public API), and the retained-artwork assertion with a cover-URL feed entry; both are covered by design rather than a
new capture.

## 8. Back/Esc cancellation on a focused release job (2026-11-18)

Circle/Esc (`Shell::back`) previously only popped the page on the Download Management list and a release job's detail
page; cancellation lived solely in the detail page's "Cancel Download" row and the Triangle option. An active background
transfer now gets a Back/Esc affordance on **both** surfaces — the focused row of the Download Management list and the
detail page:

- `releaseJobDetailId()` recognises a release-job detail page by its first row (`download:release:<id>:status`);
  `releaseJobBackTarget()` extends that to the Download Management list's focused row (clamped selection, decoded through
  the shared `releaseJobIdFromRow`, which rejects SteamCMD rows, Steam's own queue and malformed ids). The Download
  Management list and every other non-release page/row return empty.
- `cancelReleaseJob(id)` is the single source of the cancellation mutation, reused by the detail-page Cancel row
  (`activateReleaseJob`) and the Back handler, so no state change is duplicated. Queued/Paused jobs cancel immediately
  (discard partial, `Cancelled`); active jobs become `Cancelling` and raise the worker's cancel flag. An active job with no
  in-flight control is refused defensively (returns false, no state change).
- `releaseJobBack()` handles Circle/Esc when the target is active and still cancellable (Resolving / Downloading / Pausing /
  Extracting). It always leaves the page first (the console's Circle never strands a page), then raises
  `showConfirm("Cancel Download", ...)`. Yes cancels the focused job; No (the box default) dismisses it with the download
  still running in the background. Non-active rows (queued, paused, finished, failed, cancelled) and non-release rows return
  false and fall through to the ordinary pop, so Back still navigates away and other background jobs keep running.
- `pageKind()` reports `releaseJob` for the detail page, so a scripted capture can assert the state.

Because `showConfirm` carries only an `accepted` (Yes) callback — there is no distinct No path — the handler cannot run
"No navigates away while Yes cancels and closes" through two callbacks. The chosen deterministic behaviour: **Back leaves
the page on both answers**, then Yes cancels and No leaves it running. That satisfies "No leaves the page to XMB with the
download continuing" and cannot trap the user on the page behind a confirm.

### Pausing state fix

`canCancel` excluded `Pausing` (a job between "pause requested" and "paused" could not be cancelled, and its detail page
offered no Cancel row). It now includes `Pausing`. Cancelling a Pausing job must also clear the pause flag, or the worker's
finished handler would still land it on `Paused` — so `JobControl::requestCancel()` clears `pausing` and raises `cancel`.
The finished handler's settle decision (`paused ? Paused : cancelled ? Cancelled : Failed`) is hoisted into
`releaseJobSettledState(paused, cancelled)` and used in production, so the Pausing → cancel → Cancelled path is testable.

**Known limitation (pre-existing, documented rather than papered over):** if the pause's wind-down already flushed its
checkpoint before the cancel request landed, the partial archive remains in `Download Archives/<id>/` — the job is still
`Cancelled` and `archiveForResume` is cleared, but the file is orphaned. This is a cancel-during-pause race, not a
regression this affordance introduces.

### Verification

- `xmb-release-tests` gains checks over the state-predicate table, the production `releaseJobSettledState` mapping, the
  Pausing → `requestCancel()` → Cancelled flow (driven through `JobControl`), and `releaseJobIdFromRow` decoding (list row,
  detail status row, and the SteamCMD / Steam / empty rejections); all pass. The release backend's pre-existing
  temp-directory/network section fails in this sandbox (a `QTemporaryDir` environment issue, untouched by this change).
- Full `re-ps3-native` build (Qt 6.12.0, MSVC 19.44) exits 0; `release_jobs.cpp` and `shell.cpp` compile clean.
- Routing by code: Esc and pad Circle share `Shell::back`. `view.cpp` maps `Qt::Key_Escape`/`Qt::Key_Backspace` to
  `shell_.back()`, and the XInput `B` (Circle) edge to `shell_.back()`; `desktop_pad.cpp` maps Circle to Escape. The
  affordance therefore fires for both Esc and the pad's Circle.

### Scripted loopback Back/Esc E2E (2026-11-18)

A scratch harness (`%TEMP%\xmb-p-cancel\p-cancel.ts`, not committed; `verify-restart.ts` drives the restart pass) runs the
real loopback route against a build of revision `6a3ddd5`
(`out/release-tests-backesc-6a3ddd5/re-ps3-native.exe`). The feed is a `file://` import; only the archive streams from a
loopback server that throttles a 32 MB body at ~16 KB / 30 ms, so the job stays Downloading through the whole navigation to
Download Management. `XMB_GAME_LIBRARY_ROOT` points at the scratch `library` tree. Observed, all in scratch:

- Back/Esc on the focused active row → Yes → Cancelled. With Download Management open (depth 1, `pageKind=downloads`,
  focused row `download:release:f6ef9df0bb704ca9bff89ed2691f117a`, title "Cancel Target Deluxe", still Downloading), Esc
  pops the page first (capture `f-confirm` is back at the root, depth 0, before the confirm is answered), then `RIGHT` +
  `ENTER` answers Yes; the job settles `Cancelled` and the Download Management notification reappears (`g-afteryes`,
  `h-final`).
- `apps.json` is never written (`data/apps.json` absent) — no registration, since the download never completed.
- The partial archive is discarded, not orphaned: `library/XMB Games/Download Archives/f6ef9df0bb704ca9bff89ed2691f117a/`
  exists and is empty (0 entries) — the in-progress archive was cleaned up (contrast the cancel-during-pause race above,
  which can leave the file behind).
- Restart / queue restore (`verify-restart.ts`): reopening Download Management and the job's detail page reads the persisted
  `Cancelled` state and offers Retry to re-enqueue; `data/release-download-queue.bin` holds the serialized job.

Shot sidecars (`%TEMP%\xmb-p-cancel\shots\*.png.json`) carry the page kind, depth, and focused row for each step:
`a-enqueued` (source page, enqueue notification), `b-root`, `c-network`, `d-dlrow` (Download Management row selected),
`e-dlmgmt` (list open, active job focused), `f-confirm` (Esc popped to root, confirm up), `g-afteryes` (Yes answered),
`h-final` (settled). `verify-restart.ts` writes its captures to `shots2\` when run.

### Replayed omitted paths: No branch and non-active (queued) row (2026-11-18)

A second scratch harness (`%TEMP%\xmb-p-cancel2\p-cancel2.ts`, not committed) replays the two omitted paths against the same
revision `6a3ddd5` build. The loopback serves a 64 MB body at ~16 KB / 30 ms (~120 s drain) so any active job stays
Downloading through the whole navigation, and `XMB_GAME_LIBRARY_ROOT` points at a fresh scratch `library` tree. Two cases,
all in scratch:

- **Case 1 — No on the active confirm preserves the download.** One release is enqueued and reaches Downloading. Esc on the
  focused active row pops Download Management to the root (depth 0) and raises the confirm (`f-confirm`); the box default is
  No, so `ENTER` answers No (`g-afterno`). Reopening the job's detail page reads its status row as `Downloading`
  (`i-status-downloading`, `pageKind=releaseJob`) — the transfer kept running. The job is then cleanly cancelled (Esc → Yes)
  and settles `Cancelled` (`m-status-cancelled`).
- **Case 2 — Esc on a non-active (queued) row pops with no prompt while the active job continues.** Two releases are
  enqueued; one transfer runs at a time, so the Deluxe release is Downloading and the Standard release is Queued. The list's
  default focus (row 0) is the queued Standard row; Esc pops to the root with no cancel prompt (`d2-afteresc`, `modalWidget`
  empty, no confirm) — an ordinary Back. Focusing the Deluxe row (row 1) and opening its detail reads `Downloading`
  (`f2-a-status`): the active transfer was unaffected. Both jobs are then cancelled in turn (Esc → Yes on each) so nothing is
  left running (`j2-final`).
- In both cases `apps.json` is never written, and each cancelled job leaves `Download Archives/<id>/` as an empty directory
  (0 entries) — the in-progress archive is discarded, not orphaned, matching the existing E2E above.

Unrelated observation, not root-caused: quitting with a release download still active was observed to hang, so both scripts
cancel every job before the final `quit`. The hang is outside this affordance's scope and is recorded here only as observed
behaviour, not a diagnosed defect of the Back/Esc change.

Still not replayed: Back on a non-release row and Back on the remaining non-active states (paused / finished / failed /
cancelled). Those paths remain covered by the state-predicate table and the `releaseJobBackTarget()` / `cancelReleaseJob()`
unit tests above, not by a UI replay. The already-open confirm before answering (pre-confirm dismiss) is now replayed for
the pre-download confirm — see the subsection below.

### Pre-download confirm Esc path (replayed, 2026-11-18)

A third scratch harness (`%TEMP%\xmb-p-cancel-pre\p-cancel-pre.ts`, not committed) replays the remaining confirm case —
Back/Esc on the already-open confirm before answering — against the pre-download confirm. It drives the §5 route to the
drive picker, selects the scratch drive (raising `completeGameDownload`'s `showConfirm` "Download and install <game>?",
shell.cpp:2381), then sends Esc/Circle in place of the `RIGHT`+`ENTER` commit. Source is HEAD `2199652`; the build under
test is `out/native-xmb/re-ps3-native.exe` (relinked 2026-11-18 18:08). The run is evidenced by the harness log and the
shot sidecars, not by reading the PNG pixels:

- Dialog transcript: `dialog: box show choices=yes choice=no` then `dialog: box cancel choice=no`, and no `box enter`
  commit — the box's Yes callback (`enqueueReleaseGame`) never ran.
- No enqueue and no artifacts: `data/release-download-queue.bin` absent, `data/apps.json` absent, no
  `XMB Games/Download Archives/` under the scratch library (the directory is never created), and the loopback server
  counted 0 archive requests — no transfer started.
- Clean exit 0 (~29 s), no hung process.

Sidecars pin the page state at each step: `s0-focus` (root, depth 0, `game:download-hub`, row 17 = Download Games)
confirms the entry point, and `p1-confirm` / `p2-after-esc` / `p3-after-esc2` (page, depth 5, focused row the scratch
drive entry) confirm the drive picker is the active page both when the box is raised and after Esc dismisses it back to
the same page — the box itself is evidenced by the dialog log lines, not the sidecar.

Build caveat: `out/native-xmb/build-info.json` is stale — it still records `sourceRevision 67f69f7` (built 2026-11-12),
older than both the relink and HEAD `2199652` — so the exe's embedded build-info does not identify the source under test.

## 9. Defensive worker join on shutdown (2026-11-18)

The quit-hang observation in §8 did not reproduce consistently — the pre-fix binary also exited cleanly in repeated runs, so
no root cause was confirmed. `stopReleaseJobs()` now defensively joins the in-flight worker (`QFuture::waitForFinished()`)
so `app.quit()` exits deterministically rather than racing the thread pool's static-destruction `waitForDone()`. On the fixed
build, three isolated loopback active-transfer runs quit → exit in ~0.6 s with the queue and archive resume state retained.
No live HTPC run was exercised, so this remains a defensive fix, not a proven root cause.
