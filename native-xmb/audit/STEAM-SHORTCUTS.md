# Steam non-Steam shortcuts (owner decision D3, requirement R-L6)

The emulators and other programs the owner added to Steam with "Add a Non-Steam Game" show up in the Game column as
game rows, ordered with the installed Steam titles, launched through Steam, and labelled **Non-Steam**. They are
read from the signed-in account's own `userdata/<accountid>/config/shortcuts.vdf`.

Code: `src/steam_shortcuts.*` (reader, ids, artwork lookup), `src/steam_account_state.*` (the per-account snapshot),
`src/steam_account_shell.cpp` (reset and adopt on an account change), `src/shell.cpp` (rows, launch, tracking, info page).
Tests: `tests/steam_shortcuts_tests.cpp` (`xmb-steam-shortcuts-tests`, also in `.github/workflows/tests.yml`),
the snapshot checks in `tests/steam_account_tests.cpp`, and the real-app run `work/steam-accounts-e2e.ts`.

## What is read

`shortcuts.vdf` is **binary** KeyValues, not the text VDF `steam_vdf.*` reads:

| Byte | Meaning |
|---|---|
| `0x00` | nested map: NUL-terminated key, entries, `0x08` |
| `0x01` | string: key, NUL-terminated UTF-8 value |
| `0x02` | int32, little endian: key, 4 bytes |
| `0x08` | end of the current map |

Layout: `shortcuts` map, one map per shortcut keyed `"0"`, `"1"`, ... with `appid`, `appname`, `exe`, `StartDir`,
`icon`, `ShortcutPath`, `LaunchOptions`, `IsHidden`, `LastPlayTime`, a `tags` map, and so on. Keys are matched without case
(older files write `AppName` and `Exe`). The reader also steps over float, pointer, colour, uint64 and int64 values and
refuses any other type byte, because the length of an unknown type cannot be known.

Safety (the file is untrusted input; conventions follow `steam_vdf.h`): at most 16 MiB is read; every string must end
inside the file and is at most 64 KiB; nesting is at most 8 deep; at most 200000 entries and 4096 shortcuts; the first
structural error stops the read. **Partial results:** a shortcut is accepted only when its whole map was read, so a
truncated or damaged file yields the complete shortcuts before the damage plus an error string (logged as a warning),
never a half-read shortcut. (The text-VDF reader discards the whole document on error; for a list of independent
shortcuts keeping the intact ones is the more useful behaviour and is covered by the truncation test.) A missing file is
simply no shortcuts and no error.

Also dropped, and counted: a shortcut with neither a name nor an executable, one with `IsHidden = 1`, and a later
shortcut with the same app id as an earlier one (the first wins). Hidden shortcuts are left out because Steam's own
hidden titles are left out of the Game column (R-L5); the real HTPC file has not been inspected for this field.

## Ids and launch

The shortcut's 32-bit app id is the **stored** `appid` when the file has a non-zero one, otherwise it is **computed** as
`crc32(exe + appname) | 0x80000000`, where `exe` is the string exactly as stored (Steam keeps the surrounding double
quotes, and they are part of the checksum), `appname` follows it, and `crc32` is the standard zlib CRC-32. The stored id
wins when present.

The launch id is the 64-bit **game id** `(appId << 32) | 0x02000000`, written in decimal. `Shell::launchSteamApp` hands
Steam `steam://rungameid/<game id>` as the argument of `steam.exe` (a normal Steam title still uses `-applaunch <id>`).
The shortcut's own launch options are applied by Steam from the shortcut, so the XMB adds none. The Game column row
keeps the game id in `Entry::value` and `steam-shortcut:<32-bit id>` in `Entry::id`.

## Rows, ordering, counts

- One row per shortcut in the Game column, ordered with the installed Steam titles by last play (the shortcut's
  `LastPlayTime`; never-played ones keep file order after the installed titles).
- The line under the title says **Non-Steam**. The information page (Triangle > Information) says
  `Format: Non-Steam Game`, `Source: Non-Steam (a shortcut added to Steam)`, and lists last played, tags, launch options,
  path, start folder and the shortcut id.
- Never counted: not in `ownedGames_`, not in the Steam hub's "N owned" or "N from this PC's Steam data", not in the All
  Games folder, All Titles, Not Installed or a collection page. (The XMB's Favourites, Collections and Launch Options rows
  and the SteamCMD, store and uninstall rows are not offered for a shortcut: they would key Steam data on an id Steam does
  not know. Start and Information remain.)
- **Follows the signed-in account** like every other account list: they are part of `SteamAccountSnapshot`, cleared by
  `resetSteamAccountState()` and replaced as one unit with the play records and collections. Account A's shortcuts never
  show for account B (unit test and `work/steam-accounts-e2e.ts`, which also covers a switch inside the Steam client, a
  switch back, and a restart under `AutoLoginUser`).

## Artwork

From `userdata/<accountid>/config/grid`, by the 32-bit shortcut id (`<id>.png` wide capsule, `<id>p` portrait,
`<id>_hero`, `<id>_logo`, `<id>_icon`; png, jpg, jpeg, webp). The older 64-bit game-id file names are accepted too.
Row order of preference: transparent **logo** (the PS3 logo-tile look, as for Steam titles), wide, portrait, hero, grid
icon, then the shortcut's own `icon` path (decoded with Qt's image plugins; `.ico` needs the bundled `qico` plugin, which
the build ships). Nothing found leaves the default game tile. No artwork is ever downloaded for a shortcut.

## Launch tracking

`Shell::pollSteamGame` reads Steam's registry state for the **32-bit shortcut id** (`Apps\<id>\Running`, the key a normal
title uses). When the registry does not report the shortcut running, the shortcut counts as running while a process whose
image path **equals the shortcut's own executable** exists (case and slash direction ignored; a sibling program or the
folder never matches). The existing observation reducer (`steam_library.h`) then confirms, debounces and detects the
return exactly as for a Steam title.

**Quit Game** for a shortcut is not done by the XMB: a shortcut has no install folder to scope the stop to, and the quit
code only ever closes processes by folder. The PS menu answers with a message telling the owner to close it from its own
window. (A later step could stop exactly the shortcut's executable and its children.)

## Gate

`work/ab-layers.ts` reads the Game column dump. The `steam:*` wildcard in a recipe's `catalogueRowIds` now also accepts
`steam-shortcut:<id>` rows inside the run; the run must hold exactly the dump's `installedSteam` count of `steam:<id>` rows
and exactly its `steamShortcuts` count of shortcut rows. No frame reference changes for a PC without shortcuts.

## Not verified (to check on the real HTPC, T1/T15)

- That `steam.exe steam://rungameid/<(appId << 32) | 0x02000000>` starts the shortcut on the installed client (the form is
  the commonly documented one; no real Steam was started here, and the exact launch form was explicitly left to the
  HTPC run). The fallback to try is `steam.exe -applaunch <32-bit id>`.
- That Steam keeps `Apps\<32-bit id>\Running` for a shortcut, and whether `RunningAppID` uses the 32-bit or the 64-bit
  value. Without it the executable match is the only signal, and a shortcut that starts a launcher which hands over to a
  different executable is not recognised as running (the launch wait then ends with the "did not confirm" message).
- That the computed id formula matches this Steam build for a file with no stored `appid` (every modern file stores one;
  the formula is the documented one and is unit-tested only as arithmetic, not against a real file).
- The real file's `IsHidden` and `LastPlayTime` behaviour, and the real grid artwork names, were not inspected (no
  personal data was read or copied; every fixture is synthetic).
- The in-game XMB, Quit Game and artwork were exercised only by unit tests and the headless dump run, not on screen with
  a real shortcut.

## Environment switch

- XMB_STEAM_SHORTCUTS=0 leaves the non-Steam shortcuts out of the account snapshot. The render gate sets it for every capture so this PC's own shortcuts never reach a reference image; the e2e scripts that build a synthetic Steam root do not set it.
