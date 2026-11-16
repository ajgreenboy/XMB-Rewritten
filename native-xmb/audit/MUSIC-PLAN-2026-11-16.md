# Jellyfin music audit and plan (2026-11-16)

Branch `wt/jfmusic2` (from `299d45e`). PRESENT / PARTIAL / MISSING were read in this tree (file and function cited). Nothing was run
against a real server: every check in this work uses `work/jellyfin-standin.ts`, the in-process fake servers of the unit tests, and
local files. Sources read: `JELLYFIN-MUSIC.md`, `JELLYFIN-PLAYER.md`, `MEDIA-PLAN-2026-11-15.md`, `jellyfin_music_shell.cpp`,
`jellyfin_shell.cpp`, `jellyfin_player.*`, `jellyfin_client.*`, `playback_queue.*`, the Now Playing page (`view.cpp drawMusicPage`),
`tests/jellyfin_music_tests.cpp`, `tests/jellyfin_player_tests.cpp`.

## Status (before this change)

| Feature | Status | What exists, where |
|---|---|---|
| Start / progress / stopped reports for audio | PRESENT | `startJellyfinMedia` -> `reportJellyfinPlaybackState` (start, progress every 10 s and on pause) and `finishJellyfinPlayback` (stop at the end position when completed) in `jellyfin_shell.cpp`; `JellyfinClient::reportPlayback`. Audio shares the video path. |
| Played / play count for songs | PARTIAL | The stop report reaches the server, which owns PlayCount. `finishJellyfinPlayback` skips the explicit played mark for all audio (`!jellyfinPlayback_.audio`), which is right for songs but leaves a finished audiobook unmarked. |
| Resume for audiobooks / podcasts | PARTIAL | The row's `PlaybackPositionTicks` is parsed (`parseItem`) and applied (`applyJellyfinResume`). BUT `finishJellyfinPlayback` stores a local resume point for every stop 10 s or more in, so a song abandoned at 1:10 resumes at 1:10 the next time it is played in the session, and the music row shows "Resume at". Wrong for songs. |
| Queue, next / previous | PRESENT | `PlaybackQueue` (`playback_queue.h`); the rows of the page a track is played from are the queue (`openJellyfinMedia`). |
| Shuffle / repeat | PRESENT | `media/shuffle`, `media/repeatMode` apply to the Jellyfin queue (`openJellyfinMedia`), Triangle panel rows (`buildPlayerOptions`), status planes on the music page (`drawMusicPage`). The report body always says `RepeatNone` (cosmetic). |
| Play all for album / artist / playlist | PARTIAL | Cross on a track plays the page as the queue, so album and playlist pages play all from the chosen track. There is no way to start an album, playlist or artist from its row without opening it, and an artist has no "all songs" queue at all. |
| Favourites: toggle | PARTIAL | Phase 2 endpoints (`setUserFlag`, `toggleJellyfinFlag`) exist, but the Triangle menu hides them for music (`shell.cpp` `pages_.last().jellyfinMusic.isEmpty()` and `mediaKind!=Music`), and the Now Playing panel has none. |
| Favourites / Latest rows | PARTIAL | Video home has Favourites and Latest (`jellyfin_shell.cpp` pageReady). Music root: only "Recently Added" (albums by `DateCreated`) and "Random Albums". No Favourites. |
| Recently played / most played rows | MISSING | No `Filters=IsPlayed` with `SortBy=DatePlayed` or `PlayCount` request anywhere. |
| Search | PRESENT | `MusicList::Search`, "Search Music" row. |
| Lyrics | MISSING | Nothing requests `/Audio/{id}/Lyrics`; `drawMusicPage` has title, artist, album and codec only. |
| Gapless / transition | MISSING | One `QMediaPlayer` source per track (`startJellyfinMedia` stops, clears the source, requests PlaybackInfo, then plays): a gap of one round trip between tracks. Not changed (needs a second player or a prefetch of the next stream; large and risky). |
| Now Playing metadata and artwork | PRESENT | `showJellyfinTrack`, `applyJellyfinMusicArt`, album cover and "Track n of m". |

## Ranked for a couch user

1. Songs must not resume mid-song and plays must count: audio reporting and resume rules (fix and tests).
2. Play an album, an artist's songs or a playlist with one button, shuffled or not.
3. Favourites row, Recently Played and Most Played rows, favourite toggle on any music row and on the playing track.
4. Lyrics on Now Playing when the server has them.
5. Gapless: skipped (large, risky).

## What is built (one commit each, with unit tests)

1. Audio reporting and resume: songs never keep a local resume point or "Resume at"; audio of 20 minutes or longer (audiobook,
   podcast) resumes from the server's position and keeps one; a finished long audio item is marked played explicitly (a song's play
   count is the server's, from the stop report, so a song is never marked twice).
2. Queue: "Play All" and "Shuffle" on album, artist and playlist rows (Triangle menu), loading the tracks in the right order.
3. Favourites, Recently Played and Most Played rows in the music root; favourite toggle in the Triangle menu of every music row and
   in the Now Playing panel.
4. Lyrics: decided after 1 to 3, see `JELLYFIN-MUSIC.md`.

## Result

All four built; gapless skipped. See `JELLYFIN-MUSIC.md` "Phase 2" for what each does, the tests, the scripted runs and what is
unverified on a real server or the TV. Not built: untimed lyrics, a favourite toggle on the Now Playing page, favourite artists,
gapless.
