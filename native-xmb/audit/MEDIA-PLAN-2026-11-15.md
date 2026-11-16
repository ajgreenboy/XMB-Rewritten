# Media player and Jellyfin audit and plan (2026-11-15)

Branch `wt/media`. Everything marked PRESENT or PARTIAL was read in this tree (file and function cited). "Not verified" means
no run was made: this audit read code and docs, it did not drive a real Jellyfin server or the TV. Sources read:
`audit/MEDIA-CONTROLS.md`, `JELLYFIN-NATIVE.md`, `JELLYFIN-PLAYBACK.md`, `JELLYFIN-SETUP.md`, `JELLYFIN-MUSIC.md`,
`src/jellyfin_client.*`, `jellyfin_stream.*`, `jellyfin_shell.cpp`, `shell.cpp` (player options, resume store, subtitles,
audio tracks), `image_subtitles.cpp`, `media_controls.*`, `view.cpp` key bindings.

## Status per feature (before this change)

| Feature | Status | What exists, where | After phase 2 |
|---|---|---|---|
| Audio track switching, local | PRESENT | `Shell::refreshAudioTracks` / `chooseAudioTrack` (shell.cpp) over `QMediaPlayer::setActiveAudioTrack`, read back and refused with a reason when the backend does not follow. Not remembered. | PRESENT; remembered per title (phase 1) |
| Audio track switching, Jellyfin | PARTIAL | `buildPlayerOptions` lists the server's MediaStreams but maps them to the player's tracks by position. A transcode carries one audio stream, so on a transcode the other rows were disabled. `PlaybackInfo` never sent `AudioStreamIndex`. | DONE (phase 1) |
| Subtitles, sidecar (srt, vtt) | PRESENT | `subtitles::discover`, `Shell::prepareSubtitles`; choice kept for the session only. | PRESENT |
| Subtitles, image (.sup, .idx/.sub) | PRESENT | `image_subtitles.cpp`, `startImageDecode`; Jellyfin PGS fetched as `.sup` when chosen, else server burn-in (`chooseJellyfinBurnIn`). | PRESENT |
| Subtitles, embedded in a local file | MISSING | Panel says "Embedded subtitle streams are not switched yet." | DONE (phase 2): QMediaPlayer.subtitleTracks, text drawn from the video sink, remembered per title; picture-based streams not verified |
| Subtitles, Jellyfin text and PGS | PRESENT (English only) | `requestJellyfinSubtitles` filters with `isEnglishSubtitle`. Choice is session-only. | PRESENT (English only); remembered per title (phase 1) |
| Chapters (list, skip) | MISSING | No code reads chapter marks. L1/R1/Next/Previous only move in the queue (`Shell::skipMedia`). | DONE for Jellyfin (phase 1); local files still MISSING (Qt exposes no chapter list) |
| Playback speed | MISSING | No `setPlaybackRate` use in the player (grep of src; only the report body mentions a rate). | DONE (phase 2): 0.5x-2x, read back from the player |
| Seek / skip UX | PRESENT | 10 s steps (`Shell::seekMedia`, `MediaControls::seekTarget`), Beginning button, controls fade model (`media_controls.*`). No long skip. | PRESENT |
| A/V sync offset | MISSING | No setting or code found. | NOT POSSIBLE: Qt Multimedia has no audio delay on QMediaPlayer or QAudioOutput |
| Next episode | PARTIAL | A folder page's rows are the queue, so the next row plays when one ends (`PlaybackQueue`). No cross-season or server "Next Up" logic, no prompt. | DONE (phase 2) for Jellyfin: server Next Up with a countdown; local files already advance through the folder queue (no countdown) |
| Resume point, local | PRESENT | `media/resume` store and Continue Watching page (MEDIA-CONTROLS.md). | PRESENT |
| Resume point, Jellyfin: read | PARTIAL | Folder and search rows carry `UserData.PlaybackPositionTicks` (`parseItem`) and resume from it (`applyJellyfinResume`). | DONE (phase 1) |
| Progress report to Jellyfin | PRESENT | `JellyfinClient::reportPlayback`: start, progress every 10 s and on pause, stop with `PositionTicks`. The client never sent an explicit "played" mark; only the row flag changed locally. | DONE (phase 1: explicit played mark) |
| Home rows | MISSING | Library root lists libraries, Search, Sign Out. No Continue Watching, Next Up, Latest or Favourites from the server. | DONE: Continue Watching and Next Up (phase 1), Latest per library and Favourites (phase 2) |
| Search | PRESENT | `JellyfinClient::search`, `openJellyfinSearch`, music search. | PRESENT |
| Watched / unwatched marks | PARTIAL | Rows show "Watched" and "Resume at"; no toggle from the XMB. | DONE (phase 2): toggle in the Triangle menu plus a favourite toggle; the detail line reads Watched / Favourite |
| Quality / bitrate choice | PRESENT | `jellyfin/maxBitrateKbps`, quality list in the item menu and player panel. | PRESENT |
| Trickplay / seek thumbnails | MISSING | Not requested anywhere. | MISSING |
| Error messages | PRESENT | Specific text from the server's own words (`jellyfinFailureText`, JELLYFIN-PLAYBACK.md). | PRESENT |

## Gaps ranked for a couch user

1. Server-side resume and "what next" (Continue Watching, Next Up, played at 90 percent): a title started on another Jellyfin client
   could not be found from the XMB without browsing to it.
2. Audio track on transcodes, and remembering audio and subtitle choice per title: dual-audio and commentary titles forced a
   re-pick every time, and on a transcode the pick was impossible.
3. Chapters (list and skip): long films had no way to jump except 10 s steps.
4. Autoplay prompt across a show; Latest and Favourites rows; a mark-watched toggle.
5. Playback speed, A/V sync offset, embedded subtitles for local files, trickplay.

## Phase 1

Covers 1, 2, 3 and the Next Up row of 4; see `JELLYFIN-PLAYER.md`. Not done: Latest and Favourites rows, the autoplay prompt, a
watched toggle, speed, sync offset, local embedded subtitles, chapters for local files (Qt exposes no chapter list), trickplay.

## Phase 2

Items 1 to 5 of the ranked list's remainder: Play Next Episode (Jellyfin), watched and favourite toggles, Latest and Favourites home
rows, playback speed and embedded subtitles for local files; the A/V sync offset is not possible with Qt Multimedia. See the phase 2
section of `JELLYFIN-PLAYER.md`. Still not done: chapters for local files, trickplay.
