# Subtitle language-switch: Jellyfin SRT evidence (2026-11-18)

Bounded investigation of the "SRT language stays unchanged" report: whether selecting a different Jellyfin
subtitle track leaves the drawn text on the previous track. Scope is `Shell::chooseSubtitleTrack` /
`applySubtitleTrack` / `subtitleCueText` and the `subtitleReady` ordering (`jellyfin_shell.cpp`,
`shell.cpp`, `subtitles.*`), with no live server and no `shell.cpp` download work.

## Verified

1. **Baseline tests run green.** Built and executed `xmb-subtitle-tests` (Qt 6.12.0 msvc2022_64, Ninja,
   offscreen): all checks pass - discovery, `parse()`, `cueAt`, `displayName`, and the Jellyfin
   English-only rule (`isEnglishSubtitle`).

2. **`subtitles::parse()` clears `cues`/`skipped`/`bytes` up front (`subtitles.cpp:370-387`, `e82f6a4`).**
   This guard is defensive, not a fix for the reported symptom. In the Jellyfin flow each track is a fresh
   `subtitles::Track` parsed exactly once when its SRT arrives: `requestJellyfinSubtitles` rebuilds
   `subtitleTracks_` from scratch (`jellyfin_shell.cpp:719`) and each `subtitleReady` parses its stream a
   single time, so the re-parse-keeps-old-cues path the guard describes is not reachable in production
   either before or after the change. The re-parse cases in `tests/subtitle_tests.cpp` exercise the guard
   directly, not a production path.

## Selection path (Jellyfin text/SRT)

- `chooseSubtitleTrack(index)` (`shell.cpp:4774`) -> `applySubtitleTrack(index)` (`shell.cpp:4657`) sets
  `subtitleTrackIndex_=index`, records `subtitleChoice_[subtitleMediaPath_]`, and emits `changed()`.
- `subtitleCueText()` (`shell.cpp:4751`) reads `subtitleTracks_[subtitleTrackIndex_].cues` at
  `player_->position()`; the view draws the result every frame (`view.cpp:2873`). The drawn text is
  therefore the chosen track's cue and never another track's.

## Race / ordering analysis (`subtitleReady`, jellyfin_shell.cpp:342)

The handler maps `request` -> `streamIndex`, resolves the track by `jellyfinSubtitleKey(streamIndex)`
(unique per stream: `jellyfin:<itemId>#<streamIndex>`), parses the bytes, then auto-applies only when
there is no session choice, the stream is default, and nothing is selected yet
(`subtitleChoice_.contains(...) ? ... : isDefault && subtitleTrackIndex_<0`).

Races examined, each closed in the current code:

- A late default SRT arriving after a manual choice cannot revert it: `applySubtitleTrack` records
  `subtitleChoice_`, so the auto-default branch is unreachable once a choice exists.
- "Off" is recorded as an empty choice the same way, so a late default SRT cannot re-enable subtitles.
- A stale `subtitleReady` from a previous title is dropped by the `jellyfinSubtitleRequests_` request-id
  gate (monotonic request ids; the set is cleared in `requestJellyfinSubtitles`, jellyfin_shell.cpp:719).
- A text row is disabled ("Loading") until its SRT cues are present (`shell.cpp:4480`), so a user cannot
  select an unfilled track.
- Two streams stay distinct: `jellyfinSubtitleKey` is unique per stream index and each track is built and
  filled exactly once.

## Conclusion

The live switching path was **not exercised end-to-end**: `Shell::chooseSubtitleTrack` /
`applySubtitleTrack` / `subtitleCueText` live in `shell.cpp` and need a loaded `QMediaPlayer` (the Jellyfin
text path's only player dependency is `player_->position()` inside `subtitleCueText`); this investigation
reviewed them statically only. The reported "language stays unchanged" defect therefore **remains
unexplained and unverified** - the static race/path analysis above found no defect, but it does not
establish correct live behaviour, and no root cause is claimed. No code change is made and none is
recommended.

Non-English choices are intentionally filtered by the English-only rule (`isEnglishSubtitle`,
`jellyfin_client.h:85`; text and PGS alike), so a title that offers only English rows even when other
languages exist is by design, not the reported defect. `ISA.md:150` already records the unverified-switch
gap.
