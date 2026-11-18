# SteamCMD inside the XMB

Status: implemented on `wt/steamcmd`. **Everything below that involves a real SteamCMD or a real Steam account is
unverified**: it was built and tested against a fake steamcmd (`tests/steamcmd_standin.cpp`) that replays the console
behaviour the sources describe. The owner tests the real thing. The last section lists exactly what that means.

The XMB installs, updates and validates Steam games with Valve's command-line client, SteamCMD, instead of handing each
one to the full Steam window (`steam://install/<id>`). The XMB collects the Steam account name, the password and the
Steam Guard code on its own on-screen keyboard (masked where secret, paste supported, like the Jellyfin sign-in) and
gives them to SteamCMD. Games are still **launched** through the Steam client (`steam://rungameid`), because most need
its Steamworks. Hiding the client is out of scope. Uninstall stays a Steam-client hand-off (see "Not done").

## Where it lives

| Piece | File |
|---|---|
| Pure helpers, `Secret`, `OutputScanner`, command lines, zip check, manifests, `Run` (one steamcmd child) | `src/steamcmd.{h,cpp}` |
| The XMB pages, jobs, download of SteamCMD itself | `src/steamcmd_shell.cpp` (+ small hooks in `shell.cpp`, `view.cpp`, `preferences.cpp`) |
| Tests | `tests/steamcmd_tests.cpp`, fake steamcmd `tests/steamcmd_standin.cpp` (CMake option `XMB_STEAMCMD_BUILD_TESTS`, in `tests.yml`) |

## What the person sees

- **Users > Steam Account > SteamCMD** (also "Set Up SteamCMD" on the item menu of a Steam title that is not installed):
  - status (where steamcmd.exe is, or "Not set up");
  - **Choose SteamCMD Folder** (the in-XMB folder browser, with Type a Path), **Look for SteamCMD** (usual folders:
    `C:\steamcmd`, `C:\SteamCMD`, `D:\SteamCMD`, Program Files, the user's `steamcmd` folders, PATH), **Download SteamCMD**;
  - **Sign In** / **Account**, **Sign Out of SteamCMD**, **Install Location** (opens a list of Steam libraries, starting
    on the active destination. Cross selects a library for new games and Circle returns);
  - **About Your Password** (the rules below, in words).
  Nothing is found or set implicitly: SteamCMD is "set up" only after an explicit choice (so a fresh install behaves
  exactly as before). `steamcmd/path`, `steamcmd/account` (the name only) and `steamcmd/library` are the only settings.
- **Sign In** page: Steam Account Name (keyboard), Password (masked keyboard, Paste works), Sign In, Cancel. After Sign
  In the page shows what SteamCMD is waiting for: nothing (working), a **code** (the keyboard opens by itself, masked),
  or **Approve on Your Phone** (no code; an instruction row). A wrong password, a wrong code, a timed-out phone
  approval and a rate limit end the attempt with SteamCMD's own words; the password is erased and has to be retyped.
- **Install**: Cross on a Steam title that is not installed (Game column, All Games, Steam Store rows) installs it with
  SteamCMD when SteamCMD is set up and signed in; the item menu then offers **Install with SteamCMD** and **Install in
  Steam Client** (the old `steam://install` hand-off). For an installed title the item menu offers **Update with
  SteamCMD** and **Validate with SteamCMD**. When SteamCMD is not set up everything is the old hand-off.
- **Download Management** gets one row per SteamCMD job above the Steam client's own rows: "SteamCMD - Downloading 43%
  (410.1 MB of 953.7 MB)", queued/finished/failed/stopped states, the real failure text. Cross on a running job stops it
  (terminate, then kill 1.5 s later), on a waiting or finished one removes it. Jobs run one at a time.
  Unfinished jobs are saved per XMB data directory in `steamcmd-jobs.json`, with the game, operation and destination.
  After restarting XMB they appear under **Network > Download Management** as **Saved for Retry**. Cross opens
  **Retry** or **Remove Job**. Retry runs SteamCMD again in the original folder, so SteamCMD can reuse compatible
  partial files. Remove Job keeps the files. Restoring the list starts no process, and the list contains no credentials.

## SteamCMD behaviour the code relies on, with sources

I could not read Valve's wiki page directly: `developer.valvesoftware.com` answers automated requests with a bot
challenge (a 403, then an interstitial page), which I did not try to get around. What I could confirm came from the
search-result excerpts of that page and from third-party pages that quote real console output. **Sourced** below means
a page I read states it; **assumed** means nothing I could read states it.

| Behaviour | Status | Source |
|---|---|---|
| `force_install_dir <path>` must come **before** the login (`+force_install_dir` ... `+login`) | sourced (excerpt of Valve's wiki: "use force_install_dir before logon") | [Valve Developer Community: SteamCMD](https://developer.valvesoftware.com/wiki/Steamcmd) (search excerpt), [Kernelhost guide](https://www.kernelhost.com/en/blog/install-a-game-server-with-steamcmd) |
| `app_update <id> [validate]`; `validate` checks all files | sourced | same |
| Steam Guard by email: "check your e-mail for a Steam Guard access code and enter it"; the prompt is `Steam Guard code:` | sourced for the prompt text | [Renode guide](https://www.renode.space/blog/steamcmd-login-and-steam-guard) |
| Mobile authenticator code prompt `Two-factor code:` | sourced | same |
| Mobile approval: `This account is protected by a Steam Guard mobile authenticator.`, `Please confirm the login in the Steam Mobile app on your phone.`, then repeated `Waiting for confirmation...`, then `Wait for confirmation timed out.` / `Timed out waiting for confirmation.` / `ERROR (Timeout)` | sourced (one real transcript) | [CubeCoders forum transcript](https://discourse.cubecoders.com/t/how-to-provide-steam-2fa-information-from-steams-phone-app-solved/42461) |
| Password prompt is the bare word `password:` with no newline | sourced for the text (same transcript); "no trailing newline" is **assumed** from the task brief and how console prompts work | CubeCoders transcript |
| Failure reasons in the line `Logging in user ... FAILED (<reason>)`: `Invalid Password`, `Two-factor code mismatch`, `Invalid Login Auth Code`, `Account Logon Denied`, `Rate Limit Exceeded`, `No subscription` | sourced | Renode guide, [LinuxGSM error list](https://docs.linuxgsm.com/steamcmd/errors) (No subscription) |
| Progress lines: `Update state (0x61) downloading, progress: 1.11 (127644881 / 11530459441)` with states 0x3 reconfiguring, 0x5 validating, 0x11 preallocating, 0x61 downloading, 0x101 committing | sourced | LinuxGSM error list |
| `Success! App '<id>' fully installed.` | sourced | [CubeCoders Arma 3 thread](https://discourse.cubecoders.com/t/arma-3-steamcmd-update-fails-with-useractionrequired-but-no-steam-login-prompt-appears/43339) (search excerpt) |
| `Error! App '<id>' state is 0x202 after update job.`, `ERROR! Failed to install app '<id>' (No subscription)` | sourced | LinuxGSM error list |
| Progress is redrawn with carriage returns | assumed (task brief; the parser treats CR, LF and CRLF alike, so it works either way) | |
| `+login <user>` without a password asks for it on stdin; with a cached login it signs in silently | assumed from Valve's documented `login <username> [<password>]` and from the sources above showing the `password:` prompt | |
| `+@NoPromptForPassword 1` makes a missing cached login fail instead of asking | assumed from the setting's name and its use in the wild; **the exact failure text is unknown**, so the job code also treats *any* password or code prompt in a job as "sign in again" and never answers it | |
| `+@ShutdownOnFailedCommand 1` ends the run when a command fails | assumed from the setting's name and common use | |
| `+@sSteamCmdForcePlatformType windows` | assumed (the downloaded game is the Windows build; on Windows this is also the default) | |
| Download URL `https://steamcdn-a.akamaihd.net/client/installer/steamcmd.zip` | the long-standing official URL the Valve wiki lists; **not re-fetched here** (a download needs the person's explicit yes, see below) | |
| Valve publishes no checksum for that zip | stated by the task and consistent with everything I could read; the confirmation says so | |

If a real SteamCMD prints different wording, only the lists in `OutputScanner::classifyTail` and `classifyLine`
(`steamcmd.cpp`) change; the state machine and the tests around them stay.

## Credential handling

The password and the Steam Guard code:

1. Are typed on the XMB keyboard in **masked** mode; `View::oskClose` now overwrites the keyboard's own buffers after a
   masked field is accepted (the Jellyfin password, API keys and these all benefit).
2. Reach the shell as a `QString` argument of the field callback, are turned into a `steamcmd::Secret` (a byte buffer that
   overwrites itself in `wipe()` and the destructor, and is move-only) at once, and are never stored in a `QString` member.
3. Are written **once, to the child's stdin**, by `Run::provide`, which then zeroes the secret and the temporary copy it
   made for the write. A secret is only accepted for a prompt SteamCMD actually asked; a password with no pending prompt is
   wiped and refused.
4. Are never on a command line (`signInArguments` / `jobArguments` have no slot for them; the account name is checked by
   `validAccountName` so it cannot become another command), never in an environment variable (`Run` adds none), never in a
   file or the settings (only the account **name** is stored), never in a log line (the shell logs `prompt: password`,
   `sent`, character counts), never in the trace.
5. While SteamCMD runs, every line it prints is scrubbed of the held secret (`<hidden>`) before it leaves `Run`, in case a
   child echoes input. The held copy is wiped when the run ends.
6. Are held for the shortest time that works: the password sits in the shell's `Secret` from "Done" in the keyboard until
   the `password:` prompt arrives (a moment), then it is moved into `provide`. A failed or cancelled sign-in wipes it.

What I cannot control: SteamCMD's own handling of what it reads, Windows pipe buffers, a debugger or crash dump taken
while a secret is in memory for that moment, and the keyboard's `QString` copies made by Qt's implicit sharing before
`oskClose` overwrites its own (the callback's argument is the same buffer when untrimmed, and is overwritten right
after the callback returns). The test cannot prove memory is clean; it proves nothing reaches a file, argument,
environment variable or log.

**After a successful login** the XMB relies on SteamCMD's **own** cached login. Installs run
`+login <name>` with no password and with `+@NoPromptForPassword 1`. If SteamCMD asks anyway (or reports no cached
login) the job stops with "SteamCMD's saved sign-in is missing or has expired", the account name is cleared, and the
sign-in page is the next step; the failed job is queued again after a successful sign-in.

**Sign Out of SteamCMD** forgets the account name and deletes SteamCMD's own `config\config.vdf` in SteamCMD's folder
after a Yes/No box that names the file. That file is where SteamCMD keeps its own login cache (assumed, see below); it is
recreated on the next run. It is refused (the name is still forgotten, a sentence explains) when the folder also holds
`steam.exe`, so the Steam client's files are never touched. The Steam client's login, registry and files are never read or
changed by anything here, and `looksLikeSteamClientFolder` also keeps such a folder out of "Look for SteamCMD".

## Installing into a library the client recognises

For a title that is not installed the XMB chooses `<library>/steamapps/common/<safe folder name>` (the library from
`libraryfolders.vdf`, default the first; the name is the title with Windows-illegal characters removed) and passes it as
`+force_install_dir`. After a successful job `integrateManifest` looks for `steamapps/appmanifest_<id>.acf` that
SteamCMD wrote inside that folder; **if** its `installdir` equals the folder name, and the library has no manifest for
the app yet, it copies the file to `<library>/steamapps/` (written beside, then renamed; never overwriting). The Steam
client reads manifests from the library when it starts or rescans. **I could not verify** where a real SteamCMD puts
the manifest for a forced install directory; reports I know of say inside the forced folder's own `steamapps`, which is
why the copy step exists. If there is no manifest, or its `installdir` names another folder, the row says so in
SteamCMD's terms and nothing is moved; the files are still in the library's `common` folder and the client's "Locate
Local Content" (or a restart) can adopt them. For an installed title the forced folder is its existing install folder
and the library's manifest is left alone.

An *update* without `validate` into an existing folder may behave like a validate, because SteamCMD does not read the
library's manifest when the forced folder has none of its own. That is why **Update** and **Validate** are both offered
but both go through `app_update`; whether Update is cheaper than Validate is unverified.

## Downloading SteamCMD

Never silent. **Download SteamCMD** shows a Yes/No box naming the exact source URL and the destination
(`<XMB data>\steamcmd`), and states that Valve publishes no checksum. On Yes: the zip is downloaded (https only,
redirects only to https, 8 MB cap, 30 s transfer timeout); `zipHoldsSteamcmd` checks it is a zip, that its directory has
`steamcmd.exe` at the top level and that **no** entry names a drive, an absolute path or `..` (the whole file is
refused otherwise); it is saved, unpacked with Windows' own `tar.exe`, deleted, and `steamcmd.exe +quit` is run once so
SteamCMD updates itself (no account involved). Then the path is saved. Safe mode refuses all network use.

## Tests

`xmb-steamcmd-tests` (+ `xmb-steamcmd-standin`), wired into `native-xmb/CMakeLists.txt` and `.github/workflows/tests.yml`:

- prompt matching on **partial buffers** (`pass` ... `word: ` over two reads, no newline), CR/CRLF/LF line splitting, ANSI
  sequences, NUL bytes, a UTF-8 character split across two reads;
- the line classifier and the progress parser against the exact strings in the table above;
- the command-line builders (`+force_install_dir` before `+login`, no password slot, account/app id validation);
- zip verification (valid, missing steamcmd.exe, nested, `..`, drive and absolute entries, HTML bot page, truncated);
- folder search never offering a folder with `steam.exe`; library parsing; manifest integration (linked, already known,
  mismatch, none);
- the `Run` state machine against the stand-in: password only, wrong password, email guard (right and wrong code, and a
  code SteamCMD asks again), app code, **phone approval** (waiting state, no prompt), phone timeout, rate limit, a child
  that echoes the password, cancel at a prompt, jobs (progress, success, no subscription, 0x202, stale cached login,
  a job that asks for a password anyway, rate limit, cancel of a download that never ends, a missing exe);
- the **credential audit**: the stand-in records its own arguments, its entire environment and what arrived on stdin
  (length and verdict, not the value); the runner asserts the secrets are in none of the arguments or environment
  variables, that they did arrive on stdin, that none of the secrets is in any `qDebug`/`qInfo` line produced, and that
  no file under the scratch folder contains them.
- a **prompt that never arrives** (the pipe-flush gap below): the stand-in's `buffered_user` writes `password:` with
  no flush, so it stays in the child's own buffer; the `Run` state machine ends `TimedOut` on its prompt timeout.
- a **cancel just before that timeout**: `cancel()` stops the prompt timer, so a sign-in cancelled while waiting for a
  prompt it never got still ends `Cancelled`, never `TimedOut` (the stand-in ignores terminate, so only the 1.5 s kill
  timer would otherwise lose the race to the prompt timeout).

The stand-in never contacts Steam, never reads the registry and is addressed only by path. Runs use 30 s waits.

## The pipe-flush gap (reproduced, bounded)

One timing gap in the "unverified" list is reproducible without a real SteamCMD, and is now bounded. When a
program's stdout is a **pipe** — which is how `Run` reads SteamCMD (`QProcess`, merged channels) — the C runtime
switches stdout from line-buffered to **fully buffered** (4 KB). A prompt written with no trailing newline and no
explicit flush, which is exactly what `password:` looks like, then sits in the child's own buffer and never reaches
the reader until the buffer fills or the process exits. The stand-in's `buffered_user` reproduces this; against it
the `Run` state machine previously waited forever in "Signing In...".

`Run` now starts a prompt timeout for a sign-in (`setPromptTimeout`, default 60 s, generous because a first run
updates SteamCMD itself before it can prompt). If SteamCMD has not asked for the password or a code in that time the
run ends `TimedOut` and the sign-in page says it may still be starting up, or that its prompt may be held in its own
buffer instead of reaching this program's pipe. The timeout is **first-prompt-only**: it stops on the first prompt, so
the phone-approval wait (no further prompt) is not cut short, and `cancel()` also stops it. Test #13 drives
`buffered_user` with an explicit 2 s timeout and asserts the `TimedOut` outcome, that no prompt was delivered, that the
process is gone, and the "pipe" message; test #14 cancels `buffered_user` 0.6 s into a 1 s timeout and asserts
`Cancelled`, not `TimedOut`.

This does **not** verify SteamCMD's real behaviour: whether a real SteamCMD flushes its prompt, or prompts on a pipe
at all rather than a console, is still the owner's to test against a real account. What is now guaranteed is that a
sign-in whose prompt never arrives can no longer hang the XMB forever.

## Not done, and why

- **Uninstall**: no SteamCMD command for it that I could confirm, so Delete stays the `steam://uninstall` hand-off (the
  client asks to confirm).
- **Choosing the library per install** other than through the Install Location row.
- A running SteamCMD process ends with XMB. Restarting the saved job is explicit; byte-level download resumption
  depends on SteamCMD's on-disk state.

## Unverified (the owner tests these)

Everything against a real SteamCMD and a real account: that `+login <name>` without a password prompts on a **pipe**
(SteamCMD may read a console rather than stdin; if so the sign-in cannot be driven this way and the fallback is a hidden
pseudo-console, not built), that its prompt and output are flushed to a pipe in time (the buffering side of this is now
reproduced by the stand-in and bounded by a prompt timeout, see above), the exact texts in the table's
"assumed" rows, how the cached login behaves with `+@NoPromptForPassword 1`, that `config\config.vdf` is what holds the
cached login, where the manifest lands for a forced directory, that the Steam client picks the game up after a restart or
rescan, that `+@sSteamCmdForcePlatformType windows` is accepted, and the download URL and the unpack/first-run steps of
Download SteamCMD (no network access was made to Valve in development).

## Headless Steam implementations

- [WindowsGSM SteamCMD.ConPTY](https://github.com/WindowsGSM/SteamCMD.ConPTY) runs SteamCMD under a Windows
  pseudoconsole for live output. If SteamCMD's real login prompts require a console, this is the closest existing
  implementation to compare with XMB's pipe-based runner. Microsoft documents the process and handle lifecycle in
  [Creating a pseudoconsole session](https://learn.microsoft.com/en-us/windows/console/creating-a-pseudoconsole-session).
  A local native prototype stalled at the stand-in's first password prompt. It reached the outer stdout while the
  XMB parser waited for it. Keep the pipe transport until a pseudoconsole can pass the same prompt, cancellation
  and credential-redaction checks.
- [steamie](https://github.com/LargeModGames/steamie) launches with the installed Steam client and starts it
  minimized using `-silent -applaunch`. This is close to XMB's existing Windows background mode for playing games.
- [Aurelia](https://github.com/Drackrath/Aurelia) exposes a JSON CLI, a session daemon and an install backlog that can
  resume. Its Windows launch path can use the installed Steam client. [DepotDownloader](https://github.com/SteamRE/DepotDownloader)
  is a narrower reference for downloading depots and choosing authentication methods.
- [Steam Headless](https://steamheadless.com/docs/) and [Wolf](https://games-on-whales.github.io/wolf/stable/apps/steam.html)
  run a graphical Steam client with a virtual display and stream games from Linux. They are references for a future
  streaming integration. The current Windows XMB already runs games locally through the installed client.
