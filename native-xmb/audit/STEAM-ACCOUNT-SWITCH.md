# Steam account switch (Users > Steam Account)

Status: implemented 2026-11-14 on `wt/steamsw`. Verified on the development PC against a scratch Steam root and a
scratch registry key, with a fake process control. **Not yet run against a real Steam client**: that is the one thing
the owner must try on the HTPC (last section).

Code: `src/steam_switch.h`, `src/steam_switch.cpp` (the switch), `src/steam_accounts.*` (account scan, prompt text,
dry run), `src/shell.cpp` `steamSwitchRun` and neighbours (the page), `tests/steam_switch_tests.cpp`.

## Why this changed

The first version only handed control back to Steam: `steam.exe -shutdown`, then `steam.exe` with no account argument,
on the rule that the XMB never alters Steam's account settings. Steam then signs back in to whichever account it
considers the auto-login account (`HKCU\Software\Valve\Steam\AutoLoginUser` plus `config\loginusers.vdf`), so choosing
another remembered account in the XMB did nothing. The owner reported this from the real HTPC and asked for the switch
to be automatic.

The earlier statement "the XMB does not change Steam's configuration" is therefore no longer true, and this note
replaces it with an exact list of what is written.

## What is written, and only this

An automatic switch happens only for an account whose `loginusers.vdf` block has `RememberPassword "1"`: Steam already
holds a saved sign-in for it. For any other account nothing is changed (see "Accounts without a saved sign-in").

| Where | What | Notes |
|---|---|---|
| `<steam>\config\loginusers.vdf` | the target account: `MostRecent "1"`, `AllowAutoLogin "1"`, and `AutoLogin "1"` when the file uses that key; every other account: a truthy `MostRecent` or `AutoLogin` becomes `"0"` | The edit is a splice into the original bytes: unknown keys, nested blocks, comments, quoting, indentation, CRLF or LF and a byte-order mark are kept. A missing key is inserted at the end of the block in the block's own style. `WantsOfflineMode` and `SkipOfflineModeWarning` are added as `"0"` to the target only when it lacks them and the other accounts have them; an existing value (including `WantsOfflineMode "1"`) is never changed. |
| `HKCU\Software\Valve\Steam` | `AutoLoginUser` = the account's login name (`AccountName`), `RememberPassword` = 1 as a `REG_DWORD` | The previous values (or their absence) are remembered in memory and restored on failure. |
| `<steam>\config\loginusers.vdf.xmb-backup-<yyyyMMdd-HHmmss>` | a byte-for-byte copy of the file as it was before the rewrite | Made before the first write. The five newest copies are kept; older ones with that exact name pattern are removed. |

A current Steam client writes `AutoLogin` (and a lowercase `timestamp`) instead of `MostRecent`/`AllowAutoLogin`; the
HTPC's own file is in that form. The rewrite handles both generations, which is why `AutoLogin` is kept exclusive to
the target when the file uses it.

Never read, written or logged **by the account switch**: passwords, web tokens, cookies, `ssfn*` files, `ConnectCache`,
the credentials in `config.vdf`, anything under `userdata`. Steam is never started with `-login`, a user name, a password
or a token. The log carries file paths, the registry key and value names, and the account's login name; no secret.

**What is now true about passwords in the XMB as a whole (SteamCMD, [STEAMCMD.md](STEAMCMD.md)).** The old sentence
"the XMB never handles a password" no longer holds for the whole application, by design, in exactly one place: the
**SteamCMD sign-in page** (Users > Steam Account > SteamCMD > Sign In) takes the Steam account name, the password and a
Steam Guard code on the XMB keyboard and writes them to the stdin of Valve's `steamcmd.exe`, a separate program that is
not the Steam client. That is the only place a Steam password or code enters the XMB; they are held in memory only from
the keyboard until SteamCMD has read them, then overwritten, and are never in a file, a setting, a log line, a command
line or an environment variable (tests assert this). The account switch described in this file is unchanged and still
reads and writes no credential; it never touches SteamCMD, and SteamCMD never touches the Steam client's login, registry
or files.

## Why this is safe

- Both values are the ones Steam itself writes when a user ticks "Remember my password" and signs in; the XMB changes
  *which* remembered account Steam picks, not what Steam stores for it. No credential is created or moved.
- Steam is fully gone before anything is touched (see the flow), so Steam cannot overwrite the file on exit after the
  XMB wrote it.
- The file is replaced atomically (`QSaveFile`: temp file in the same folder, then replace) after a backup exists, then
  read back and compared with what was written.
- Any failure after the first write (file, registry, read-back, starting Steam) restores the file from the in-memory
  original and the registry values, and the result page says whether the restore worked. The backup copy remains as a
  second source.
- An account that is not in the file, appears twice, has no `AccountName`, whose login name no longer matches, or has no
  saved sign-in is refused **before Steam is asked to close**.
- The rewrite is a pure function (`rewriteLoginUsersForSwitch`); running it on its own output changes nothing.

## Flow

1. **Page.** Users > Steam Account lists the remembered accounts. Selecting one opens the existing two-step box
   (Close Steam, then Start Steam). The wording now says what will happen: step 1 says Steam closes (close any game
   first) and starts again as the chosen account, signing in by itself with its saved sign-in; step 2 names the
   registry value, the file and that it is backed up first. An already-active account says so and nothing is started.
2. **Dry run.** After the second confirmation the shell logs the exact command lines and the exact writes
   (`steam: dry run command:` / `steam: dry run action:`), then runs. `XMB_STEAM_SWITCH_HOLD` stops after that log and
   changes nothing.
3. **Preflight.** The file is read and the rewrite is computed in memory. A refusal ends here with nothing changed and
   Steam untouched.
4. **Close.** If `steam.exe` or `steamwebhelper.exe` is running, `steam.exe -shutdown` is started and the processes are
   polled every 500 ms for up to 45 s. If they are still there the switch stops, changes nothing and says Steam did not
   close. If Steam is not running this step is skipped (no `-shutdown` is sent to a stopped client).
5. **Settle** 1.5 s, then **write**: backup, file, registry, with the read-back checks above.
6. **Start.** `steam.exe` with no arguments, detached.
7. **Verify.** Every 500 ms for up to 60 s the XMB reads `HKCU\...\Steam\ActiveProcess\ActiveUser` (and that Steam is
   running). When it names the account the page says *Signed in as <persona name>*. At the deadline it says one of: Steam
   signed in as another account (named); Steam is running but nobody is signed in (its own sign-in window: expired saved
   sign-in or a Steam Guard code is needed; sign in there once); Steam is not running.

All of it runs on timers; the UI never blocks. The status page rewrites itself while it is on screen; if the user
dismissed it, the result arrives as a notification instead.

### Accounts without a saved sign-in

Nothing is changed. Steam is closed and started again as before, and both prompts say, in the box and on the result
page, that this account must be signed in once inside Steam (tick Remember my password) and that the next switch can then
be automatic.

### Offline mode and Steam Guard

An account with `WantsOfflineMode "1"` is switched like any other and Steam starts it offline; step 1 says so. Steam
Guard, an expired saved sign-in or a revoked token show up as the "Steam's own sign-in window" result; the XMB never
enters a code.

## Test seams (never used by the real code path)

| Seam | Effect |
|---|---|
| `XMB_STEAM_ROOT` / `--steam-root` | Steam root (already existed). |
| `XMB_STEAM_REGISTRY_KEY` | Registry key that stands in for `HKCU\Software\Valve\Steam`. Accepted only when it begins with `HKEY_CURRENT_USER\Software\XmbTest\` (the full `HKEY_CURRENT_USER` spelling; `HKCU\...` is not accepted). **Any other non-empty value is replaced by `HKEY_CURRENT_USER\Software\XmbTest\RejectedOverride`, never by the real key.** |
| `XMB_STEAM_SWITCH_NOPROC=1` | Steam is never running, nothing is started or stopped; the start is logged and Steam counts as running afterwards so the verify step can be exercised. |
| `XMB_STEAM_SWITCH_VERIFY_MS` | Verify wait, 500 to 300000 ms (default 60000). |
| `XMB_STEAM_SWITCH_HOLD` | Log the plan, change nothing, start nothing (already existed). |

An early scratch run on 2026-11-14 passed the key as `HKCU\Software\XmbTest\...`, which the first version of the override
rejected by falling back to the **real** key, so that run wrote the real `AutoLoginUser` and `RememberPassword` on the
development PC. It was caught from the log (the logged key was the real one) and `AutoLoginUser` was set back to the
value the owner had reported. `RememberPassword` reads `1` (a DWORD), which is what Steam keeps for a remembered account,
but its value before that run was held only in memory and was not recovered, so "unchanged" is not claimed for it. The fallback was removed, a test pins it, and the scripted-run helper now compares the real key before and
after every run. No real Steam process or `loginusers.vdf` was touched.

## Verification

- `tests/steam_switch_tests.cpp` (target `xmb-steam-switch-tests`, `-DXMB_STEAM_SWITCH_BUILD_TESTS=ON`, in CI): 143 before the library refresh work, 178 now
  checks, 0 failures. Pure rewrite: exact output for two accounts, one account, thirty accounts; unknown keys, nested
  blocks, comments, trailing comments; CRLF stays CRLF; missing keys inserted in the block's style; compact one-line
  blocks; bare and quoted values; escapes; byte-order mark; offline keys; the current `AutoLogin`/`timestamp` format;
  refusals (no saved sign-in, unknown or duplicate account, no `AccountName`, truncated file, no users block); idempotence.
  Whole switch against a scratch root, a scratch registry key and a fake process control: steam running or not, steam never
  exiting (bounded), launch failure and read-only file (rollback, with and without previous registry values), login
  window, other account, not running afterwards, refusal before closing, hold, no saved sign-in, backup pruning, and
  `scanSteamAccounts` agreeing afterwards. Registry seam rejections.
- `xmb-steam-vdf-tests`: unchanged, 4908 fixture results, 0 failures.
- Scripted runs at 1920x1080 against a scratch root and key (`XMB_STEAM_SWITCH_NOPROC=1`): remembered account (new
  prompts, files and registry written, "Signed in as" result), account without a saved sign-in (prompts, nothing
  written, "sign in once" result), already-active account (blocked page), login-window and other-account results, hold
  (file byte-identical). The real `AutoLoginUser` was compared before and after each run.
- Gate: see the final report of the commit; no existing gate state reaches these pages (the gate points
  `XMB_STEAM_ROOT` at an empty folder, so there are no accounts), so no pinned frame changed and no reference was saved.

## Library refresh after a switch (2026-11-21)

Until this change `Shell::steamSwitchFinished` only re-scanned `steamAccounts_` and armed the window sweep, so after a
successful switch the Game column still showed the previous account: `steamId_`, the play records (`steamActivity_`,
which also drive the last-played order and the detail text), `ownedGames_`/`ownedSignature_`, `collections_`, the
family list and the per-user grid artwork all belonged to the old account until the XMB restarted.

| Outcome | Library |
|---|---|
| `SignedIn`, `OtherAccount` (Steam is signed in as someone) | refreshed for whoever Steam's record names |
| `LoginWindow` | not yet; refreshed later if Steam reports an ActiveUser (below) |
| `NotRunning`, `Started`, `Held`, `Failed` | unchanged (nothing the library depends on changed, or the user is still signing in) |

`Shell::steamLibraryAccountChanged(name)` clears `ownedGames_`, `ownedSignature_` and `collections_`, cancels any in-flight
owned request, then runs `refreshContent()`. That pass re-reads the manifests, the new account's `localconfig.vdf`
play records, family sharing and the new account's `userdata/<id>/config/grid` art, rebuilds the column, **keeps each
column's selection by row id where the row still exists**, and at its end calls `refreshOwnedGames()` (only when an API
key exists) with the new `steamId_`. A notification "Library refreshed for <persona>" follows the last pass (a queued
second rescan postpones it). The name is what Steam reported, else the active account's persona, else the prompt's label.

**Stale-response guard.** `SteamOwned::requestedId()` is the account its latest `refresh()` asked for; `cancel()` already
drops older requests, and the `loaded` handler also drops a response whose requested id differs from `steamId_`
(`ownedResponseIsStale`, src/steam_refresh.h), clears the in-flight flag and asks again for the current account, so an
owned list for the old account can neither replace the new one nor leave the in-flight flag stuck. An unknown id on
either side is never called stale.

**LoginWindow and changes made inside Steam.** The window sweep that already polls Steam's `ActiveProcessActiveUser`
is reused (no new timer): after a `LoginWindow` result it is armed for 10 minutes, and each tick feeds
`ActiveUserWatch`. The first observation only primes; a later change to a non-zero ActiveUser that differs from the
column's `steamId_` runs the same refresh. That also catches an account change made inside Steam, but **only while the
sweep is armed** (after a switch, a LoginWindow result, or the first 8 s / 120 s after a kiosk start) and **only when
Steam background mode is active** (the sweep does not run with `XMB_STEAM_HIDE=0`, an elevated XMB or the setting
off). There is deliberately no permanent watcher: a plain account change inside Steam long after those windows is not
noticed until the library is refreshed some other way (restart, Retry Artwork, a content refresh).

**Not refreshed (superseded 2026-11-23, see the last section).** Collections, the family list and a permanent watcher were added afterwards.

### Verification of the refresh

- `tests/steam_switch_tests.cpp`: +35 checks (178 in total, 0 failures): which outcomes refresh or watch, the stale-id
  rule, ActiveUser to SteamID64, the notice name, and the ActiveUserWatch sequence (prime, quiet, sign-out, change,
  switch back, reset, login-window then sign-in).
- `work/steam-switch-refresh-e2e.ts` (a focused script, not a gate state: it needs a scratch registry key and a stand-in
  that plays Steam): builds a synthetic Steam root (two accounts, three manifests, different play records and grid art per
  account), runs the real app with `XMB_STEAM_SWITCH_NOPROC=1`, a scratch key and `XMB_STEAM_HIDE=0`, opens Users > Steam
  Account, picks the other account, answers both prompts, lets the stand-in write ActiveUser, and compares the Game
  column before and after: order (Bravo, Alpha, Charlie to Charlie, Alpha, Bravo), detail text, grid artwork colour, kept
  selection by id, the log lines, and that the real `AutoLoginUser` is unchanged. 20 checks pass on this build; the same
  script against the 0.3.20 build fails the five after-switch checks (the bug).
- Unverified: a real Steam (the ActiveUser stand-in is a registry write by the script), the owned list and collections
  after a switch (no network or API key in the run; only the pure guard and the unchanged cancel/generation code are
  tested), and the LoginWindow-then-sign-in refresh and in-Steam change path (they need a live `steam.exe` pid; the pure
  watch is tested, the sweep wiring is not run).

## Not verified

A real `steam.exe` shutdown, restart and sign-in on a machine with two remembered accounts; whether the real client
honours `AutoLoginUser` and the `loginusers.vdf` flags as described when only these two are changed (this is the widely
used mechanism and matches the HTPC's own file, but it has not been run here); Steam Guard and offline paths against a
real client; behaviour with Steam installed under Program Files with restricted permissions (the failure path is tested,
the real ACLs are not).

## What to try on the HTPC

1. Make sure both accounts were signed in once with Remember my password (the file shows `RememberPassword "1"`).
2. Close any running game. Users > Steam Account > the account that is not active > Yes, Yes.
3. Expect: "Closing Steam", Steam exits, "Selecting the account", Steam starts, then "Signed in as <name>" within a
   minute. A copy `loginusers.vdf.xmb-backup-<time>` appears next to `loginusers.vdf`.
4. If it shows the sign-in window result instead, send `XMB_LOG` (the `steam: switch` lines) and the file listing of
   `config`; the backup copy restores the previous state by copying it over `loginusers.vdf` while Steam is closed.

## Start path with Steam background mode (2026-11-16)

Step 6 of the flow (start Steam) now passes `-silent -nofriendsui -nochatui` when Steam background mode is on
(`makeSystemSteamProcessControl(steamhide::startArguments(true))`, decided in `Shell::steamSwitchRun`); with the mode off,
the kill switch `XMB_STEAM_HIDE=0`, or an elevated XMB, the start has no arguments, exactly as before. No account
argument is ever passed (`-login` stays forbidden). Only the arguments changed: what is written to `loginusers.vdf` and
the registry, the backup, the rollback and the verify step are untouched, and the tests that use the fake process
control still pass. When the switch ends in any outcome in which Steam is running, the window sweep is armed for 15 s so a
main window the restarted client shows is hidden; a signed-out client (the "sign-in window" result) is never hidden and
its window is brought forward. See [STEAM-HIDDEN.md](STEAM-HIDDEN.md) for the switches, sources and rules. Unverified
against a real client, like the rest of the switch.

## Full account switch and the library without a key (2026-11-23)

Owner requests: (A) after a switch, whether made in the XMB or inside the Steam client, nothing account-specific may be
left over from the previous account; (B) with no Steam Web API key the Game column must show the signed-in account's own
library, not just the PC's installed games. This section supersedes the "Not refreshed" paragraph above (collections and
the family list are now handled) and the "no permanent watcher" remark (there is one now).

### Which account is "the signed-in account" (`scanSteam()`)

Order, first hit wins (`steamSelectAccountFromLoginUsers` for steps 2 to 5, tested in `tests/steam_account_tests.cpp`):

1. The registry's `HKCU\Software\Valve\Steam\ActiveProcess\ActiveUser`, only when `SteamPath` names this root (or the
   `XMB_STEAM_REGISTRY_KEY` scratch key is in use). `0` means nobody is signed in and falls through.
2. `MostRecent=1` in `config/loginusers.vdf` (older clients; newest timestamp among several).
3. The account whose `AccountName` equals the registry's `AutoLoginUser` (case-insensitive; same trust rule as step 1).
4. The newest `timestamp` / `Timestamp` (a tie goes to the higher SteamID64).
5. The only remembered account.

`AllowAutoLogin` and `AutoLogin` are no longer read. Current clients set `AllowAutoLogin=1` on every remembered account,
so the old rule treated all of them as "most recent" and picked the highest SteamID64, which was the wrong account on a
PC with two. Real data on the HTPC: neither account has `MostRecent` or `AllowAutoLogin`; both have `RememberPassword=1`
and a lower-case `Timestamp`; ActiveUser is set while Steam runs.

### Account-specific state inventory and how each part resets

Everything is gathered by one worker-thread read (`loadSteamAccountSnapshot`, `steam_account_state.h`) and adopted as one
unit by `Shell::applySteamAccountSnapshot`; an account change first runs `Shell::resetSteamAccountState`.

| State | Source | Reset / reload |
| --- | --- | --- |
| `steamId_`, persona (`steamPersona_`) | `scanSteam()`, loginusers.vdf `PersonaName` | replaced from the snapshot every rescan |
| Owned list `ownedGames_`, `ownedSignature_`, `ownedFromKeyless_` | Valve (key) or keyless library | cleared on a change; the keyless list (or Valve's, once it arrives) is adopted |
| In-flight owned request, `ownedInFlight_` | `SteamOwned` | `cancel()`; a response for another account is dropped (`ownedResponseIsStale`) |
| Collections `collections_` and the row labels | `userdata/<id>/config/cloudstorage` | cleared, re-read keyless from the snapshot (they never needed a key) |
| Play records `steamActivity_` and the installed-row order | `userdata/<id>/config/localconfig.vdf` | cleared, re-read; the column is re-sorted by the new account |
| Steam Families `family_` | localconfig.vdf `FamilyGroup` | cleared, re-read |
| Family-shared titles `familyGames_` | `<data>/family-shared-<accountid>.json`, else `<data>/family-shared.json` unless it carries an `"account"` naming another account | cleared, re-read scoped to the account |
| Artwork overrides | `userdata/<id>/config/grid` (looked up by `artworkFor` with the scan's account) | the rescan re-resolves them for the new id |
| Keyless library cache | per account id, validated by size and mtime of every input file | other accounts' entries are separate; stale entries cannot be served |
| Remembered-account list and the open Steam Account page | loginusers.vdf, registry | `steamAccounts_` replaced; an open page re-marks its rows (`refreshSteamAccountsPage`) |
| Open Steam folder, collection and hub pages | rows of the previous account | closed (`closeSteamLibraryPages`) with the options panel |
| Friends | not part of the shell (the headless `--steam-friends` command reads fresh each run) | nothing to reset |
| Account-independent, deliberately kept | store/CDN header and logo caches (keyed by app id), the PC's installed games (manifests belong to the PC, not an account), RAWG art | none |

### How a change is noticed

- The XMB's own switch: `steamSwitchFinished` calls `steamLibraryAccountChanged` (unchanged).
- A switch made inside the Steam client, or a sign-in finishing later: a 3 s registry poll (`Shell::pollSteamActiveUser`,
  one registry value, read-only, no kiosk or armed sweep needed; `XMB_STEAM_ACCOUNT_POLL=0` turns it off) feeds
  `steamPollAccountChange`, which refreshes once per account change (the account a refresh was started for is not
  restarted every poll). The old sweep path still works and now shares the same reset.
- A restart or any rescan that finds a different account than the one on screen: `applySteamAccountSnapshot` compares ids
  and resets before adopting.

### Library without a key (`steam_keyless.h`)

When `SteamOwned::apiKey()` is empty (or while Valve's list has not arrived after a change), the owned list is built from
the signed-in account's own local data only, off the UI thread, bounded to 5000 titles and cached per account id:

| Source | Format (observed 2026-11-23 on a real client) | Used for |
| --- | --- | --- |
| `userdata/<accountid>/config/localconfig.vdf` | KeyValues; `apps > <appid> > LastPlayed / Playtime` | apps with playtime or a last-played time; a stub with neither is noise and ignored |
| `userdata/<accountid>/config/cloudstorage/cloud-storage-namespace-1.json` | array of `[key, {key, value}]`; `user-collections.*` values are JSON strings with `added` app ids | static collection members; dynamic collections have no member list and add nothing |
| `userdata/<accountid>/config/librarycache/<appid>.json` | array of `[section, {version, data}]` (achievements, descriptions, ...); the file name is the app id, there is no title inside | "this app is in this account's library" |
| `appcache/appinfo.vdf` | binary, version 29 on the real client (27 and 28 also read); layout documented in `steam_appinfo.h`; `appinfo > common > name/type` | the authoritative title and type |
| `steamapps/appmanifest_*.acf` | installed titles | names an app the account's data already lists when appinfo has no entry; sets the `installed` flag. Never adds an app by itself |

Only type `Game` (any case; the real file spells it both `Game` and `game`) is kept; tools, redistributables, dedicated
servers, DLC, music and videos are dropped. **Demos are dropped too** (they are not purchases and Valve's own list is where
a real one would appear); say so if the owner wants them. An app that appinfo has no entry for is kept only when an
installed manifest names it. The row line shows no playtime or last-played date (owner direction); the order of installed
rows still follows the play records.

The source is stated where the library count is shown: the Game column's Steam row reads "N from this PC's Steam data"
instead of "N owned", and the hub's All Games row detail reads "From this PC's Steam data for <persona>: games you have
played or collected. Add a Steam Web API key in Settings for the full list." Valve's list replaces the keyless one as soon
as it loads, and wins whenever a key exists. `XMB_STEAM_ACTIVITY=0` (the deterministic-capture switch) turns the keyless
library off together with the play records.

### Verification

- `tests/steam_account_tests.cpp` (`xmb-steam-account-tests`, run in CI with a scratch directory argument): the selection rule
  incl. the HTPC case and the old AllowAutoLogin bug, registry-first order against a scratch registry key, hand-built
  v27/v28/v29 appinfo files and damaged variants, the keyless library of two synthetic accounts (no cross-account row,
  noise, tools, demos, installed fallback, cache hit and invalidation, the 5000 cap), the per-account snapshot and
  family-shared scoping, and the refresh decisions.
- `work/steam-accounts-e2e.ts`: real app, synthetic root with two accounts, no key (the child gets a scratch `APPDATA` and
  `XMB_API_KEY_DIR`): A's keyless list only; a registry-only ActiveUser change (the stand-in for a switch inside Steam)
  swaps owned list, collections, play-record ordering, family titles and persona with no stale row; switching back
  restores A; a restart with a signed-out client selects the `AutoLoginUser` account.
- `work/steam-switch-refresh-e2e.ts` still passes (its two play-time checks were already stale after the owner removed
  play time from the row line and now assert that it is absent).
- Checked read-only against a real Steam install: the appinfo reader parsed a 6.9 MB v29 file and named real titles, and
  the keyless scan produced a plausible library for the account with the large library. Not verified on the HTPC itself.
