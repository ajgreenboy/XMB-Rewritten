# Controller mode (virtual Xbox 360 pad), phase 1

Spec: section 4.11 of the Steam workflow spec (R-C1..R-C4, R-C6, R-C8..R-C11) and decisions D5, D8 and D9. This file
records what the implementation does, where every piece comes from, and what has and has not been verified.

## What it does

While a Steam title launched from the XMB runs, the XMB exposes one virtual Xbox 360 controller whose state follows the
physical PlayStation controller, so Xbox-only games (for example Black Ops 3) see a pad. The PS button is never forwarded; it
stays with the XMB (tap or hold opens the in-game menu). Steam Input is not involved. While the in-game XMB is raised the game
is fed a neutral pad, so navigating the XMB never moves the game.

| Piece | File |
|---|---|
| Mapping, state machine, lifecycle, fake backend (pure) | `src/controller_mode.h` |
| Real ViGEm backend, driver query, installer check, backend factory | `src/controller_mode.cpp` |
| Shell wiring (launch, return, feed timer, driver action) | `src/controller_mode_shell.cpp`, hooks in `src/shell.cpp` |
| Per-game override | `controllerMode` in `steam-collections-local.json` (`SteamUserOverlay::controllerMode`) |
| Global setting | preference `steam/controllerMode` (bool, default on, machine-wide) |
| Settings rows | Game Settings: Controller Mode, and Install controller driver (only while the driver is missing) |
| Per-title row | the title's options menu: `Controller Mode: Default / On / Off` |
| Unit tests | `tests/controller_mode_tests.cpp` (`XMB_CONTROLLER_MODE_BUILD_TESTS`, wired in `tests.yml`) |
| Scripted e2e | `work/controller-mode-e2e.ts` |

Switches: `XMB_CONTROLLER_MODE=0` turns it off for a run. Test seams (never needed by a normal run):
`XMB_CONTROLLER_BACKEND=fake`, `XMB_CONTROLLER_FAKE_LOG=<file>`, `XMB_CONTROLLER_FAKE_FAIL=<reason>`,
`XMB_CONTROLLER_DRIVER=present|missing`.

## Mapping (R-C4, as implemented and unit-tested)

The pad reader already delivers the DualSense in XInput's `wButtons` layout, so the buttons map by an explicit allow-list
mask (`kForwardedButtons`); anything outside it, including the PS bit 0x400 (the Xbox Guide position), is dropped.

| DualSense | Virtual Xbox 360 |
|---|---|
| Cross / Circle / Square / Triangle | A 0x1000 / B 0x2000 / X 0x4000 / Y 0x8000 |
| L1 / R1 | LB 0x0100 / RB 0x0200 |
| Create / Options | Back 0x0020 / Start 0x0010 |
| L3 / R3 | 0x0040 / 0x0080 |
| D-pad hat (8 directions + released) | Up 1, Down 2, Left 4, Right 8 (diagonals set two bits) |
| L2 / R2 analog 0..255 | LT / RT 0..255 (rounded, clamped, NaN is 0) |
| Left and right stick X | -32768 .. 32767 (negative half x32768, positive half x32767, centre 0) |
| Left and right stick Y | inverted: the pad reports +down, Xbox is +up, so pad up (byte 0) is +32767 and pad down (byte 255) is -32768 |
| PS, touchpad click, Mute | not forwarded (the last two are not in the reader's state at all) |

The tests feed documented synthetic reports through `pad::parseReportState` (USB report 0x01 and Bluetooth report 0x31, one
extra byte in front) and check that both transports give the identical Xbox report.

## State machine and lifecycle

`Off / DriverMissing / Ready / Active / Error(reason)`. `Active` is only reported while the virtual pad exists. The pad is
created in `beginTitle()` before the game is started (`launchWithPad` makes that order one function), destroyed in
`endTitle()` when Steam reports the title stopped, the launch wait expires, a launch fails, or the XMB shuts down. Between
titles (the desktop, the XMB navigating) no pad exists whatever the setting says. A failed create (driver missing, access
denied, no slot) never blocks the launch: the mode becomes `Error` or `DriverMissing` with the reason and the game starts.
A refused update ends the pad (the game keeps running) and reports `Error`. Only Steam launches are covered in phase 1
(RPCS3 and imported games are not).

## Steam Input status (R-C6): a documented gap

The setting is "Steam Input for PlayStation controllers". Evidence gathered read-only on the dev PC (a machine with a
DualSense, Steam installed):

- `config\config.vdf`: no key naming controllers, Steam Input or PlayStation support (it holds only the unrelated
  `SDL_GamepadBind` mapping table for Steam's virtual pad).
- `userdata\<id>\config\localconfig.vdf`: only `ControllerTypesUsed` (`controller_ps5,`, which says a pad was *used*, not
  that Steam Input is on) and the Steam Deck configurator keys; no per-game controller key.
- `userdata\<id>\config\cloudstorage\*.json`: no controller keys.
- The Steam client's own UI bundle (`steamui\sp.js`) shows where the setting lives: a client setting `controller_ps_support`
  (int32, field 14003 of the client settings table, where 2 means enabled and 1 means "when needed"), plus a per-app
  `eEnableThirdPartyControllerConfiguration` field. Both are protobuf/client-settings values, not keys in a text file we
  can read, and none was present in the files above.

So the key cannot be established with evidence, and the row says **"Steam Input status unknown"** (never a guess). The
warning text for the "on" case exists and is unit-tested, but nothing can trigger it yet. Closing the gap needs either the
protobuf client-settings blob located and decoded on a machine with the setting changed (compare before and after), or a
spike on the HTPC (`S1`-style: toggle the setting in Steam, diff Steam's files).

## The real backend: ViGEmClient

`third_party/ViGEmClient/` vendors the MIT-licensed client library byte for byte (see its `README.md`: upstream
https://github.com/nefarius/ViGEmClient , tag `v1.21.222.0`, commit `9e842ba1c3a6efbb90d9b7e9346a55b1a3d10494`, zip
SHA-256 `2B421149ACC3C411D1E9E0E22DC225997D87913860C006F5FF257F6E98282326`; the upstream repository was archived on
2023-09-08; no local modifications). It is a static library (`ViGEmClient` target, `setupapi`, warnings off for that
target only) linked into the app on Windows with `XMB_HAVE_VIGEM`; Linux builds do not compile it and get an
"unavailable" backend that reports so truthfully. `ViGEmPad` maps `vigem_alloc / connect / target_x360_alloc /
target_add / target_x360_update / target_remove` to the `VirtualPad` interface, turns every `VIGEM_ERROR` into a
readable reason, and never throws. Rumble (phase 2) is not registered; the `VirtualPad` interface has the handler slot.

## The driver: ViGEmBus, bundled but not committed

ViGEmBus 1.22.0 (BSD-3-Clause) is the kernel driver. The 6 MB installer is **not in Git**. `packaging/vigem-driver.json` pins
file name, URL, size 6278576, SHA-256 `89220A7865076B342892F98865F3499FB7C4CFD673159E89D352C360FD014C6A` and the Authenticode
signer `Nefarius Software Solutions e.U.`. `packaging/fetch-vigem-driver.ps1` (called by `build-package.ps1`, which the
release workflow runs) downloads it, or takes it from a local cache (`-DriverCache` / `XMB_VIGEM_CACHE`; the dev cache is
`C:\dev\out\vigem-vendor`), then verifies size, SHA-256 and a valid Authenticode signature by the pinned signer. Any
mismatch deletes the file and fails the package. The zip carries it under `drivers\` and the Inno setup, which wraps the same
folder, carries it too.

- **Setup** (`re-ps3.iss`): an optional task "Install the controller driver (needed for Controller Mode)", offered and ticked
  only when the `ViGEmBus` service is missing; an existing ViGEmBus (the HTPC has 1.21.442) is never touched. The vendor
  installer runs through `runas`; the setup itself stays per user.
- **Silent install: not available.** The vendor's release page and README document no silent switch (the docs page tried
  for it does not exist), so none is guessed: the installer always runs interactively (UAC plus its own window), and a
  silent setup skips it.
- **In the app**: Game Settings > Install controller driver (shown only while the driver is missing) asks for confirmation
  naming the file and version, re-verifies size, SHA-256 and signer immediately before running, and starts only the installer
  elevated (`ShellExecute runas`); the XMB never runs elevated and never downloads at run time. A dev build without
  `drivers\` says so.
- The driver state shown on the row is a read-only service query (`OpenService("ViGEmBus")`), so it works without the
  client library.

Licences: `THIRD-PARTY.md` carries the MIT text (ViGEmClient) and the BSD-3-Clause text (ViGEmBus); packaging ships it as
`THIRD-PARTY-NOTICES.md`. Both upstream projects are archived, so there will be no upstream fixes; the pins are what is
shipped.

## Verification (what was and was not observed)

Verified here (dev PC, 2026-11-27):

- 159 unit checks pass (mapping of every button, axis extremes and centre, trigger scaling, hat, USB vs Bluetooth, state
  machine, lifecycle ordering with the fake backend, override resolution, overlay round trip, pin equals JSON, decoy
  installers rejected, the real vendor installer accepted by the C++ size/SHA-256/Authenticode check).
- `fetch-vigem-driver.ps1` accepts the real installer from the dev cache and rejects (and deletes) a same-size file with
  the wrong hash.
- The scripted e2e (`work/controller-mode-e2e.ts`) with the fake backend and the Steam stand-in: see the run results in the
  task report.

NOT verified (nothing has run against a real game):

- Creating a real virtual pad: ViGEmBus is not installed on the dev PC, and CI cannot load the driver. The real `ViGEmPad`
  is compiled and linked but untested against a driver here. It needs the HTPC test (spike C1/C2: an unelevated process creates
  the pad, an XInput reader sees it, Black Ops 3 uses it, the PS button stays with the XMB).
- That a game uses the virtual pad rather than the physical one (double input, spike C5 and decision D8).
- Rumble (phase 2), and Steam Input detection (the gap above).
- The Inno task and the interactive vendor installer (no Inno Setup run, no elevation here).
- The release workflow's download path (only the local-cache path was run).
