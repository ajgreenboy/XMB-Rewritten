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

Never read, written or logged: passwords, web tokens, cookies, `ssfn*` files, `ConnectCache`, the credentials in
`config.vdf`, anything under `userdata`. Steam is never started with `-login`, a user name, a password or a token. The
log carries file paths, the registry key and value names, and the account's login name; no secret.

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

- `tests/steam_switch_tests.cpp` (target `xmb-steam-switch-tests`, `-DXMB_STEAM_SWITCH_BUILD_TESTS=ON`, in CI): 143
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
