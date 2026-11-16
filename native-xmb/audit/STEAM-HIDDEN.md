# Steam background mode: the full Steam client, hidden behind the XMB

Created 2026-11-16. Branch `wt/steamhide`. Code: `src/steam_hidden.h`, `src/steam_hidden.cpp` (switch set, readiness state
machine, client starter, window rules), the glue in `src/shell.cpp` (`ensureSteamClient`, `armSteamSweep`,
`sweepSteamWindows`, `steamReturnFocus`, `startSteamClientInBackground`), the Game Settings row in
`src/settings_menu.cpp`, tests in `tests/steam_hide_tests.cpp` and `tests/steam_hide_standin.cpp`.

**Status: implemented and tested against a stand-in `steam.exe` only. Nothing here has run against a real Steam client.**
Every statement about what the real client does is marked verified or unverified below; the owner will test on the HTPC.

## What it is for

The XMB is a living-room front end. The owner does not want to see Steam's own window on the TV, but games still need the
full client: Steamworks, cloud saves, achievements, Steam Input (controller mapping) and friends data all live in it.
Background mode therefore runs the **full client**, started to the tray, and keeps what it puts on screen off the TV.
It does not replace Steam with a headless tool and it never signs in for the user.

## 1. Start-up switches: sources, effects, drawbacks

Sources that were read for this note (2026-11-16):

- S1 Valve Developer Community, *Command line options (Steam)*, `https://developer.valvesoftware.com/wiki/Command_line_options_(Steam)`.
  A direct fetch was **refused (HTTP 403)** from this machine, so its rows are known only through search-engine excerpts
  of that page and its mirror `.../wiki/CHILLMODEA/Pages/Command_line_options_(Steam)`. What those excerpts said: `-silent`
  is "Silent startup mode (tray mode only)", `-nofriendsui` "Prevents the friends list window from showing automatically
  upon start", `-noverifyfiles` stops the client checking file integrity, and `-no-browser` is in the page's deprecated
  list ("disables all instances of CEF", deprecated as of January 2023). The wiki is community maintained.
- S2 xPaw, gist of Steam client switches, `https://gist.github.com/xPaw/fe7d275d31da14d70481` (fetched): `-silent`
  "Starts Steam in silent mode, tray only"; `-tenfoot` "Starts Steam in Big Picture Mode"; `-nobigpicture` "Starts Steam in
  regular mode and turns Big Picture mode off"; `-nofriendsui` listed without a description; many `-cef-*` switches
  (`-cef-disable-gpu`, `-cef-force-gpu`, `-cef-disable-sandbox`, `-cef-enable-debugging`).
- S3 Steam man page text, `https://git.gnous.eu/Alnotz/bazar/raw/branch/master/Man/steam.6` (fetched; it describes the
  older Linux client): `-silent` suppresses the dialog opened at start, used when Steam auto-starts at boot; `-no-browser`
  "unconditionally disables CEF", which turns off the CEF-based components such as the in-game overlay browser and the
  new Library UI; `-nofriendsui` "Prevents the friends list window from showing automatically upon start"; `-noverifyfiles`
  stops the integrity check (useful when testing localisation); `-tenfoot` starts Big Picture Mode.
- S4 Steam Client Beta group, a June 2023 thread on the Friends list opening at start,
  `https://steamcommunity.com/groups/SteamClientBeta/discussions/3/6717729343873717559` (fetched): users discuss
  `-nofriendsui`, `-nochatui`, `-silent` and `-vgui` as workarounds; one report says the pair "seems to not work anymore"
  on the new chat UI, another that `-nofriendsui` worked for a few hours and then the list returned. A Steam Client Beta
  post saying Valve is ending support for `-noreactlogin` and `-no-browser` was seen only as a search excerpt
  (`https://steamcommunity.com/groups/SteamClientBeta/discussions/3/3710433479207750727`), not fetched.
- S5 This repository's own read-only probe of the installed client binaries, `STEAM-CLI.md` (section 1): `SteamUI.dll`
  carries the strings `Silent startup mode (tray mode only)` and `Start in regular mode (force Big Picture mode off)`; the
  client running on the development PC at that time had been started as `steam.exe -silent`.

| Switch | What it does | Persists? | Drawbacks and notes | Used |
|---|---|---|---|---|
| `-silent` | Starts the client to the system tray without the main window (S1, S2, S3, S5). | No: a per-start argument. Steam keeps its own "start minimised" style settings separately; the XMB writes none. | The client still runs its full CEF UI (steamwebhelper); it is hidden, not light. A sign-in window can still appear when the client is signed out (that is why rule 5 below exists). It only applies to a start that creates the client; given to a second `steam.exe` that forwards to a running client it has no effect (S5 for forwarding). Unverified: whether a first-run update dialog still shows. | **Yes** |
| `-nofriendsui` | No friends list window on start (S1, S3, S4). | No. | Reports say it stopped working for some users after the 2023 chat redesign (S4); harmless when ignored. Unverified on the HTPC's build. | **Yes** |
| `-nochatui` | No chat window on start (S4 only; not in S1-S3, S5). | No. | Community-reported, undocumented; the 2023 thread says it may no longer work. An unknown switch is ignored by Steam (unverified, but `STEAM-CLI.md` shows unknown `+`/`-` tokens only produce a log line). | **Yes** |
| `-no-browser` | Disables CEF entirely (S3); deprecated, Valve ending support (S1, S4). | No. | Removes the overlay browser and the new UI; Steam Input's configurator and the overlay need CEF, so this would break exactly what background mode keeps. | **Never** |
| `-nobigpicture` | Forces the regular UI, Big Picture off (S2, S5). | No (per start). | Would stop Big Picture appearing on start when Steam was last left in it, which is attractive, but it also forces a mode change on a client the user may use; with `-silent` there is no window to be in either mode. Left out to stay minimal; the window rules hide Big Picture if it does appear. | No |
| `-tenfoot` / `-bigpicture` | Starts Big Picture Mode (S2, S3). | No. | The opposite of the goal: a full-screen window on the TV. | Never |
| `-noverifyfiles` | Skips the client's file-integrity check at start (S1, S3). | No. | Not a window switch. Skipping the check hides a corrupt-client problem; does not help the goal. | No |
| `-cef-disable-gpu` | Disables GPU use in the embedded Chromium (S2). | No. | Helps some GPUs that crash the web helper, costs performance and does not hide anything; a workaround, not a hiding switch. | No |
| `-login <user> <pass>` | Starts with credentials. | n/a | The XMB never handles credentials or passes `-login`. | Never |
| `-applaunch <id>` / `-shutdown` | Launch an app / ask the client to exit through a running client (S5). | n/a | Already used by the XMB; unchanged. | (existing) |

**Chosen set: `-silent -nofriendsui -nochatui`**, in that order (`steamhide::startArguments(true)`; exactly this list is pinned
by the unit tests, and the tests also pin that none of the rejected switches is ever in it). It is the smallest set that
the sources say keeps the main window, friends list and chat from appearing at start while leaving CEF, the overlay, Steam
Input, achievements and cloud sync untouched. With the setting off or the kill switch engaged the start has **no
arguments** (what it always was).

Not verified (all of it): that this exact set keeps every window away on the HTPC's client build (11.x at the last probe);
whether `-nochatui` is understood; whether a pending client update shows its own window despite `-silent`; that no Steam
Guard or sign-in window appears for a remembered sign-in. These are why section 3 exists.

## 2. What the XMB does

### Setting and kill switch

- **Game Settings > Steam Background Mode** (On/Off, default **On**; shared preference `steam/background`, native-only row
  after the firmware's Game rows). It has an effect only when Steam is installed (`steam.exe` found in the library scan's
  root).
- **`XMB_STEAM_HIDE=0`** turns the whole mode off for a run, whatever the setting says (start arguments, waiting, window
  sweeping all disabled; the old launch path runs). Any other value, or unset, leaves the setting in charge.
- It is also off, with the reason logged (`steam hide: not used (...)`), when `XMB_STEAM_FAKE=1` (the Quit Game test seam),
  when Steam is not installed, and when the XMB itself runs with an elevated token: Steam would inherit that token, which
  breaks its integration with normal programs, so the XMB refuses to start it from an administrator process and says so.

### When the client is started

Always with the section 1 arguments, always `QProcess::startDetached`-style (a `QProcess` with a create-process modifier,
`CREATE_NO_WINDOW` and a minimised not-activated show hint; Steam is a GUI program so a console never appears either way),
working directory = Steam's own folder, and never when already registered.

1. **Before a game launch.** `Shell::startSteamTracked` first calls `ensureSteamClient`. If Steam's registry says a client
   is running and signed in, the launch goes ahead at once. Otherwise the XMB starts the hidden client, shows
   **"Starting Steam..."** as a message page titled with the game, waits, and then launches `steam.exe -applaunch <id>` as
   before. A second press while it waits is ignored (logged).
2. **At XMB start in kiosk mode** (`--kiosk`): after the library scan has found Steam, `Shell::startSteamClientInBackground`
   brings the hidden client up so the first game does not wait. Its status ("Starting Steam...") is a quiet status line, and
   a failure is a notification. Nothing happens in a plain (non-kiosk) run, the scheduled-task path included.
3. **After the account switch.** The switch's restart (`SteamAccountSwitcher`, `SystemProcessControl::launch`) now passes the
   same arguments; see `STEAM-ACCOUNT-SWITCH.md`. The registry values and files it writes are unchanged.
4. **After Quit Steam.** The XMB has no Quit Steam row, only Quit Game, which never touches the client
   (`STEAM-QUIT.md`). If the user closes Steam by hand, the next game launch starts it hidden again by rule 1. Not built:
   a row that quits the client.

### Readiness (reuses Steam's own registry record)

`HKCU\Software\Valve\Steam\ActiveProcess` holds two values the client keeps current: `pid` (its process id) and
`ActiveUser` (the signed-in account id; 0 while no one is signed in). The XMB already read `ActiveUser`
(`steamActiveUserId`, `steam_accounts.cpp`); `pid` is the same key, read with the same test seam
(`XMB_STEAM_REGISTRY_KEY`, accepted only under `HKEY_CURRENT_USER\Software\XmbTest\`, never falling back to the real key).
A pid is trusted only if that process exists and its image is `steam.exe` (a pid left by a client that exited, or reused
by another program, is not "running"). `steamhide::evaluate` (pure, unit-tested) maps (client state, elapsed time) to:

| State | Condition | On screen |
|---|---|---|
| Ready | pid alive and `ActiveUser` != 0 | nothing (the launch proceeds) |
| Starting | no live client for < 90 s, or a live client signed out for < 20 s | "Starting Steam..." |
| SignInNeeded | live client, `ActiveUser` 0, >= 20 s (1.5 s when the client was already up before the XMB asked) | "Steam needs you to sign in. Its sign-in window is on screen; the game starts once you are signed in." |
| Failed | no live client after 90 s ("Steam did not start: no Steam client registered itself within 90 seconds."), or still signed out after 10 min | the text of the real state |

A refused start has its own text: "Steam was not started: steam.exe was not found." / "...the XMB is running as
administrator...". `XMB_STEAM_HIDE_TIMEOUT_MS` (500 to 600000) shortens the 90 s for scripted runs. When SignInNeeded ends
(the user signs in) the launch that was waiting carries on by itself.

## 3. Window rules (stray Steam windows)

While the XMB has just started the client, launched a game, or restarted Steam after a switch, a sweep runs every 400 ms and
applies these rules to **top-level windows of the Steam client only** (the registered `steam.exe` and the `steamwebhelper.exe`
/ `steam.exe` processes under it). `classifyWindow` is pure and unit-tested; the order matters:

1. **Not the client, never touched.** Any other process (the game, the XMB, `gameoverlayui*.exe`, anything not `steam.exe`
   or `steamwebhelper.exe`) is ignored. The in-game overlay therefore keeps working.
2. **Not visible, owned (a child dialog), or a tool window: ignored.**
3. **Untitled windows: ignored** (Chromium plumbing and overlay surfaces).
4. **A title that looks like sign-in or verification** (sign in, sign-in, log in, login, Steam Guard, verification,
   security code, two-factor, password...): **kept**, and brought to the front.
5. **Signed out** (`ActiveUser` = 0): **nothing is hidden, whatever the title**, because the client is showing its login
   window and the title may be in any language. The window is brought to the front, and the XMB says
   **"Steam needs you to sign in"** (notification) and in the start wait above. A hidden login window is a dead end.
6. **A title that looks like an error or warning** (error, warning, failed, unable, cannot, required, not enough, disk space,
   corrupt, locked...): **kept** and brought to the front with "Steam is showing a message".
7. **Everything else** a signed-in client puts up (main window "Steam", Big Picture, "Steam - News", update and friends
   windows): **hidden** with `SW_HIDE`.

After a window is hidden, focus goes back: to the game's own window when the title is already running, otherwise to the XMB
(`Shell::foregroundRequested`, wired to the existing `bringWindowForward`). Sign-in and message windows are raised at most
once per 15 s so the XMB does not fight the user for focus.

The sweep is time-limited so it never fights the user at the overlay: armed for 10 s after the client becomes ready, 15 s
after an account switch, until 10 s after the game is seen running (at most 120 s) after a launch, and up to 10 min while
waiting for a sign-in. Outside those periods Steam's windows are the user's (Guide button overlay, Steam's tray menu, and so
on). The setting off, or `XMB_STEAM_HIDE=0`, disables the sweep as well.

## 4. Kiosk start scripts

`run-kiosk.ps1` and `watchdog.ps1` are **not** changed. Doing it from the app is cleaner: the app knows the setting, the
kill switch, the elevation rule and the on-screen status, and the watchdog restarts the app, which then re-checks the
registry and starts nothing when a client is already running. Nothing on the HTPC was touched.

## 5. Test seams

| Seam | Effect |
|---|---|
| `XMB_STEAM_HIDE=0` | Whole mode off for a run (also the user's kill switch). |
| `XMB_STEAM_HIDE_TIMEOUT_MS` | The 90 s "client never registered" timeout, 500..600000. |
| `XMB_STEAM_ROOT` | Steam folder (existing); a scratch folder with a stand-in `steam.exe` stands for Steam. |
| `XMB_STEAM_REGISTRY_KEY` | Registry key standing in for `HKCU\Software\Valve\Steam`; only under `HKEY_CURRENT_USER\Software\XmbTest\`, any other non-empty value is replaced by a dead scratch key. `steamAppState` (Running flags) now honours it too (`steam_library.cpp` carries its own copy of the same check because that file does not link `steam_accounts.cpp`), so a scripted run can show a stand-in game running without touching the real key. |
| stand-in `steam.exe` | `tests/steam_hide_standin.cpp`: refuses to run unless `XMB_STEAM_REGISTRY_KEY` is a test key; records its command line to `XMB_STANDIN_RECORD`; writes `ActiveProcess\pid` / `ActiveUser` after `XMB_STANDIN_READY_MS` (late sign-in with `XMB_STANDIN_SIGNIN_MS`); opens the windows named in `XMB_STANDIN_WINDOWS` (`|` separated); a second copy given `-applaunch <id>` while a client is registered acts as Steam's forwarder and sets the test key's `Apps\<id>\Running` for `XMB_STANDIN_GAME_MS`. Never shipped. |

## 6. Verification (2026-11-16, development PC, the real Steam registry and files not read or written by tests)

- `xmb-steam-hide-tests` (`-DXMB_STEAM_HIDE_BUILD_TESTS=ON`, in `tests.yml`): the exact switch set and the absence of every
  rejected switch; the kill switch (`0`, `1`, empty, unset, setting off); the readiness state machine across ready, starting,
  failure, stale `ActiveUser`, sign-in needed and sign-in give-up; the window classification table (hide: main window, Big
  Picture, News, friends, update, helper-process window; keep: sign-in, Steam Guard, login, security code, every window of a
  signed-out client in two languages, error and disk-space windows; ignore: a game, the XMB, the overlay process, hidden,
  owned, tool and untitled windows). Live, against the stand-in: hidden start records exactly `-silent -nofriendsui
  -nochatui`; a second start while registered launches nothing; background mode off records no switches; a late sign-in
  reports the sign-in state and then becomes ready; a client that never signs in fails with "nobody signed in"; a client
  that never registers fails with "did not start"; a missing `steam.exe` and an elevated token refuse before anything starts;
  real windows of the stand-in client: "Steam" and "Steam - News" are hidden, "Sign in to Steam" and "Steam - Error" stay
  visible, a pid map that does not contain the stand-in changes nothing, a signed-out client has nothing hidden and its main
  window is the one to raise.
- A scripted XMB run against a scratch Steam root (stand-in `steam.exe`, test registry key): Enter on the game row, Enter
  to skip the game-boot animation, then `steam hide: started steam.exe with [-silent -nofriendsui -nochatui]`, "Starting
  Steam...", `client ready after 1994 ms`, `launch: steam -applaunch 208650`, `steam hide: hid 2 Steam window(s)`,
  `steam: Stand In Game is running`, `returned from Stand In Game`; the record file shows exactly two `steam.exe` starts, the
  hidden one and the forwarded `-applaunch 208650`.
- Gate: the new Steam Background Mode row is on the Game Settings page, but no pinned gate state or layer shows that page,
  so no pinned frame changed and no reference was saved: 28/28 named states and 6/6 layers passed on the unchanged
  references (the gate points `XMB_STEAM_ROOT` at an empty folder, so background mode is inactive there as well).

## 7. Unverified, all of it against a real Steam

- Everything in section 1 about the real client: the three switches' effect on the current client build; that no window
  flashes before the sweep hides it (the sweep runs every 400 ms, so a window can be visible for up to that long, and the
  start hint is only a request).
- That `SW_HIDE` on the real main window is accepted by the client without it re-showing itself, and that hiding Big Picture
  (a full-screen window) leaves Steam Input working. (The tray icon's own "open" remains the user's way back.)
- The window titles and the sign-in / message keyword lists on the real client and in other languages (rule 5 covers the
  signed-out case without titles).
- Window classes and titles of the real Steam windows were not enumerated here (no Steam may be run on this PC). The
  rules use titles and process names only, never classes.
- That `ActiveProcess\pid` is exactly the client's pid on the HTPC's client and is cleared or stale after it exits (a stale
  value is handled by the image check, a cleared one by `pid` = 0).
- Steam's own start-up behaviours the XMB cannot see: an update on start, a first-time EULA or Steam Guard prompt, "Steam
  Input" first-run dialogs.
- Focus return over a real exclusive-full-screen game, as with Quit Game.
- Kiosk start timing on the HTPC and the interaction with the watchdog restart.

**Incident, 2026-11-16, during this work:** a `taskkill /IM steam.exe` meant to remove a stand-in's leftover process also
ended the **real Steam client that was running on this development PC** (a user Steam that had been started with
`-silent`, pid 17556). No Steam file or registry value was written by it. Steam was not restarted by this work; start it
again from its tray/Start menu entry. The cleanup now filters on the stand-in's own path.

## What to try on the HTPC

1. Game Settings > Steam Background Mode is On. Close Steam from its tray icon (Exit Steam).
2. Pick a game. Expect "Starting Steam..." on the XMB, no Steam window on the TV, then the game starts. `XMB_LOG` shows
   `steam hide: started steam.exe with [-silent -nofriendsui -nochatui]`, `client ready`, `launch: steam -applaunch ...`,
   and `steam hide: hid N Steam window(s): ...` if anything appeared (the titles it lists are the real window titles:
   please send them).
3. Sign Steam out on purpose, then launch a game: expect "Steam needs you to sign in" and Steam's login window in front, not
   hidden.
4. `XMB_STEAM_HIDE=0` (or the setting Off) and launch again: Steam starts and shows itself as before.
5. In kiosk mode, restart the XMB: `steam hide: kiosk start: bringing up the hidden Steam client` in the log and the client
   in the tray before the first launch.
