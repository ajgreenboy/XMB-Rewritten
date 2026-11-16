# Jellyfin music in the Music column

Written 2026-11-14 on branch `wt/jfmusic` (from `6b54ac9`). Everything below was checked in this worktree unless marked
UNVERIFIED. No credential of the owner's server was used or looked for: every run was against the local stand-in
(`work/jellyfin-standin.ts`) with test credentials the stand-in accepts.

## What changed for the owner

Before: all Jellyfin libraries, music included, lived under TV/Video Services > Jellyfin, and a song was just another
playable row there. Now:

- **Music > Jellyfin Music** (a row under Spotify, wearing the Jellyfin mark) opens the server's music libraries directly.
- **TV > Jellyfin** lists only the video libraries (and "Search Jellyfin", "Sign Out of Jellyfin"); music libraries no
  longer appear there, and the TV search no longer returns songs, albums or artists.
- **One session.** The music row does not sign in by itself. If the TV row's session is live it opens at once; if a
  remembered ("Stay Signed In") session exists it is restored silently, exactly as the TV row does; with no session it
  opens the same Jellyfin sign-in page the TV row uses and, once the sign-in is accepted, lands on the music pages. The
  Stay Signed In choice, Sign Out and the stored DPAPI record are the same ones. A sign-out from either place signs out of
  both, and the row's second line follows the session (see below).
- With no Jellyfin registered the Music column shows nothing extra (the "Set Up Jellyfin" row stays in TV/Video Services).

### Interpretation to confirm

The coordinator's clarification said the entry should "appear/disappear as that session signs in or out" and also that
"only when no session exists" it should route to the TV sign-in page. Both cannot hold literally (an entry that is gone
while signed out cannot route to a sign-in page), so this build keeps the row **while a Jellyfin app is registered** and
changes what its second line says: "Not signed in - Cross to sign in", "Artists, albums and playlists", or "No music
libraries" (after the server's library list has been read). Cross with no session goes to the existing TV sign-in page. If
the owner wants the row hidden while signed out, it is one condition in `Shell::refreshContent` (`shell.cpp:1286`).

## The pages (`src/jellyfin_music_shell.cpp`)

| Page | Request (`JellyfinClient::music`, `jellyfin_client.cpp:461`) |
|---|---|
| Music root | `/UserViews`, filtered to `CollectionType == music`; none: "This Jellyfin server has no music libraries."; one: its root rows; several: the libraries first (each opens the same root) |
| Artists | `/Artists/AlbumArtists?ParentId=<library>&SortBy=SortName` |
| Albums | `/Items?IncludeItemTypes=MusicAlbum&Recursive=true&ParentId=<library>&SortBy=SortName` |
| Playlists | `/Items?IncludeItemTypes=Playlist&MediaTypes=Audio&Recursive=true` (playlists live in their own view, so no `ParentId`) |
| Songs | `/Items?IncludeItemTypes=Audio&Recursive=true&ParentId=<library>&SortBy=SortName` |
| Recently Added | albums, `SortBy=DateCreated,SortName` descending |
| Random Albums | albums, `SortBy=Random`, one page of 50, no More row |
| an artist | `/Items?IncludeItemTypes=MusicAlbum&ArtistIds=<id>&SortBy=ProductionYear,SortName` (an artist's albums by year, not by name) |
| an album | `/Items?ParentId=<album>&IncludeItemTypes=Audio&SortBy=ParentIndexNumber,IndexNumber,SortName` (disc and track order) |
| a playlist | `/Playlists/<id>/Items` (the playlist's own order, never re-sorted) |
| Search Music | `/Items?SearchTerm=<t>&IncludeItemTypes=MusicArtist,MusicAlbum,Audio`; "Search Again" row |

Every request carries `UserId`, `StartIndex`, `Limit=200` (50 for Random), `EnableImageTypes=Primary`, `ImageTypeLimit=1`
and goes out with the Authorization header (no credential in any URL). A `More...` row opens the next 200 as a new page
(`StartIndex=200` ...), so a 25000-song library is read 200 rows at a time and the page never freezes
(`tests/jellyfin_music_tests.cpp` asks two windows of a 25000-song stand-in). The queue of a played track is the rows of the
page it was played from (an album, a playlist, a songs window, search results).

Covers: the server's Primary picture (`/Items/<id>/Images/Primary?maxHeight=480&tag=<tag>&quality=80`), at most 60 requests
per page open, one per album for a page of tracks (a track has no cover of its own: the album's `AlbumPrimaryImageTag`),
square-cropped to 192 px in the icon box exactly as the Spotify rows do, cached on disk by item id and tag
(`JellyfinClient::artwork`, 400 files, oldest out) and in memory (40 albums). A changed tag is fetched again.

## Playback and Now Playing

The audio player is the existing one (`startJellyfinMedia`, `reportJellyfinPlaybackState`): PlaybackInfo with an Audio direct
play profile (now mp3, aac, m4a, flac, ogg, oga, opus, wav, webm, mka; codecs aac, mp3, opus, flac, vorbis, alac, pcm), the
original file direct played from `/Audio/<id>/stream?static=true` through the loopback relay (`jellyfin_stream.*`, with the
Authorization header added upstream, so the player is never given a token), the server's HLS or http transcode
(`TranscodingUrl`, relayed the same way) for anything the profile does not list, start / progress (every 10 s) / stop reports
and a resume position when the server keeps one (Jellyfin keeps it for audiobooks, not for songs). Next and Previous walk
the queue; the Now Playing page shows the title, artist, album, the album cover and "Track n of m". A server stream carries
no tags the player reads, so the page takes them from the rows (`rememberJellyfinTracks`, `showJellyfinTrack`).

## Errors that name the real reason

`JellyfinClient` no longer answers every failed list with "Jellyfin could not complete that request." It says what happened:
`Jellyfin failed to answer (HTTP 500: "Error processing request."). Check the server's log.`, `Jellyfin refused the request
(HTTP 403). Sign out of Jellyfin and sign in again.`, `Jellyfin has no such page (HTTP 404: ...)`, `The Jellyfin server did not
answer (<the network's reason>)`. A JSON body is read for its `Message` (or `detail`/`title`), the text goes through
`scrubDiagnostic` (no token, no id) and the same line is in the log (`jellyfin: request failed: ...`).

Found on the way, fixed for all Jellyfin pages: `JellyfinClient::failed` set the shell's raw `message_` without a message
kind, so **no failure of a Jellyfin list was ever drawn** (an empty page and no text). It now goes through `showMessage`
(`jellyfin_shell.cpp`, the `failed` handler) and the empty page that failed to load is removed, so Circle returns to the
page before it. The same fix covers the "sign in again" and "maximum folder depth" messages.

## Stand-in server and tests

- `work/jellyfin-standin.ts` now serves music: one music library ("Music"; `--second-library` adds "Classical",
  `--no-music` removes them), 3 artists, 4 albums, 15 tracks, a "Road Trip" playlist, a distinct square cover per item,
  `Artists/AlbumArtists`, the album / artist / playlist / search / recent / random lists, paging (`--extra-songs 450` adds a
  filler album to page through), PlaybackInfo for audio, `/Audio/<id>/stream` and `/Audio/<id>/universal` serving a 30 s sine
  tone (WAV, 22.05 kHz mono, a different pitch per track, with Range support), `--music-transcode` (answers every track with
  a Transcode answer) and `--music-status <n>` (every music list answers that status). Strict like the rest: UserId, an
  Authorization header, `static=true`, `MediaSourceId` and an Audio direct play profile are required.
- `tests/jellyfin_music_tests.cpp` (`xmb-jellyfin-music-tests`, CMake option `XMB_JELLYFIN_MUSIC_BUILD_TESTS`, wired into
  `.github/workflows/tests.yml`): 65 checks against an in-process strict server: the request of every list (path, query,
  sorting, paging, no credential in a URL), what the parser keeps of an artist / album / track, 25000-song paging, search and
  the TV search without music, refusal of invalid ids / terms / offsets before anything is sent, covers (tag, size, disk
  cache hit, changed tag), audio PlaybackInfo and direct play through the relay (Authorization added upstream), and the error
  text for 500 / 403 / 404 / JSON bodies. `xmb-jellyfin-playback-tests` (62) and `xmb-jellyfin-setup-tests` (81) still pass.

## Scripted verification (stand-in, this PC, 1920x1080)

Build `C:\dev\out\jfmusic-build` (MSVC /W4: 0 warnings), data dir with one `apps.json` registering the stand-in, frames in
`C:\dev\out\jfmusic-run\`:

| Frame | Shows |
|---|---|
| `m1.png` | Music column, `Jellyfin Music` focused under Spotify, "Not signed in - Cross to sign in" |
| `s1-root.png` | after Cross: the shared sign-in page, test credentials typed through the keyboard, sign-in accepted, the music root |
| `s2-artists.png`, `s3-artist-albums.png`, `s4-album.png` | artists with covers, an artist's albums, an album's tracks (`Aurora Lane - 00:30`) |
| `s5-playing.png` | Now Playing: "Wake Up Call", Aurora Lane, Morning Static, the cover, "Track 1 / 5 Tracks", position 00:05 of 00:30, `mediaProvider Jellyfin`, `playMethod DirectPlay`; `s6-next.png` after NEXT: "Coffee Hum", the previous track's stop report and the next one's start report in the log |
| `r7-tv-jellyfin.png` | TV > Jellyfin after the same sign-in: Movies, Search Jellyfin, Sign Out; no Music |
| `r7-row-live.png`, `u1`..`u4` | the row says "Artists, albums and playlists" while signed in; a remembered session restores silently on Cross (log: `music uses the remembered session`); Sign Out from the music root returns to the column and the row says "Not signed in" again |
| `nm1-root.png` | `--no-music` server: "This Jellyfin server has no music libraries." |
| `lg1`..`lg4` | `--second-library --extra-songs 450`: two libraries listed; Songs page of 201 rows of 465 with `More...` (4 cover requests for 201 rows) |
| `se-results.png` | Search Music "har": the artist and the album (the stand-in's cover), Search Again |
| `er-artists-error.png` | `--music-status 500`: `Jellyfin failed to answer (HTTP 500: "Error processing request."). Check the server's log.` |
| `tc` run | `--music-transcode`: `playMethod Transcode`, the relay delivers `/Audio/<id>/universal`, the track plays |

The sign-in used the keyboard's real handlers; the highlighted key at the keyboard's start is not always where earlier scripts
assumed, so the working path is 3 DOWN, 3 RIGHT, Cross to reach Enter (`C:\dev\out\jfmusic-run\sign.txt`).

## Gate

See the end of this file (appended after the run).

## Not done / UNVERIFIED

- Nothing was run against a real Jellyfin server (no credentials, by instruction). The routes used are the ones the web client
  uses (`Artists/AlbumArtists`, `Items` with `IncludeItemTypes`/`ArtistIds`/`ParentId`, `Playlists/<id>/Items`); a server
  that answers one of them differently shows its own HTTP status and words on screen and in the log.
- Playlists that mix audio and video items are listed (`MediaTypes=Audio` is the server's own filter); a video item in one
  is not played from the music pages.
- Album art for a library with no Primary picture shows the XMB's own album placeholder; Now Playing then shows the default
  cover.
- Direct play of formats Qt's FFmpeg cannot decode (the profile lists only formats it normally can) falls to the server's
  transcode; that was exercised with the stand-in's `--music-transcode`, not with a real transcoding server.
- Real audio output: the runs played the 30 s test tone through this PC's output for a few seconds each (no volume or mute
  change was made; `XMB_AUDIO_DRY_RUN=1` does not silence playback).

# Phase 2 (2026-11-16, branch `wt/jfmusic2`)

Plan and the present / partial / missing audit: `MUSIC-PLAN-2026-11-16.md`. Same constraints as phase 1: the existing sign-in, every
run against `work/jellyfin-standin.ts` (extended) or the in-process fake servers of the unit tests, nothing sent to a real server.

## What was built

- **Audio reports and resume.** Start, progress (10 s) and stop reports for audio already existed (they share the video path). What
  was wrong: `finishJellyfinPlayback` kept a local resume point for any stop 10 s or more in, so a song left at 1:10 resumed at 1:10
  and its row said "Resume at". Now `jfplayer::audioStopOutcome` / `audioRowResumeMs` (`jellyfin_player.*`): a song never keeps a
  resume point; audio of 20 minutes or longer (`kAudioResumeMinMs`: audiobook, podcast) follows the video rule (position kept,
  played at 90 percent, no resume under 10 s in) and is also marked played explicitly when finished, which clears its server
  position. A song's play count stays the server's own rule on the stop report (a finished song is reported at its end position);
  no explicit mark is sent for songs so it cannot be counted twice.
- **Play and Shuffle Play** in the Triangle menu of an album, artist or playlist row (`Shell::startJellyfinPlayAll`): the tracks are
  read window by window (200 a request, at most `kPlayAllCap` = 1000) from the same lists the pages use (an artist: the new
  `MusicList::ArtistTracks`, `Items?IncludeItemTypes=Audio&ArtistIds=<id>&SortBy=Album,ParentIndexNumber,IndexNumber,SortName`),
  then queued like Cross on a track (`playJellyfinQueue`, shared with `openJellyfinMedia`). Shuffle Play turns the Shuffle setting
  on (`media/shuffle`, the panel's own preference) and starts at a random track. Next, Previous, Shuffle and Repeat already worked
  on the Jellyfin queue and were not changed.
- **Favourites, Recently Played, Most Played** rows in the music root (`MusicList::Favorites` = songs and albums with
  `Filters=IsFavorite`; `RecentlyPlayed` = `Filters=IsPlayed&SortBy=DatePlayed&SortOrder=Descending`; `MostPlayed` =
  `SortBy=PlayCount&SortOrder=Descending`, the row detail reads "N plays"). Rows show "Favourite". The Triangle menu of a song and
  of an album has Add to / Remove from Favourites (phase 2's `/UserFavoriteItems/{id}` through `setUserFlag`; the row changes only
  after the server answered). Artists and playlists are not offered the flag, and artists are not in the Favourites list: I am not
  sure of the server's route for favourite artists and did not guess.
- **Lyrics.** `JellyfinClient::lyrics` asks `/Audio/{id}/Lyrics` when an audio item starts; Now Playing shows the current line (40 px)
  and the next (28 px, dimmer) under the cover. Only timed lyrics (every line has a `Start`) are shown: untimed text has nothing to
  follow the song with and the page has no scrolling area, so it is not displayed (it is read and dropped, logged as "not timed").
  A server or song without lyrics (404, an old server, any failure) shows nothing and raises no error. Not offered: a Favourite
  toggle on the Now Playing page itself (Triangle there opens the console's control bar, not an options list, so there is no row to
  add it to without new artwork).
- **Skipped on purpose:** gapless playback (one `QMediaPlayer` source per track, the next stream is requested only when the previous
  one ends; avoiding the gap needs a second player or a prefetched source, which is large and risky) and a `RepeatMode` that follows
  the player in the report body (cosmetic).

## Stand-in and tests

- `work/jellyfin-standin.ts`: audio reports are now accepted in strict mode (the music PlaybackInfo did not register its
  PlaySessionId, so every audio report was answered 400 before), tracks carry `UserData` (Played, PlayCount, IsFavorite,
  LastPlayedDate, position for long audio), flag routes accept tracks and albums, `Filters=IsFavorite` / `IsPlayed` lists, an artist's
  songs by `ArtistIds`, `/Audio/{id}/Lyrics` (the first track of every album, timed; `--plain-lyrics`, `--no-lyrics`) and `--audiobook`
  (one 25 minute item "The Long Story" that keeps a position). Its stand-in assumption that only long audio keeps a position is
  a guess about the real server, see below.
- Tests (no new target, so CMake and `.github/workflows/tests.yml` are unchanged): `xmb-jellyfin-player-tests` 70 -> 100 checks
  (audio stop rules, the 20 minute line, row resume, played mark, Play All windows, lyric index) and `xmb-jellyfin-music-tests`
  65 -> 100 (audio start / progress / stop bodies, ArtistTracks request, Favourites / Recently Played / Most Played requests and parse,
  play count, lyrics timed / untimed / 404 / oversized / invalid id). `xmb-jellyfin-playback-tests` (132) and setup (81) unchanged.

## Scripted verification (stand-in, this PC, 1920x1080, build `C:\dev\out\jfmusic2`, MSVC /W4: 0 warnings)

Frames in `C:\dev\out\jfmusic2-run\`: `b1` the Triangle menu of an album row (Play, Shuffle Play, Open); `b2`/`b3` Now Playing after
Play, then Next ("Foghorn", Track 2 of 3 Tracks); `c1`..`c3` Favourites ("Coffee Hum - Favourite"), Recently Played (the song played
last first) and Most Played ("Signal Fade - 3 plays") after the seed script played them against the stand-in; `d2`/`e1` the album and
the song rows reading "Favourite" after the server accepted the flag (log: `favourite set for ...`), `d3` the song's Triangle menu
(Play, Add to Favourites, Information); `f1`/`f2` Now Playing with the timed lyric line and the next one; `g`/`h` the audiobook
left at 14.4 s is resumed by a new app run at 14.4 s (log: `resume applied at 14396 ms`).

## Unverified

- Everything against a real Jellyfin server. The routes (`/Audio/{id}/Lyrics`, `Filters=IsPlayed` with `SortBy=DatePlayed` and
  `PlayCount`, `Filters=IsFavorite` on `Audio,MusicAlbum`, `ArtistIds` with `IncludeItemTypes=Audio`) are from the Jellyfin API as I
  know it; the stand-in shares those assumptions.
- That the real server keeps a position only for long audio and counts a play from the stop report alone (the stand-in does; the
  client does not depend on it: a position a server did keep for a song is ignored by the row).
- That `/Audio/{id}/Lyrics` timed lines use `Start` in 100 ns ticks (that is the documented `LyricLine` shape); how the lyric lines
  look from the couch (40 px over the visualizer bars) and what a long audiobook does on the TV.
- Real audio output: the runs used `XMB_AUDIO_DRY_RUN=1` plus the stand-in's 30 s test tone.
