# Jellyfin player: server resume, home rows, audio and chapters (phase 1)

Branch `wt/media`. Plan and audit: `MEDIA-PLAN-2026-11-15.md`. Everything here uses the existing Jellyfin sign-in; no new credential.
All runs and tests used the stand-in `work/jellyfin-standin.ts` and local code. No request was sent to a real Jellyfin server, a
192.168.x.x host or the TV.

## What was built

- **Home rows.** The Jellyfin libraries page now starts with `Continue Watching` (`/UserItems/Resume`, older
  `/Users/{id}/Items/Resume` on a 404) and `Next Up` (`/Shows/NextUp`). Rows carry the server's `UserData`, so a position saved by
  another client resumes here ("Resume at m:ss"). Episodes read "Show S2E5 - Title". A server without the route gives an empty page
  ("Nothing to continue watching on Jellyfin."), not an error; a real HTTP error is still shown. A home page queues only the chosen
  row, because its neighbours are other shows.
- **Progress and played.** Start, progress (10 s and on pause) and stop reports already existed; they now also carry
  `AudioStreamIndex` / `SubtitleStreamIndex` when set. On stop at the end, or at 90 percent or more (`jfplayer::reachedPlayed`), the
  client also POSTs `/UserPlayedItems/{id}?userId=` (older `/Users/{id}/PlayedItems/{id}` on a 404) and the row shows Watched with no
  resume point. A stop under 10 s in keeps no resume point.
- **Audio.** A transcode carries one stream, so choosing another audio row re-opens the title at the same position with
  `AudioStreamIndex` in `PlaybackInfo` (direct play off) and in the transcode URL. A direct play still switches in the player.
- **Remembered choice per title.** Preference `media/choices` (bounded to 64, validated): Jellyfin keyed by item id (server stream
  index), local files by the resume path hash (track ordinal). Audio and subtitle choices return on the next play; a remembered
  audio stream that is not the server default re-requests a transcode once.
- **Chapters.** Read from `MediaSources[].Chapters`, else `Items?ids=&fields=Chapters` (silently none on failure). The Playback
  (Triangle) panel gets `Chapters: n of m`, which opens a list and jumps. L1/R1, Next and Previous skip chapters; past the last
  chapter (or before the first) they fall through to the next or previous item. Back within 3 s of a chapter start goes to the previous
  chapter, else restarts the current one. The video panel's Next button uses the same call. Local files have no chapters.

Code: `src/jellyfin_player.*` (pure logic), `jellyfin_client.*` (`home`, `markPlayed`, `chapters`, `audioStreamIndex`),
`jellyfin_shell.cpp`, `shell.cpp`, `preferences.cpp`.

## Tests

`xmb-jellyfin-player-tests` (47 checks: ticks, the 90 percent boundary, stop outcome, chapter navigation, audio stream pick, choice
store and validator) and `xmb-jellyfin-playback-tests` (102 checks, about 40 new: home routes and fallbacks, report bodies and ticks,
played route and fallback, `AudioStreamIndex`, chapters). Both are wired into CMake and `.github/workflows/tests.yml`. The stand-in
gained Resume, NextUp, played marks, reports that change state (`GET /__reports`), chapters, a second audio stream, `--no-home`,
`--legacy-home` and `--no-chapters`.

## Not verified

- Against any real Jellyfin server: the route names are from the 10.9+/12 API as I know it and the stand-in shares that assumption.
  `/UserItems/Resume`, `/UserPlayedItems/{id}` and whether PlaybackInfo carries `Chapters` need a real check.
- On the TV / HTPC: how the new rows look, the feel of chapter skip on a controller, audio re-open time on a real transcode.
- Whether the server's own 90 percent rule and the explicit played mark ever disagree (both are sent).
- The shell flows (home page, audio re-open, chapter list, remembered choice) were built and compiled, but only the client and pure
  logic have automated tests; the shell paths were not driven against the stand-in in a scripted run.
- Not done: Latest/Favourites rows, autoplay prompt, watched toggle, speed, sync offset, local embedded subtitles, trickplay.
