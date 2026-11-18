# Phase 3 — Seerr through to Jellyfin (2026-11-17)

The forward handoff's Phase 3 asks to keep a request visible after it leaves the SABnzbd queue, distinguish
downloading / waiting-for-import / ready-to-watch, resolve the matching Jellyfin library item through provider
identifiers, and offer **Watch** only when the item is actually playable. `seerr_shell.cpp` already polls Seerr
requests, checks the exact SABnzbd queue ids, and offers Pause/Resume/Cancel; what is missing is the last leg — the
Jellyfin match that lets a completed Seerr request turn into a Watch action.

## 1. The missing primitive: provider identifiers on Jellyfin items

A Seerr request carries a TMDB id (`media.tmdbId`), and Seerr's search/detail paths already key on it. Jellyfin items
carry the same external identifiers in `ProviderIds` (`Tmdb` / `Imdb` / `Tvdb`), but the client never read them, so
there was no way to recognise the matching item. `JellyfinItem` and `parseItem` now parse them:

- `JellyfinItem` gains `tmdbId` and `imdbId` (src/jellyfin_client.h).
- `parseItem` reads `ProviderIds.Tmdb` (kept only when it is a bounded numeric id, 1..32 digits) and
  `ProviderIds.Imdb` (bounded, 1..32 chars), ignoring any other provider key (src/jellyfin_client.cpp).
- `items()` and `search()` now request `ProviderIds` alongside `Overview` in `Fields`, so the server actually
  returns them (src/jellyfin_client.cpp).

This is the foundation the Watch resolution needs; the resolution itself (shell-side: match a completed Seerr
request's `tmdbId` against a Jellyfin search result and surface a Watch action) is the next increment.

## 2. Verification

```
cmake -S native-xmb -B out/phase-candidate ... -DXMB_JELLYFIN_PLAYBACK_BUILD_TESTS=ON
cmake --build out/phase-candidate --target xmb-jellyfin-playback-tests
out/phase-candidate/xmb-jellyfin-playback-tests.exe   -> 133 checks, 0 failures
```

Two new assertions cover the parse: the home fixture's `resumeItem` now carries
`ProviderIds{"Tmdb":"1399","Imdb":"tt0944947","Tvdb":"333"}`, and the test checks `item.tmdbId == "1399"` and
`item.imdbId == "tt0944947"` (the `Tvdb` key is deliberately ignored). The full app build is clean; `ci-guard` and
the release-check static gates pass unchanged (the `Fields` change is not pinned by any test).

## 3. Open

- The shell-side resolution: on a Seerr request whose SABnzbd transfer has completed (no live queue id, media status
  not yet `Available`), run a Jellyfin `search()` on the title, match by `ProviderIds.Tmdb == tmdbId` (falling back
  to a careful title match), and offer **Watch** when a playable Movie/Episode resolves. Keep seasons and 4K distinct
  (the `is4k` flag and the per-season `downloadStatus4k` already read in `seerr_shell.cpp`), and keep an honest
  "Waiting for import" intermediate when no Jellyfin item matches yet.
- A mock movie + TV season driven through transfer → import delay → Jellyfin availability, plus a completed-SAB
  transfer whose Jellyfin item has not arrived, a conflicting same-title item, and a different season.

See `audit/JELLYFIN-NATIVE.md` and `audit/SEERR.md` for the service contracts.
