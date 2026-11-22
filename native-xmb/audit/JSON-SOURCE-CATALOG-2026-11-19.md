# JSON game-source catalogue semantics (2026-11-19)

Scope: `native-xmb/src/hydra_library.{h,cpp}` and the release source layers
(`release_index.{h,cpp}`, `release_source.{h,cpp}`). No changes to `release_shell.cpp`,
`view.cpp`, `shell.cpp`, or `release_jobs.cpp`.

## 1. Audit — remote/local JSON sources and torrent vs direct handling

Two JSON catalogues feed game downloads:

- **Remote (Hydra)**: `api.hydralibrary.com/sources` lists sources; `api.hydralibrary.com/games`
  searches across them; `api.hydralauncher.gg/catalogue/hot` supplies the popularity ranking.
- **Local (imported)**: `ReleaseSources` persists a normalized index to `<data>/sources/<slug>.json`
  (via `release_index::parseReleaseIndex`, FitGirl shape + heuristic fallback). `listSources()`
  reads every `*.json` back with no filtering.

Link kinds are classified by `classifyUri` → `magnet` / `torrent` / `direct` / `unknown`, and the
download orchestrator `ReleaseSources::downloadResolved` handles **only** `direct` and `magnet`:

- `direct` → debrid `unrestrict` (or a plain HTTP fetch when no token is configured).
- `magnet` → debrid `addMagnet` → poll → `selectFiles("all")` → first link. Without a token this
  fails with `no Real-Debrid token configured`.
- a standalone `.torrent` URL (not a magnet) is classified `torrent` but **never submitted**; such a
  release falls through to `No direct download or supported magnet for this release`.

The pre-change bug: `HydraLibrary::searchGames`/`popularGames` dropped every game whose source failed
`isPreinstalledFor` (torrent-only sources, or sources not marked "pre-installed"/"portable"). That was
a **silent filter** — a torrent source's games were invisible even when the user had a debrid token and
a magnet was the only thing needed. The `/games` payload does not expose per-game link kinds, so the
catalogue could not, and still cannot, distinguish a usable magnet from an unusable `.torrent` at list
time.

## 2. Change

- `searchGames`/`popularGames` no longer filter by source; **every** source is included.
- The merge is now a deterministic, unit-tested function, `buildGameCatalog(records, sources)`:
  - one `GameResult` per distinct normalized name (Unicode NFKC + whitespace collapse + case-fold,
    the same key the shell's `gameSearchKey()` uses) — editions that differ in name stay separate;
  - one `GameSourceOption` per source (identified by its release-index URL, falling back to the source
    name), so a duplicated record from one source cannot add a second choice;
  - each option carries its `source` display name and `fileSize` (empty = "unknown", passed through).
  - Games are ordered by their most-installed source, descending (unchanged intent).
- `listSources` now uses the testable `parseSourcesPage` + `hasMoreSources` pair; the page loop
  (stop at `total`, on an empty page, on `limit<=0`, capped at 100 pages) is unchanged in effect and
  is exercised by fixtures.
- `popularGames` drops its now-redundant `listSources` pre-flight (the hot-catalogue fetch and each
  title's search already fail loudly on a dead network).

## 3. "Do not silently filter" vs "avoid offering unusable choices"

The honest resolution, given the API: include everything at list time, and let the download layer keep
reporting accurately at pick time. A torrent-only source's game is listed; choosing it either resolves
through debrid (token configured) or fails with `no Real-Debrid token configured` / `No direct download
or supported magnet for this release` — a real, actionable error, never a silently missing row.

## 4. Blocker (reported, not papered over)

Per-game link kind is **not exposed** by `api.hydralibrary.com/games`, so "avoid offering unusable
choices" cannot be honoured at catalogue time beyond including everything and relying on the download
layer's honest errors. A source whose releases are `.torrent`-file-only (no magnet, no direct) will
still be listed and will fail only when picked. Removing such sources at list time would require
fetching and normalizing every source's index per search, which is out of scope for this change and
would re-introduce a silent filter. This is documented as a known limitation, not a satisfied
requirement.

## 5. Verification

- `xmb-release-tests` (local fixtures only, no network, no real game media, no credentials) gains
  checks for: merged duplicates (same name across sources → one game, two options; same-source
  duplicate → one option), source name + size carried on each option, edition separation by name,
  empty-name drop, empty/unknown size passthrough, `parseSourcesPage`/`hasMoreSources` pagination,
  and malformed-source handling in `parseReleaseIndex`. Observed: **183 checks, 0 failures**.
- `bun work/ci-guard.ts` → ok (26 options, 29 targets, 25 runners).
- Full `re-ps3-native` build exits 0 (header change is additive; `shell.h` re-includes it).

## 6. Browse Sources reliability (2026-11-21)

An audit of the 61-feed Hydra directory found 34 feeds that open, 15 that refuse the app (HTTP 403,
14 of them on one host), 11 whose body is 16.8-53 MB (over the old 16 MiB cap) and one transient
timeout. Every feed looked the same in the list and every failure said "retry".

What changed (each its own commit):

- **Cap and lazy lookup.** The selected feed gets its own 64 MiB body cap (the import path already
  allowed 64 MiB). The directory, search and ranked fetches keep 16 MiB, and the declared-length and
  streaming/decompressed limits stay in force on every path. Opening a feed fetches and parses the
  index only; the per-game `/games` search that used to run for every row now runs once, for the
  game picked at Download (`HydraLibrary::resolveGameChoices`). Shared retry budget of the directory
  action (v0.3.20) is unchanged.
- **Honest reasons.** Failures are classified (`HydraFailureKind`): 401/403 "This source blocks the
  app (HTTP 403)" with no Retry row; 429 "The source is rate limiting requests; try again later"
  (manual Retry kept, never automatic); over the cap "Source too large for this device" with no
  Retry row; timeouts, 5xx and anything else keep Retry. The source directory page uses the same
  rules with its own wording. A refusal is requested exactly once.
- **Source-health note.** `source_health.json` in the app data folder keeps, per feed, the last
  outcome and time, and per host a count of refusing feeds. Feeds and hosts are keyed by SHA-1, so
  the file holds no URLs or host names. The list sorts working feeds first, then unprobed, failing,
  likely blocked (a never-opened feed on a host that refused two or more feeds within a day), too
  large and blocked; the last three are dimmed with "May block the app" / "Too large for this
  device" / "Blocks the app" and stay openable. Opening a feed that refused or was too large less
  than a day ago shows the stored reason without another request; after a day it is asked once more,
  so a source that recovers is marked working again.

Verified (loopback fixtures in `xmb-release-tests`, no real host used): a synthetic 100,000-entry
feed of about 50 MB opens with one feed GET and no directory or search request, in roughly 5-8 s on
an idle machine (longer when the machine is busy) with a peak working set of about 370 MiB, test
fixture included; the over-16 MiB feed is accepted and over-64 MiB rejected; 403 once with no
retry and no "retry" in its text, 401, 429, over-cap and 5xx classification; health persistence,
ordering, per-host hint, expiry, recovery, damaged-file handling and the once-a-day re-probe through
the real open path. 347 checks, 0 failures.

Not verified: the real 61 feeds were not fetched (no real host in any test), so the 15/11/34 split
is the audit's, not re-measured here; the on-screen rows (wording, dimming, absence of the Retry row)
were built and compiled but not driven in the running app; the per-game cover and Download flows for
a 100k-game feed were not exercised in the shell.
