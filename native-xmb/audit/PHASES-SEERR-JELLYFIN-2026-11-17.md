# Phase 3 — Seerr through to Jellyfin (2026-11-17)

The forward handoff's Phase 3 asks to keep a request visible after it leaves the SABnzbd queue, distinguish
downloading / waiting-for-import / ready-to-watch, resolve the matching Jellyfin library item through provider
identifiers, and offer **Watch** only when the item is actually playable. `seerr_shell.cpp` already polls Seerr
requests, checks the exact SABnzbd queue ids, and offers Pause/Resume/Cancel; what is missing is the last leg — the
Jellyfin match that lets a completed Seerr request turn into a Watch action.

This document records the Stage 0 safety fix on top of that foundation: the match decision is now a pure, tested
policy (`src/seerr_jellyfin_match.h`) that enforces movie-only matching, exact TMDB equality, the 4K width rule and
a deliberate no-TV-Watch state, replacing the earlier first-wins `(tmdbMatch || titleMatch)` over
Movie/Episode/Video that dropped `is4k` and could match a same-title Movie to a TV request.

## 1. The foundation: provider identifiers and source width on Jellyfin items

A Seerr request carries a TMDB id (`media.tmdbId`), and Seerr's search/detail paths already key on it. Jellyfin items
carry the same external identifiers in `ProviderIds` (`Tmdb` / `Imdb` / `Tvdb`). `JellyfinItem` and `parseItem` read
them, and now also the source width the 4K rule needs:

- `JellyfinItem` gains `tmdbId` and `imdbId` (src/jellyfin_client.h).
- `parseItem` reads `ProviderIds.Tmdb` (kept only when it is a bounded numeric id, 1..32 digits) and
  `ProviderIds.Imdb` (bounded, 1..32 chars), ignoring any other provider key (src/jellyfin_client.cpp).
- `items()` and `search()` request `ProviderIds` alongside `Overview` in `Fields`; `search()` also requests
  `MediaSources` so the resolver has a width to read (src/jellyfin_client.cpp).
- `JellyfinItem` gains `width` (src/jellyfin_client.h): `parseItem` takes the widest `MediaSources[].Width`, and
  falls back to a top-level `Width` only when no source width was seen. `0` means the server reported none — it is
  "unknown", never a confirmation and never "SD".

## 2. Watch resolution — the Stage 0 safety policy

The match decision lives in `resolveSeerrWatch()` (`src/seerr_jellyfin_match.h`), a pure function over the Seerr
request and one Jellyfin search page, with no network and no session state. The rules it enforces:

- **Movie-only.** A movie request matches only a Jellyfin `Type == "Movie"` item. Episode, Video and Series are
  never a movie match.
- **Exact TMDB equality.** With a Seerr TMDB id, a candidate matches only on exact id equality; a same-title
  candidate with a different id is never accepted as a fallback.
- **Title fallback, narrowly.** The title fallback (case-insensitive, trimmed) applies only when the Seerr TMDB id
  itself is missing. With a supplied Seerr id, a candidate with a missing id cannot be confirmed and is refused just
  like a mismatched id — title never overrides a known id or fills in an absent one.
- **4K width rule.** A 4K request watches only a candidate whose `MediaSources` width confirms `>= 3840`; an unknown
  width is unverified and never presumed 4K, and a known `2160p` (non-UHD) source is not 4K. A regular request
  prefers a known `< 3840` copy; otherwise it watches its sole matching copy (a regular request needs no quality
  proof, so a sole 4K or sole unknown-width copy is permitted), and refuses only when several copies leave no safe
  pick — the resolver never invents a variant.
- **No TV Watch.** A TV request never matches Movie/Video/Series as directly playable; it is left pending until
  Stage 1 season/episode matching exists.

`seerr_shell.cpp` now threads the request's `mediaType` and `is4k` (and the movie's TMDB id) through
`resolveSeerrJellyfin`, stores them per row, and applies `resolveSeerrWatch()` to the search result. A TV row is
never queued for resolution and never gains a Watch option — it keeps its Seerr media status, which is the honest
"when import cannot be confirmed" state. A failed search (a 401 or 5xx) releases its row: the in-flight map, the
pending set and the title-key are cleared, keyed on the search's own request id, so the next poll retries. An
unrelated Jellyfin failure carries a request id that is not in that map and is left untouched.

## 3. Verification

The policy is tested headlessly, with no real server, account or credential:

```
cmake -S native-xmb -B out/seerr-watch -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH=C:\Qt\6.12.0\msvc2022_64 -DXMB_SEERR_WATCH_BUILD_TESTS=ON \
      -DXMB_JELLYFIN_PLAYBACK_BUILD_TESTS=ON
cmake --build out/seerr-watch --target xmb-seerr-watch-tests xmb-jellyfin-playback-tests
out/seerr-watch/xmb-seerr-watch-tests.exe          -> 18 checks, 0 failures
out/seerr-watch/xmb-jellyfin-playback-tests.exe    -> 140 checks, 0 failures
```

`xmb-seerr-watch-tests` (`tests/seerr_jellyfin_match_tests.cpp`) pins the policy: SD vs 4K for a 4K request, unknown
width, a same-title mismatched TMDB, title fallback only when the Seerr id is missing (a missing candidate id is
refused), an exact-id match across a title difference, wrong-type TV, the single-copy movie, the regular preference
for `< 3840`, the sole-copy allowance (4K or unknown width), and the ambiguous/no-variant refusals.

`xmb-jellyfin-playback-tests` gained seven assertions (133 → 140): `MediaSources[].Width` is parsed, a top-level
`Width` is honoured when the server sends no `MediaSources`, the `search()` route requests `MediaSources` in `Fields`
and parses a movie's width, and a failing search (401 then 5xx) answers under the search's own request id through
`failed` with no page — the signal the resolver clears its in-flight row on.

`seerr_shell.cpp` compiles clean under `/W4` (the `value` shadow warning is renamed away). `ci-guard` wiring
(CMakeLists.txt ↔ tests.yml ↔ the new runner) is updated.

The policy is also driven through the loopback three-service fixture — a mock Seerr/Jellyfin on `127.0.0.1`
(`:8190`/`:8191`), **no live service** — re-run against the integrated build `617b453` (clean, verified on disk).
Evidence: `out/seerr-harness/EVIDENCE-INTEGRATED.md`; the per-case `dump.options.json` records whether `Watch`
(condition `seerr-jellyfin`) was offered:

| Case | Seerr is4k | Jellyfin search result | Options | Verdict |
|---|---|---|---|---|
| `int-match` | false | "Fight Club" Tmdb 550 (1920) | `Status`, `Watch` | exactly one Watch (baseline) |
| `int-sametitle` | false | "Fight Club" Tmdb 999999 (1920) | `Status` | no Watch (same title, different TMDB) |
| `int-4k-sd-first` | true | SD 1920 first, then 4K 3840 | `Status`, `Watch` | Watch `movie-4k` (PlaybackInfo log) |
| `int-4k-sd-only` | true | SD 1920 (sole copy) | `Status` | no Watch |
| `int-4k-width-unknown` | true | no width reported | `Status` | no Watch |
| `int-401-retry` | false | first search 401, later match | `Status`, `Watch` | retry resolves Watch |

The whole run logged exactly one PlaybackInfo line — `POST /Items/movie-4k/PlaybackInfo` — so the 4K request with
SD listed first opened `movie-4k`, not `movie-sd` (`int-4k-sd-first`'s `grab.png.json` records the stream opened).
The 401 case releases its row on the failed search and the next poll re-resolves it to a Watch. All of this is
loopback mock evidence; no live Seerr or Jellyfin service was contacted.

## 4. Open

- The loopback Seerr → Jellyfin resolution is now recorded (table above). A fuller fixture that also drives a mock
  TV season through transfer → import delay → Jellyfin availability remains open, as does any live-service evidence
  (none claimed here).
- TV season/episode matching is deliberately unbuilt: a TV request is `Pending`/unverified until Stage 1 implements
  per-season episode matching. This is not claimed as complete.
- The 4K rule reads `MediaSources[].Width`; a server that does not return `MediaSources` for a movie yields an
  unknown width and is refused a Watch for 4K (a regular request still watches its sole copy), rather than presumed
  either way.

See `audit/JELLYFIN-NATIVE.md` and `audit/SEERR.md` for the service contracts.
