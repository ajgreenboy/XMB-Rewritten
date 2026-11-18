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

This is the foundation the Watch resolution needs.

## 2. Watch resolution

The second increment closes the request → Watch loop in `seerr_shell.cpp`:

- A new `OptionCommand::SeerrDownloadWatch` and three Shell methods (`resolveSeerrJellyfin`,
  `applySeerrJellyfinMatch`, `watchSeerrJellyfin`).
- When a Seerr request's transfer has left the SABnzbd queue (`seerrSabIds` empty) but its media status is below
  `Available` (5), and Jellyfin is signed in, the row is queued for a resolution pass: one `jellyfin_.search(title)`
  whose `pageReady` answer is taken by a dedicated connection keyed on a request id the player's own search never
  uses, so the two routes cannot cross. The answer is matched against every still-pending row.
- A match is accepted on `ProviderIds.Tmdb == tmdbId` (the Seerr request's TMDB id) or, fallback, an exact
  case-insensitive title match, and only for playable `Movie`/`Episode`/`Video` items. On a match the row reads
  "Ready to watch · Seerr · …" and its Triangle menu gains a single **Watch** row (no SABnzbd controls, since the
  transfer is gone).
- **Watch** builds a one-item Jellyfin `Entry` and plays it through the existing `playJellyfinQueue` path, so the
  intended item opens in the same player as any Jellyfin row, with resume/seek/session reporting intact.

An honest intermediate remains: while the transfer is done but no Jellyfin item matches (still importing), the row
keeps its Seerr media status and offers no Watch — the state the acceptance calls "when import cannot be confirmed".

## 3. Verification

Provider-identifier parse (committed with the foundation):

```
cmake -S native-xmb -B out/phase-candidate ... -DXMB_JELLYFIN_PLAYBACK_BUILD_TESTS=ON
cmake --build out/phase-candidate --target xmb-jellyfin-playback-tests
out/phase-candidate/xmb-jellyfin-playback-tests.exe   -> 133 checks, 0 failures
```

Two assertions cover the parse: the home fixture's `resumeItem` carries
`ProviderIds{"Tmdb":"1399","Imdb":"tt0944947","Tvdb":"333"}`, and the test checks `item.tmdbId == "1399"` and
`item.imdbId == "tt0944947"` (`Tvdb` ignored). The full app build is clean; `ci-guard` and the release-check static
gates pass.

The Watch resolution compiles clean (no new warning beyond the pre-existing `value` shadow) but is not yet driven
end-to-end: the loopback Seerr + SABnzbd + Jellyfin fixture is the next piece of evidence.

## 4. Open

- A mock movie + TV season driven through transfer → import delay → Jellyfin availability, plus a completed-SAB
  transfer whose Jellyfin item has not arrived, a conflicting same-title item, and a different season. This needs the
  loopback Seerr + SABnzbd + Jellyfin fixture (the Seerr/SAB replay the handoff describes as local evidence).
- Keeping TV seasons and 4K distinct at the Watch step: the resolution currently matches the request (movie) or the
  show (title/tmdb), not a specific season or the 4K variant; the `is4k` flag and per-season `downloadStatus4k` are
  already read in `seerr_shell.cpp` and are the natural inputs for that refinement.

See `audit/JELLYFIN-NATIVE.md` and `audit/SEERR.md` for the service contracts.
