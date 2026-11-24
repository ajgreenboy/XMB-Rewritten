# PS3 BD Remote Control (Sony 054C:0306)

Status: implemented and unit-tested against synthetic reports. **Not verified on a real remote**: no BD Remote was
available on the development machine. Every statement about the device's behaviour below is either a cited public
fact or marked INFERRED/UNVERIFIED.

## Problem

A paired BD Remote did nothing in the XMB. `pad.cpp` accepted only DualShock 3, DualShock 4 and DualSense product
ids, and Windows exposes the remote only as a generic HID device (no keyboard or gamepad mapping), so nothing else
translated its keys.

## Sources (facts only; no code was copied)

- Linux kernel `drivers/hid/hid-sony.c` (PS3 BD Remote support, `PS3REMOTE` quirk): the substitute report
  descriptor (12-byte layout, battery 0-5), `ps3remote_keymap_joypad_buttons` and `ps3remote_keymap_remote_buttons`.
  https://github.com/torvalds/linux/blob/master/drivers/hid/hid-sony.c
- Patch "[v4] HID: Add support for Sony PS3 BD Remote Control": remote keys in the fifth byte (collection 2), key
  table, pairing by holding Start+Enter for about 7 s. https://patchwork.kernel.org/patch/1506471/
- Fedora wiki "Features/PlayStationBDRemote": the remote's own descriptor dump (vendor page, report ids 1-6 with 11
  items each). https://fedoraproject.org/wiki/Features/PlayStationBDRemote
- Sony PS3 manual, Register BD Remote Control (registration steps; it names no passkey and no buttons to hold):
  https://manuals.playstation.net/document/en/ps3/current/settings/bdremotecontrol.html
- PS3 firmware key names (RCO/vsh text) were not consulted; key names below are the ones printed on the remote.

## Input report (as Windows returns it, report id in byte 0)

| Byte | Meaning |
|------|---------|
| 0 | report id `0x01` |
| 1-3 | 24 joypad-style bits, little-endian: bit n-1 = button usage n (table below) |
| 4 | remote key code; `0xFF` = no remote key held (release) |
| 5-9 | constant `0xFF` |
| 10 | press marker (not used; a key is "down" while byte 4 is not `0xFF`) |
| 11 | battery strength 0-5 |

Linux's descriptor has no report id, so its "byte 1 constant" is this id byte; the 12 bytes agree with the Fedora
dump (id plus 11 items). Reports with id 2-6 are not read. Anything shorter than 12 bytes or with another id is
rejected, and nothing past the end of a report is ever read.

Joypad usages: 1 Select, 2 L3, 3 R3, 4 Start, 5 Up, 6 Right, 7 Down, 8 Left, 9 L2, 10 R2, 11 L1, 12 R1,
13 Triangle, 14 Circle, 15 Cross, 16 Square, 17 PS, 20 Enter. The face-button names follow the kernel's key table
order (Option, Back, BTN_0, Screen) and the DualShock 3's button order; that identification is INFERRED.

## Key mapping

Pad buttons reach the shell through `pad::latest()` in XInput's `wButtons` layout, so the XMB, every panel and the
pad key-repeat (300 ms then 67 ms) treat the remote exactly like a controller. Media keys are one-shot commands
taken once per press by `pad::takeRemoteActions()` and sent to `View::remoteKeyAction`, the same function Windows
media keys use.

| Remote key | Code / usage | Result |
|---|---|---|
| Up / Down / Left / Right | usage 5/7/8/6 | D-pad (with repeat) |
| Enter | usage 20 | Cross |
| Return | code 0x0E | Circle |
| Triangle / Circle / Cross / Square | usage 13/14/15/16 | Triangle / Circle / Cross / Square |
| L1 / R1 / L3 / R3 | usage 11/12/2/3 | L1 / R1 / L3 / R3 |
| L2 / R2 | usage 9/10 | analog trigger 1.0 (L2/R2 seek 10 s in a seekable player) |
| Select / Start | usage 1/4 | Select / Start |
| PS | usage 17 | PS bit: PsWatcher (tap, hold = system menu), as the DualShock path |
| Play / Pause | 0x32 / 0x39 | play / pause the open player (declined with no media open) |
| Stop | 0x38 | stop (leaves the player as Circle does) |
| Prev / Next | 0x30 / 0x31 | previous / next item |
| Scan Back / Scan Forward | 0x33 / 0x34 | seek -10 s / +10 s (one step per press) |
| Display, Time | 0x70, 0x28 | toggle the player's display (as Select) |
| Pop Up/Menu | 0x40 | options / Triangle on the current surface |
| Subtitle | 0x63 | next subtitle track, then Off (video only) |
| Audio | 0x64 | next audio track (video with 2+ tracks) |
| Top Menu 0x1A, Angle 0x65, Step Back/Forward 0x60/0x61, Eject 0x16, Clear 0x0F, 0-9 (0x00-0x09), Blue/Red/Green/Yellow 0x80-0x83 | | ignored: the app has no such command |

An unlisted code is held like any key and does nothing; `0xFE` is not a release.

## Behaviour notes

- Windows reconnects re-enumerate: the watcher scans every 2 s and opens each supported HID interface; a reader
  thread ends when the device disappears and the next scan reopens it.
- The remote is opened **read-only** and shared with all readers and writers (no exclusive access). The controllers
  keep read+write. If Windows refuses access the log says so: `pad: BD Remote found but Windows refused read access
  (error N)`; a report of another id is logged once.
- Freshness: the remote reports only while a key is down. Held keys are valid for 500 ms (controllers: 300 ms), so
  a remote that repeats less often than that would not give held-arrow repeat. UNVERIFIED.
- Scripted runs: the live remote is gated like every pad (`pad::scriptOnly`); its media commands are discarded and
  counted in `ignoredEvents()`.
- Battery: bars 0-5 shown as 20 % steps (`--pad-report`); charging is reported as "unknown" because the report has
  none. The remote reports only while a key is down, so `--pad-report` needs a key pressed during its 2.5 s window;
  the JSON now also lists `present` (supported devices Windows enumerates, silent or not).

## Verification

- `tests/bd_remote_tests.cpp` (CMake option `XMB_BD_REMOTE_BUILD_TESTS`, `tests.yml` runner `bd-remote`): framing,
  every key code, every joypad bit and its byte/bit position, combined reports, battery, press detection.
- `--pad-check` has two BD cases (battery 3; truncated report rejected).
- Script token `BDREMOTE=<24 hex digits>` injects one report through the same parser, mapper and command dispatch
  (not through the HID reader or the live pad gate): for example `BDREMOTE=01200000ffffffffffff0105` is Right and
  `BDREMOTE=01000000ffffffffffff0005` is the release.

## To verify on a real remote

1. Pair it in Windows, run the XMB, press keys: do reports arrive (`pad: BD Remote over Bluetooth` in the log) or is
   there a read-access error?
2. Is the report really 12 bytes with id 0x01 on the interface Windows exposes (it also lists ids 2-6)?
3. Do the joypad-bit positions and the face-button order match the keys printed on the remote?
4. Does a held key repeat reports (and at what rate)? Is `0xFF` really sent on release?
5. Is the battery byte 0-5, and what do 5 and 0 look like?
6. Do the PS key tap/hold open the PS menu as with a DualShock?
