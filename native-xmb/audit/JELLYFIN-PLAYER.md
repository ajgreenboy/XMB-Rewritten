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

# Phase 2: Play Next Episode, watched and favourite toggles, Latest and Favourites, speed, embedded subtitles

Branch `wt/media2`. Same constraints as phase 1: the existing sign-in, nothing sent to a real server (no 192.168.x.x host), every run
against `work/jellyfin-standin.ts` and local files.

## What was built

- **Play Next Episode.** When a Jellyfin Episode (one that carries a SeriesId) ends, the client sends the watched mark for it and, once
  the server has answered, asks `/Shows/NextUp?seriesId=` (skipping the finished episode itself and any watched one). A found episode is
  shown as one line over the video, "Play Next Episode: <show> S1E2 - <title> in N s. Cross plays now, Circle cancels.", counting down from
  ten (`jfplayer::kAutoplaySeconds`). Cross plays it now, Circle cancels and stays on the finished episode, the countdown reaching zero
  plays it; it joins the queue the page built (or is appended to it) so Next and Previous keep working. A server that names no next
  episode, answers an error, has no route, or a movie, a series-less item, music or Repeat One: nothing is asked or shown and the old
  behaviour (next row of the page, else the end) happens. The Playback panel has "Play Next Episode: On/Off" for episodes
  (preference `jellyfin/autoplayNext`, default on). Local files: the folder's rows were already the queue and still advance at once;
  no countdown was added there. Not built: the "final seconds / credits" trigger; only the end of the episode starts it.
- **Watched / unwatched and favourite toggles.** The Triangle menu of a Jellyfin video, series or season has "Mark as Watched/Unwatched"
  and "Add to/Remove from Favourites": POST or DELETE `/UserPlayedItems/{id}?userId=` and `/UserFavoriteItems/{id}?userId=`, each with
  the older `/Users/{user}/PlayedItems|FavoriteItems/{id}` retried once on a 404/405/501. The row changes only after the server
  answered (a notification says so); a refusal leaves the row as it was and says "The server did not accept ...". Rows read
  "Movie - Watched - Favourite - 00:20" (the detail line; folders show Watched/Favourite only when set). Not offered on libraries, home
  rows or music rows.
- **Latest and Favourites.** The libraries page now reads Continue Watching, Next Up, Favourites, then one "Latest in <library>" per
  video library (movies, tv shows, home videos, music videos, mixed; `jfplayer::latestEligible`), then the libraries. Latest is
  `/Items/Latest?userId=&ParentId=` (a bare JSON array; older `/Users/{id}/Items/Latest` on a 404), Favourites is `/Items?Filters=IsFavorite
  &Recursive=true`. A server without either answers an empty page ("Nothing new in this Jellyfin library." / "No favourites on Jellyfin
  yet."), not an error; a real HTTP error is still shown.
- **Playback speed.** "Speed: 1x" in the Playback panel opens 0.5x, 0.75x, 1x, 1.25x, 1.5x, 1.75x, 2x. The player is asked
  (`QMediaPlayer::setPlaybackRate`) and the rate it reports back is what is kept and shown; a backend that keeps another rate is named in
  the notification. The speed is reapplied when the next file loads and returns to 1x when playback is left.
- **A/V sync offset: not built.** Qt Multimedia has no audio delay on `QMediaPlayer` or `QAudioOutput`; a row that did nothing would be a
  lie. Doing it would need the audio path outside QMediaPlayer.
- **Embedded subtitle streams of local files.** `QMediaPlayer::subtitleTracks()` are listed after the sidecars as "<language or title>
  (embedded)". Choosing one calls `setActiveSubtitleTrack`; the text the player hands to the video sink (`subtitleTextChanged`) is drawn
  like a sidecar cue. The choice is remembered per title with the existing `media/choices` store (an index into the combined list). An
  embedded stream is never the default; a sidecar still is. A late `tracksChanged` is handled. Picture-based embedded streams (PGS,
  VobSub in a container) are not known to be decoded by the backend and show nothing; the row's note says so.

Code: `jellyfin_client.*` (`home(kind, library)`, `setUserFlag`, `nextEpisode`, `userFlagChanged`, `nextEpisodeReady`, `favorite`/`seriesId` on items),
`jellyfin_player.*` (countdown, labels, eligibility, speed steps), `jellyfin_shell.cpp`, `shell.cpp/.h`, `view.cpp` (the prompt line),
`subtitles.h` (`Track::embedded`), `preferences.cpp`.

## Tests

`xmb-jellyfin-player-tests` 47 -> 70 checks (countdown rounding, next-episode eligibility, labels, Latest eligibility, toggle labels,
speed steps and labels) and `xmb-jellyfin-playback-tests` 102 -> 132 (Latest and Favourites routes, queries, the bare-array answer and
the older route, the flag routes and methods and their retry, the failure answer, NextUp after the watched mark, the finished episode
never being its own successor, silent failures). No new test target, so CMake and `.github/workflows/tests.yml` are unchanged. The
stand-in gained `/Items/Latest`, favourites, DELETE `/UserPlayedItems`, SeriesId, `--clip <video>` (a real video on the direct-play route),
`--episodes-direct`, `--series-done`, `--no-latest`, `--no-favorites`, `--no-flags`, `--legacy-flags` and a fixed token (`STANDIN_TOKEN`).

## Verified in a scripted run (stand-in and local files, this PC)

The XMB (MSVC /W4, 0 warnings) was driven with `--script` against the stand-in serving the project's own 20 s test clip, with a stored sign-in:
the home page shows the new rows; Latest in Movies lists the newest first; Mark as Watched and Add to Favourites reached the server
(`/__reports`) and only then changed the row ("Movie - Watched - Favourite - 00:20", menu now offers Unwatched and Remove); with
`--no-flags` the row stayed and the log said the server did not accept it; with `--no-favorites --no-latest` the Favourites page is
empty with its message; an episode played to its end, was marked watched, Next Up named S1E2 and the prompt counted down 10 -> 3 on
screen, then played it; Cross played it at once; Circle cancelled and stayed; with `--series-done` nothing was offered and the old end
state remained; speed 2x moved the position about twice as fast; a local .mkv with an embedded text stream listed it, drew "Embedded cue
one" at 3.7 s and brought the choice back on the next play (`ffmpeg` from the project's own Jellyfin portable folder muxed the .mkv).

## Not verified

- Anything against a real Jellyfin server: `/UserFavoriteItems`, `/Items/Latest`, `Filters=IsFavorite` and `/Shows/NextUp?seriesId=` are
  from the 10.9+/12 API as I know it, and the stand-in shares that assumption. NextUp for a series has one episode on the servers I know;
  the code accepts a list and skips the finished and watched ones.
- On the TV / HTPC: how the one-line prompt reads from the couch, the ten second length.
- Embedded subtitles only with a text (SubRip) stream in Matroska; ASS styling, MP4 tx3g, picture-based streams, and what the Windows
  backend does with several embedded streams were not tried. Speed was timed through the player position, not by ear (pitch).
- The "final seconds" trigger and a countdown for local files were not built.
- Commits: the work is one logical change across shared files (shell.cpp/h, the client), so it is committed as a code commit and a
  docs commit, not one commit per item.
