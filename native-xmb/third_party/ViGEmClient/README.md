# ViGEmClient (vendored)

The user-mode client library of the ViGEm virtual gamepad bus. Controller mode (`src/controller_mode.cpp`, the
`ViGEmPad` backend) uses it to create the virtual Xbox 360 pad.

- Upstream: https://github.com/nefarius/ViGEmClient (archived by its owner 2023-09-08)
- Tag: `v1.21.222.0`
- Commit: `9e842ba1c3a6efbb90d9b7e9346a55b1a3d10494`
- Source zip SHA-256: `2B421149ACC3C411D1E9E0E22DC225997D87913860C006F5FF257F6E98282326`
- Licence: MIT (`LICENSE`)
- Local modifications: none. The files are byte-for-byte upstream.

Vendored files only: `include/ViGEm/*.h`, `include/ViGEm/km/BusShared.h`, `src/ViGEmClient.cpp`, `src/Internal.h`,
`src/resource.h`, `LICENSE`. Upstream's `.rc`, project and CI files are not copied (they only build a DLL).

It is built as a static library on Windows by `native-xmb/CMakeLists.txt` (target `ViGEmClient`, linked with `setupapi`,
warnings off for this target only) and never on Linux. The driver it talks to, ViGEmBus, is not part of this source tree:
see `native-xmb/packaging/vigem-driver.json`.
