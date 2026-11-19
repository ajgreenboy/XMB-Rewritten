# Legacy PlayStation Store — product-detail metadata + the download confirmation step (2026-11-19)

Completes the two surfaces `audit/LEGACY-STORE.md` left to "the separate worker": the
product-detail metadata and the source → confirmation → drive → Download Management hand-off.
No Steam changes; no cart or wallet (the feed has no prices, so every item is the manual's
free-item path).

## 1. Product-detail metadata (RAWG)

`HydraLibrary::GameResult` still carries no description, genre, release year, developer,
publisher or platform, so the detail page resolves those from RAWG on demand.

- **Resolution is title-driven and conservative.** `openLegacyDetail` triggers
  `Shell::requestLegacyDetails(title)`, which searches RAWG by the game's title and maps the
  result to an id with `resolveRawgGameId` (rawg_client.{h,cpp}): an **exact normalized name
  match only**. Two distinct ids sharing a name (e.g. a 2006 and a 2017 "Prey") or no match
  both yield id 0, so a wrong description is never attached — the page stays on its honest
  "Not available" lines. An edition suffix ("Prey Remastered" for "Prey") is a different game
  and is not taken.
- **Detail-only, never per grid item.** Only the product-detail page requests metadata; the
  catalogue grid and its left panel never do.
- **Async with a stale-response guard.** The lookup records `pages_.size()` when it starts
  (`legacyDetailDepth_`) plus the title; any `searchReady`/`detailsReady` that lands after
  navigation (the detail page popped, or another page pushed on top) is dropped. The client is
  a dedicated `RawgClient` (`rawgDetails_`), separate from the artwork client's.
- **No key → no network, no block.** `requestLegacyDetails` returns before any request when
  `RawgClient::hasApiKey()` is false, so the catalogue stays usable and the fields stay
  "Not available".
- **Plain text only.** The view renders description and the metadata strip through the text
  renderer (QPainter text, never Qt rich text), and `parseRawgGameDetails` already strips the
  HTML description to plain text, so a description cannot inject markup.

Rendered fields, from the parsed document only: **publisher** (in the title strip, next to the
existing Size label), **genre / release year / developer / platform** (one metadata strip built
by `rawgMetadataSummary`, actual data only), and the **description**. When a field is absent the
detail page keeps the previous "Publisher: Not available" / "Description not available for this
title." copy.

## 2. Download confirmation step (JSON game source)

The modal `showConfirm` in `completeGameDownload` is gone. The flow is now ordinary pages, so
Circle/Back pops each step and the compact source list stays beneath:

1. **Source list** (`openGameSources`) — unchanged.
2. **Confirmation page** (`beginGameConfirm`) — a full page titled with the game, one primary
   **Download** row whose detail shows the source (and mirror) and whose value is the download
   size. It is the free-item confirmation; there is no cart, wallet or price line.
3. **Drive picker** (`beginGameDownload`) — unchanged, still preselects the last-used drive
   (`downloads/lastGameDrive`).
4. **Enqueue** (`completeGameDownload`) — pops the drive picker and the confirmation page, then
   `enqueueReleaseGame` persists the drive on success and opens Download Management automatically.

`completeGameDownload` no longer builds a question/body or calls `showConfirm`; the confirm cue
and the download action live in the confirmation page's own row (`Action::GameDownloadConfirm`),
which `shell.cpp` dispatches to `confirmGameDownload()`.

Back still never cancels a running download: it only navigates (the Download Management job
lifecycle in `release_jobs.cpp` is untouched).

## Files

- `src/rawg_client.{h,cpp}` — `normalizeRawgTitle`, `resolveRawgGameId`, `rawgMetadataSummary`
  (pure, unit-tested, shared by the shell and the test runner).
- `src/shell.h` — `Action::GameDownloadConfirm`; `Page::legacyDetails`; `legacyDetails()`
  accessor; `rawgDetails_`/`legacyDetailDepth_`/`legacyDetailTitle_`; `requestLegacyDetails`,
  `beginGameConfirm`, `confirmGameDownload`.
- `src/legacy_store.cpp` — `requestLegacyDetails` and its hook in `openLegacyDetail`.
- `src/release_shell.cpp` — `beginGameConfirm`/`confirmGameDownload`; `pickGameSource` hands off
  to the confirmation page; `completeGameDownload` enqueues without a modal.
- `src/shell.cpp` — dispatch for `GameDownloadConfirm`.
- `src/view.cpp` — the product-detail panel renders publisher, the metadata strip and the
  description from `Shell::legacyDetails()`.
- `tests/rawg_tests.cpp` — `resolveRawgGameId` and `rawgMetadataSummary` checks.

## Verification

- `xmb-rawg-tests` (synthetic fixture, no network, no real copyrighted description, no
  credentials) covers the resolution rules and the metadata summary alongside the existing
  parser checks.
- Full `re-ps3-native` build; `xmb-release-tests`; `bun work/ci-guard.ts`;
  `work/release-check.ps1 -SkipBuild`.
- The RAWG network round-trip (search → `/api/games/{id}`) is not exercised here: it needs a
  RAWG key and the public API, and the no-key path is exactly what the tests pin. The layout
  change is a self-baseline the render gate's scripted run reaches through the existing
  `XMB_LEGACY_STORE_FIXTURE` hook.
