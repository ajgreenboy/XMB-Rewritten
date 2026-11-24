# PS3 BD Remote Control (Sony 054C:0306; CECH-ZR1 and CECH-ZRC1)

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

## Models: CECH-ZR1 vs CECH-ZRC1

| | CECH-ZR1 (2008) | CECH-ZRC1 (2011 "Media/Blu-ray Disc Remote") |
|---|---|---|
| HID id | 054C:0306 (public: hid-sony, Fedora wiki) | **054C:0306, CAPTURED** (Windows product string "BD Remote Control", usage page 0x01 usage 0x05, 12-byte input report) |
| Bluetooth keys | the 12-byte report above | the same layout, CAPTURED; plus Instant Back / Instant Forward |
| Also has | | infrared keys for the TV (power, input, volume, channel, mute): they go to the TV and never reach the PC |
| Instant Back / Forward | none | jump back / forward 15 s (Sony's manual); key codes **0x76 / 0x75, CAPTURED** |

### Capture on a real CECH-ZRC1 (2026-11-23, HTPC, v0.3.32 capture log)

Every key pressed once in a fixed order; each press is one report and each release one `0xff` report (byte 10 is
1 on a press, 0 on the release; byte 11 read 5 throughout). Confirmed against the table and bit layout:

| Key | Joypad bits (bytes 1-3) | Key code (byte 4) |
|---|---|---|
| Instant Back / Instant Forward | none | 0x76 / 0x75 |
| Up / Right / Down / Left | 0x10 / 0x20 / 0x40 / 0x80 in byte 1 (usages 5-8) | 0x54 / 0x55 / 0x56 / 0x57 |
| Enter | 0x08 in byte 3 (usage 20) | 0x0b |
| Return | none | 0x0e |
| Triangle / Circle / Cross / Square | 0x10 / 0x20 / 0x40 / 0x80 in byte 2 (usages 13-16) | 0x5c / 0x5d / 0x5e / 0x5f |
| PS | 0x01 in byte 3 (usage 17) | 0x43 |
| Play, Pause, Stop, Prev, Next, Scan Back, Scan Forward | none | 0x32, 0x39, 0x38, 0x30, 0x31, 0x33, 0x34 |
| Display, Subtitle, Audio, Pop Up/Menu, Top Menu | none | 0x70, 0x63, 0x64, 0x40, 0x1a |

The key codes sent alongside a joypad bit (arrows, Enter, shapes, PS) are named `Key::ButtonCode`: the bit acts, the
code does nothing and is no longer logged as unknown. One transient report (`01 90 00 00 ff ...`) appeared between two
Up presses with two arrow bits and no code; it carries no new key and is handled as plain bits.

What is unknown for the ZRC1, and what the code does about it:

- **Product id.** A Sony (054C) HID interface is taken as a BD remote candidate when it is 054C:0306, or when its
  product string contains "BD Remote", "Remote Control", or "PLAYSTATION" together with "Remote" (case-insensitive).
  Known DualShock 3/4 and DualSense ids are never candidates. A candidate found by name is **trusted only for
  reports that have the BD remote's shape**: exactly 12 bytes, id 0x01, bytes 5-9 all 0xFF, byte 10 0 or 1, byte 11 0-5
  (`bd::looksLikeBdReport`); a candidate whose descriptor input length is not 12 is not read at all (unless the capture
  log is on). Each candidate is logged once: `pad: BD Remote candidate (...) vid=... pid=... usagePage=... usage=...
  inputReportLength=... product="..."`.
- **Report layout.** Assumed identical to the ZR1; the capture log shows if it is not.
- **Instant Back / Instant Forward codes.** Unknown. `bd::kInstantCodes` in `src/bd_remote.h` holds two empty slots
  (`std::nullopt`): until filled, those keys log as `unknown key code 0xNN` and do nothing. When filled they map to
  `bd::Action::JumpBack/JumpForward` -> `RemoteKey::JumpBack/JumpForward` -> a 15 s seek (`bd::kJumpMs`) through the
  same path as Scan Back/Forward (10 s), declined with no seekable media open.
- **Consumer-usage guess.** If the ZRC1 puts those keys on a separate HID collection (Windows lists each top-level
  collection as its own device, usage page 0x0C), the reader parses that collection with the HID parser and maps
  Consumer AC Back (0x0224) / AC Forward (0x0225) to jump back/forward. **This is a guess pending capture.** Scan
  Previous/Next (0xB6/0xB5) are in the table but disabled (Windows already turns them into the Next/Previous media
  keys, so mapping them would act twice). Windows may also turn AC Back into its own Browser-Back key (the XMB's Back).
  Windows may refuse read access to a keyboard-class collection; the log then says so.

## Capturing the ZRC1 (steps for the owner, on the HTPC)

1. Pair the remote in Windows (Settings > Bluetooth & devices > Add device > Bluetooth; for the older remote Sony's
   registration steps are in the Sources list). Check Windows shows it as connected before going on.
2. Switch the capture log on. Any one of these (the kiosk task starts the XMB through `watchdog.ps1`/`run-kiosk.ps1`
   with fixed arguments and the process environment of the task, so the config file is the easy route):
   - `xmb-config.json` next to `re-ps3-native.exe`: add `"bdRemoteLog": true` (an interactive/kiosk launch reads it;
     remove it afterwards), then restart the XMB; or
   - run the XMB by hand with `--bd-remote-log`; or
   - set `XMB_BD_REMOTE_LOG=1` in the environment of the process that starts the XMB.
3. Press **every key once**, with a second of pause between keys, including Instant Back and Instant Forward, and
   the ordinary ones (Play, Stop, d-pad, colours, numbers). Keys that do nothing in the log are infrared TV keys.
4. Read the log (`<data dir>\logs\xmb.log`, or the file named by `XMB_LOG`): `findstr /c:"bd-remote" xmb.log`. The lines:
   - `bd-remote: sony hid interface vid=054c pid=NNNN usagePage=... usage=... inputReportLength=N product="..." (BD remote candidate)`
     lists every Sony HID interface once (an unknown pid shows up here, marked a candidate or `(not a supported controller)`);
   - `bd-remote: report vid=054c pid=NNNN len=12 hex=01 .. ..` is every raw report, with `; unknown key code 0xNN`
     appended when the key byte (byte 4) is in no table entry. The Instant keys' codes are those `0xNN` values;
   - `bd-remote: consumer usage 0x0224 down -> jump-back` appears if they come as standard consumer usages.
5. Alternative without restarting the XMB: stop it, run `re-ps3-native.exe --pad-report out.json --data-dir <data>`
   and press keys during its 2.5 s window; `sonyInterfaces` and `bdRemoteCapture` in the JSON hold the same lines (hold
   a key a little, the remote reports only while a key is down).
6. Send back the `sony hid interface` lines and the report lines for Instant Back and Instant Forward (they carry only
   ids, the product string and key bytes; no device path or address). To enable the keys, put the two codes in
   `bd::kInstantCodes` (`src/bd_remote.h`) and extend the unit test.

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
| Instant Back / Instant Forward | 0x76 / 0x75 (captured on a CECH-ZRC1, `bd::kInstantCodes`) | seek -15 s / +15 s |
| Top Menu 0x1A, Angle 0x65, Step Back/Forward 0x60/0x61, Eject 0x16, Clear 0x0F, 0-9 (0x00-0x09), Blue/Red/Green/Yellow 0x80-0x83 | | ignored: the app has no such command |

An unlisted code is held like any key and does nothing; `0xFE` is not a release.

## Presses: quick taps, held keys, other pads (v0.3.34)

Owner report: "it doesn't pick up all my inputs". Root cause (code): the shell sampled the newest pad STATE each frame
(`pad::latest()`) and derived presses from 0->1 changes; the remote's release report overwrote its press report in the
single shared `newest` before the shell looked (a quick tap lost), and any streaming pad overwrote the remote's state
(`newest` is shared, newest report wins). Media keys were unaffected (their own queue). A second report: a held arrow
stopped repeating after about a second, because the state expired 500 ms after the last report and the remote sends no
repeat.

Design (`bd::PadLatch`, `src/bd_remote.h`; `pad.cpp`; `View::pollController`):
- The remote has its own state and never writes `newest`. `pad::latest()` ORs it in for PsWatcher, setup and the desktop
  pad (each press stays visible for `bd::kLingerMs` = 60 ms, so a 20 ms poller still sees down then up).
- The main shell does not read the remote through `latest()` (`latest(hid,false)`). It calls `pad::takeRemotePad()`:
  the held bits plus every 0->1 edge since the last tick. The edges go into `keyPressed_` (the keyboard-tap latch the
  edge detection already ORs in), the held bits join `buttons`, so a tap shorter than one poll is exactly one press,
  and the held-key repeat (300 ms, then 67 ms) runs from the held bits. PS stays PsWatcher's. L2/R2 (digital, seek)
  count a tap as a full pull for one poll.
- Roll-over artefact: a report with key byte 0xFF and non-zero bits (`01 90 00 00 ff ...`, captured between two Up
  presses; Up and Left bits, no key code) is ignored whole (`isRolloverArtefact`): no edge, no change of the held state.
  Choice: nothing in the layout lets a key be down without a code, and the Left bit was never pressed; treating it as
  held would repeat a phantom Left. If a real key ever turns out to report bits with 0xFF, the capture log shows it.
- Scripted runs: `BDREMOTE=<hex>` is unchanged (replay path, its own edge detection). New `BDLIVE=<hex>[+<hex>...]`
  feeds the live latch (through a second, script-only latch, since the physical remote stays gated), reports joined by
  `+` land in one tick. `work/bd-presses-e2e.ts` uses both. A streaming DualSense cannot be injected in a scripted run
  (every physical pad is gated); independence from `newest` is structural and unit-tested as an OR.

## Behaviour notes

- Windows reconnects re-enumerate: the watcher scans every 2 s and opens each supported HID interface; a reader
  thread ends when the device disappears and the next scan reopens it.
- The remote is opened **read-only** and shared with all readers and writers (no exclusive access). The controllers
  keep read+write. If Windows refuses access the log says so: `pad: BD Remote found but Windows refused read access
  (error N)`; a report of another id is logged once.
- Held keys: the remote reports once on key-down and once on key-up (CAPTURED), never while a key is held. A joypad bit is
  therefore held from its press report until its release report (code 0xFF, bit cleared), whatever the age of the last
  report; the 500 ms / 300 ms freshness window applies to the streaming pads only. Safety releases for a lost release
  report: the reader thread ends (handle closed, read failed, disconnect or sleep; the 2 s re-enumeration then opens it
  afresh) and a cap of 30 s since the last report (`bd::kMaxHeldMs`; far past any real hold, short enough that a key is
  never stuck). A hold of more than 30 s therefore stops repeating: a deliberate trade for never sticking.
- Scripted runs: the live remote is gated like every pad (`pad::scriptOnly`); its media commands are discarded and
  counted in `ignoredEvents()`.
- Battery: bars 0-5 shown as 20 % steps (`--pad-report`); charging is reported as "unknown" because the report has
  none. The remote reports only while a key is down, so `--pad-report` needs a key pressed during its 2.5 s window;
  the JSON now also lists `present` (supported devices Windows enumerates, silent or not).

## Verification

- `tests/bd_remote_tests.cpp` (CMake option `XMB_BD_REMOTE_BUILD_TESTS`, `tests.yml` runner `bd-remote`): framing,
  every key code, every joypad bit and its byte/bit position, combined reports, battery, press detection.
- The same file covers candidate matching (product strings, DualShock/DualSense never matched), report-shape detection,
  the capture-log lines (`unknown key code 0xNN`), the Instant slots (disabled by default, decode when filled) and the
  consumer-usage guess.
- `--pad-check` has two BD cases (battery 3; truncated report rejected).
- Script token `BDREMOTE=<24 hex digits>` injects one report through the same parser, mapper and command dispatch
  (not through the HID reader or the live pad gate): for example `BDREMOTE=01200000ffffffffffff0105` is Right and
  `BDREMOTE=01000000ffffffffffff0005` is the release.

## To verify on a real remote (the ZRC1: see "Capturing the ZRC1" first)

1. Pair it in Windows, run the XMB, press keys: do reports arrive (`pad: BD Remote over Bluetooth` in the log) or is
   there a read-access error?
2. Is the report really 12 bytes with id 0x01 on the interface Windows exposes (it also lists ids 2-6)?
3. Do the joypad-bit positions and the face-button order match the keys printed on the remote?
4. Does a held key repeat reports (and at what rate)? Is `0xFF` really sent on release?
5. Is the battery byte 0-5, and what do 5 and 0 look like?
6. Do the PS key tap/hold open the PS menu as with a DualShock?
