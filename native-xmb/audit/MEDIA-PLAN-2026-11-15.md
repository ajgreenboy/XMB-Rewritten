# Media player and Jellyfin audit and plan (2026-11-15)

Branch `wt/media`. Everything marked PRESENT or PARTIAL was read in this tree (file and function cited). "Not verified" means
no run was made: this audit read code and docs, it did not drive a real Jellyfin server or the TV. Sources read:
`audit/MEDIA-CONTROLS.md`, `JELLYFIN-NATIVE.md`, `JELLYFIN-PLAYBACK.md`, `JELLYFIN-SETUP.md`, `JELLYFIN-MUSIC.md`,
`src/jellyfin_client.*`, `jellyfin_stream.*`, `jellyfin_shell.cpp`, `shell.cpp` (player options, resume store, subtitles,
audio tracks), `image_subtitles.cpp`, `media_controls.*`, `view.cpp` key bindings.

## Status per feature (before this change)

| Feature | Status | What exists, where |
|---|---|---|
| Audio track switching, local | PRESENT | `Shell::refreshAudioTracks` / `chooseAudioTrack` (shell.cpp) over `QMediaPlayer::setActiveAudioTrack`, read back and refused with a reason when the backend does not follow. Not remembered. |
| Audio track switching, Jellyfin | PARTIAL | `buildPlayerOptions` lists the server's MediaStreams but maps them to the player's tracks by position. A transcode carries one audio stream, so on a transcode the other rows were disabled. `PlaybackInfo` never sent `AudioStreamIndex`. |
| Subtitles, sidecar (srt, vtt) | PRESENT | `subtitles::discover`, `Shell::prepareSubtitles`; choice kept for the session only. |
| Subtitles, image (.sup, .idx/.sub) | PRESENT | `image_subtitles.cpp`, `startImageDecode`; Jellyfin PGS fetched as `.sup` when chosen, else server burn-in (`chooseJellyfinBurnIn`). |
| Subtitles, embedded in a local file | MISSING | Panel says "Embedded subtitle streams are not switched yet." |
| Subtitles, Jellyfin text and PGS | PRESENT (English only) | `requestJellyfinSubtitles` filters with `isEnglishSubtitle`. Choice is session-only. |
| Chapters (list, skip) | MISSING | No code reads chapter marks. L1/R1/Next/Previous only move in the queue (`Shell::skipMedia`). |
| Playback speed | MISSING | No `setPlaybackRate` use in the player (grep of src; only the report body mentions a rate). |
| Seek / skip UX | PRESENT | 10 s steps (`Shell::seekMedia`, `MediaControls::seekTarget`), Beginning button, controls fade model (`media_controls.*`). No long skip. |
| A/V sync offset | MISSING | No setting or code found. |
| Next episode | PARTIAL | A folder page's rows are the queue, so the next row plays when one ends (`PlaybackQueue`). No cross-season or server "Next Up" logic, no prompt. |
| Resume point, local | PRESENT | `media/resume` store and Continue Watching page (MEDIA-CONTROLS.md). |
| Resume point, Jellyfin: read | PARTIAL | Folder and search rows carry `UserData.PlaybackPositionTicks` (`parseItem`) and resume from it (`applyJellyfinResume`). |
| Progress report to Jellyfin | PRESENT | `JellyfinClient::reportPlayback`: start, progress every 10 s and on pause, stop with `PositionTicks`. The client never sent an explicit "played" mark; only the row flag changed locally. |
| Home rows | MISSING | Library root lists libraries, Search, Sign Out. No Continue Watching, Next Up, Latest or Favourites from the server. |
| Search | PRESENT | `JellyfinClient::search`, `openJellyfinSearch`, music search. |
| Watched / unwatched marks | PARTIAL | Rows show "Watched" and "Resume at"; no toggle from the XMB. |
| Quality / bitrate choice | PRESENT | `jellyfin/maxBitrateKbps`, quality list in the item menu and player panel. |
| Trickplay / seek thumbnails | MISSING | Not requested anywhere. |
| Error messages | PRESENT | Specific text from the server's own words (`jellyfinFailureText`, JELLYFIN-PLAYBACK.md). |

## Gaps ranked for a couch user

1. Server-side resume and "what next" (Continue Watching, Next Up, played at 90 percent): a title started on another Jellyfin client
   could not be found from the XMB without browsing to it.
2. Audio track on transcodes, and remembering audio and subtitle choice per title: dual-audio and commentary titles forced a
   re-pick every time, and on a transcode the pick was impossible.
3. Chapters (list and skip): long films had no way to jump except 10 s steps.
4. Autoplay prompt across a show; Latest and Favourites rows; a mark-watched toggle.
5. Playback speed, A/V sync offset, embedded subtitles for local files, trickplay.

## Phase 1 (this change)

Covers 1, 2, 3 and the Next Up row of 4; see `JELLYFIN-PLAYER.md`. Not done: Latest and Favourites rows, the autoplay prompt, a
watched toggle, speed, sync offset, local embedded subtitles, chapters for local files (Qt exposes no chapter list), trickplay.
