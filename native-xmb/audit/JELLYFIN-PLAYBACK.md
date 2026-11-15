# Jellyfin playback failures and the stale backdrop

Written 2026-11-14 on branch `wt/jfplay`. Everything below was checked in this worktree unless marked INFERRED. No credential
of the owner's server was used: the server was read only through its log files, and the XMB was driven against a local stand-in.

## What the owner saw

1. A transcoded title ("Boyz n the Hood", "Bridge to Terabithia", "Casino") stops at once with "the media could not be
   played, check the server and its transcoding support". The XMB log says `Could not open media. FFmpeg error
   description: Server returned 5XX Server Error reply`. DirectPlay titles ("Batman: Mask of the Phantasm") play.
2. Turning a PGS subtitle on says "The server did not deliver this PGS stream as .sup", then falls back to burn-in.
3. On a Jellyfin folder page the full-screen backdrop stays on the first item's art while focus moves.

## What the server log shows (read-only, `log_20261114.log`)

- Every failing attempt is `PlaybackInfo` (the `User policy ... EnableVideoPlaybackTranscoding: True` line), then
  `stopped playback` 0.35 to 0.4 s later. No `[ERR]`. No `FFmpeg.Transcode-*.log` is created (the newest is 11-11).
- Jellyfin's HLS design explains the missing ffmpeg: `main.m3u8` is a *virtual* playlist built from the runtime
  (`DynamicHlsController.GetVariantPlaylistInternal` never calls `StartFfMpeg`); ffmpeg starts on the first segment
  request. So the failure is at `master.m3u8` or `main.m3u8`, before any segment.
- `SubtitleEncoder` ran at 21:14:13 for "Boyz n the Hood" (the `.sup` request) and finished 21:15:17: **64 seconds** to
  extract every subtitle stream of the file (27 `.sup` files).
- The server's transcodes of 11-11 (an episode of about 22 minutes, `h264_nvenc`, burned-in subtitles) ran fine, so the
  server's transcoding itself works.

## Root causes

### 1. The HLS relay refuses long films' playlists (`src/jellyfin_stream.cpp`, was `kMaxPlaylistBytes`, line 20)

The player is given a loopback URL; `JellyfinStream` relays it to Jellyfin with the Authorization header and rewrites the
playlists so every sub-request also comes back through the relay. It read a playlist into memory with a hard limit of
**1 MiB** and answered `502` beyond it (`pump()`/`finish()`, `fail(request)` with no reason), which ffmpeg reports as
"Server returned 5XX". Jellyfin writes the **whole query string of the request on every segment line** of `main.m3u8`
(`CreateMainPlaylistRequest` is given `Request.QueryString`, each line is `hls1/main/N.ts?<query>&runtimeTicks=...&
actualSegmentLengthTicks=...`), about 0.6 to 1 KB per line at one line per 6 s segment. A 20 minute episode is 0.15 MB, a
2 hour film is about 1 MB and a 3 hour one 1.5 MB. DirectPlay never fetches a playlist, which is why it is unaffected.

Evidence: reproduced in `tests/jellyfin_playback_tests.cpp` (a 2200-segment playlist, 1.4 MB, relayed whole; with the old
1 MiB constant restored the same test fails with the 502). Not confirmed on the owner's server (no login, and the 5xx
leaves no server-side trace because it is the XMB's own reply): see "What to retry" for the line that will confirm or
refute it. The 21:15:32 case "Bridge to Terabithia" (95 minutes) is the least certain fit: it needs a query of about
1.1 KB per line. If the retry names a different failure, the new log line says which.

Fix: the limit is now 16 MiB (`kMaxPlaylistBytes` in `jellyfin_stream.h`), and a refusal is no longer silent (below).
Also: a `Range` header from the player is no longer forwarded for `.m3u8` requests (a generated document; a range only
invites a 416 or a partial list). The relay's upstream user agent is now `XMB/1.0`, the same as every other request.

### 2. The PGS `.sup` request gave up after 15 seconds (`src/jellyfin_client.cpp`, `get()`, was
`setTransferTimeout(RequestTimeoutMs)` = 15 s)

Choosing a PGS subtitle requests `GET /Videos/{item}/{source}/Subtitles/{n}/Stream.sup` (the route is right for
Jellyfin 10.9 to 12: `Stream.{format}` with the stream index). The server answers only after it has extracted all subtitle
streams (64 s measured above). `get()` set Qt's transfer timeout (inactivity) to 15 s for every request, which silently
overrode the 120 s deadline `send()` meant for image subtitles, so the request was aborted with no bytes and reported as
"The server did not deliver this PGS stream as .sup". Text subtitles (SRT) are extracted the same way and had the same
15 s limit. Fix: both subtitle requests now use a 180 s transfer timeout and deadline. Regression test: the stand-in
answers the `.sup` after 16.5 s.

### 3. A transcode URL could be re-encoded (`withoutSecrets()` in `src/jellyfin_client.cpp`)

The credential keys (`ApiKey`, `api_key`, ...) are removed from the server's `TranscodingUrl` (the Authorization header is
what authenticates). The old code decoded every value and re-added it, which turned `%2B` into a literal plus (a space to
the server) and dropped the encoding of an ampersand inside a value. Real Jellyfin values are mostly plain, so this is
probably not what the owner hit, but it changed what the server reads. The query is now filtered pair by pair in its
encoded form and put back untouched.

### 4. Stale backdrop (`src/jellyfin_shell.cpp`, `pageReady`, old lines 104-108)

The page asked for exactly one backdrop, `if(backdrop.isEmpty() && !media.backdropTag.isEmpty())`: the first playable
item's. `jellyfinBackdropId_` held that one id and the `artworkReady` handler accepted only that id. Focus changes never
reached the backdrop code at all. The fix keeps every item's backdrop tag from the page (`jellyfinBackdropTags_`) and
re-evaluates on every `changed` (`syncJellyfinBackdrop`):

- the focused row decides: a cached picture is applied at once, otherwise the request waits 120 ms of stillness (skimming
  does not ask for every picture) and the client's disk cache keyed by item id and tag is used first;
- a picture arriving for an item that is no longer focused is kept in a 12-entry cache but never drawn (the stale-completion
  case); a failed load for the focused item, and an item with no backdrop, clear the picture, so art is never attributed to
  the wrong film; rows that are not items (More, Search, Sign Out) keep what is shown;
- the view already cross-fades a changed page backdrop (`View::syncFocusedBackground`), so there is no flicker;
- folders (series, seasons) use their own backdrop like items.

## Diagnostics (so the next failure names itself)

- `JellyfinStream` records every error reply it relays (and every refusal of its own, with its reason) and emits
  `upstreamRejected(status, "GET /videos/<id>/main.m3u8", detail)`; the XMB log gets
  `jellyfin: stream request rejected: GET /videos/<id>/main.m3u8 -> HTTP 500 -- "Error processing request."` as it happens.
- When the player gives up, `JellyfinClient::probeStream` issues the same URL itself (same Authorization header and user
  agent, one GET, 8 s, first 2 KB read) and the log carries one line
  `jellyfin: stream open failed: player="..." stream saw ... probe GET <path> -> HTTP <status> "<first 300 bytes>"`.
- `scrubDiagnostic()` flattens control characters, hides `api_key=`/`Token=`/`Authorization:` values, every 32-hex id or
  token and any opaque run of 28+ characters, and cuts to 300 characters. Neither the query string nor a token is logged.
- The on-screen text is specific: "Jellyfin failed to start the stream (HTTP 500: "Error processing request."). Check the
  server's log and its transcoding settings.", "... refused the stream (HTTP 401). Sign out of Jellyfin and sign in
  again.", "The XMB could not relay the stream: the playlist is larger than 16 MB.", "Jellyfin answers (HTTP 200) but the
  player could not open the stream: <reason>.". The old generic sentence remains only when nothing is known.

## The stand-in server and the tests

- `work/jellyfin-standin.ts` (`bun work/jellyfin-standin.ts --port 8099 --log <file>`): sign-in, views, a folder of five
  movies (four backdrops, one without, the first answering 700 ms late), PlaybackInfo with DirectPlay and Transcode
  responses (`TranscodingUrl` relative, `ApiKey` last, as Jellyfin 10.9+ builds it), `master.m3u8`, `main.m3u8`,
  segments, `.sup`/`.srt`, reports. Strict: needs an Authorization header or ApiKey, `mediaSourceId`, `DeviceId`,
  `PlaySessionId`; a deviation answers a 4xx/5xx with its reason. `--hls-status 500 --hls-body "..."` forces an error.
  Logs query keys only, never values. Test credentials are generated per run; any user name and password sign in.
- `tests/jellyfin_playback_tests.cpp` (`xmb-jellyfin-playback-tests`, CMake option `XMB_JELLYFIN_PLAYBACK_BUILD_TESTS`,
  wired into `.github/workflows/tests.yml`): 62 checks against an in-process strict server: DeviceProfile and PlaybackInfo
  body (DeviceId equal in header, body and profile; the hls transcoding profile; burn-in needs an Encode subtitle profile
  and no direct play), the transcode URL the client hands the player (relative URL resolved, key removed, encodings kept),
  the relay end to end (master, main, segment, the Authorization header on every upstream request, no key in any URL),
  a playlist beyond 1 MiB, a playlist beyond the limit (502 with a reason), an upstream 500 (relayed, reported, scrubbed),
  the probe, a foreign host refused, `scrubDiagnostic`, and the 16.5 s `.sup`. It takes about 20 seconds.

## Scripted verification (stand-in, this PC)

Backdrop (build `C:\dev\out\jfplay-build`, frames `C:\dev\out\jfplay-run\bd-*.png`): sign in, open the folder, press Down
five times. Sampled background pixel (1500,950): A (120,24,24) red, B (24,120,24) green, C (24,24,120) blue, D
(no backdrop) back to the plain XMB background, E (120,24,120) magenta, then Up four times: A red again from the cache.
A skim (Down Down within 0.6 s, with A's reply 700 ms late) ends on C, not on A. Transcode: against the stand-in the
relay delivers master, main and segments (it ends in "Invalid data" only because the stand-in's segments are not video);
with `--hls-status 500` the screen reads the message above and the log carries both lines.

## What to retry on the HTPC

1. Install this build. Play "Bridge to Terabithia" or "Casino" (a title that failed before). If it plays, cause 1 was it.
2. If it still fails, open the XMB log and read the two lines beginning `jellyfin: stream request rejected:` and
   `jellyfin: stream open failed:`. They give the failing request (path only), the HTTP status and the first bytes of the
   server's reply: `HTTP 500/400` with a server message means Jellyfin itself refuses (read the server log at that time and
   send the line); `(not relayed)` with "the playlist is larger than" or "the playlist names another server" means the relay;
   `probe ... -> HTTP 200` with the stream failing anyway means the player cannot decode what the server sends.
3. Turn a PGS subtitle on for "Boyz n the Hood". Expect a wait of about a minute while the server extracts, then the
   subtitle (log line `PGS subtitle stream 3 arrived, N bytes; decoding`) instead of "did not deliver".
4. Open a Jellyfin library folder with several movies and move focus: the full-screen art should follow the focused item,
   disappear on one without a backdrop, and not jump back to an earlier item.

## Gate

`bun work/v2-fixtures.ts` then `bun work/ab-layers.ts --all --exe C:/dev/out/jfplay-build/re-ps3-native.exe` on this branch's
build (MSVC /W4: 0 warnings): `states: 28/28 pass` (2026-11-14). `xmb-jellyfin-playback-tests` 62 checks and
`xmb-jellyfin-setup-tests` 81 checks, 0 failures. No reference was saved and no state was added. `jellyfin-signin` is unchanged (the sign-in page does not use the backdrop).

## Not done

- The reply to a request the stream refused is a plain 502/504 to ffmpeg; the reason travels in the log and on screen only.
- Nothing was verified against a real Jellyfin 12 server (no credentials by instruction).
