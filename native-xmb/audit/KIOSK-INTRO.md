# Kiosk cold-boot intro

`--kiosk` skips the 4.93 cold-boot intro by default (an appliance should be usable the moment the window appears).
Opt in with `"kioskIntro": true` in `xmb-config.json` next to the exe, or `kioskIntro=true` under `[boot]` in
`<data>\settings.ini`. Default off, so no pinned gate frame changes.

- Decision: `xmb::setup::introEnabled` (`src/setup.cpp`), tested in `tests/setup_tests.cpp`.
- `--no-intro` always wins; `--script`, `--background-clock`, `--measure-reference` and safe mode also suppress it.
- A kiosk start logs `boot: intro enabled|disabled (kiosk, kioskIntro=true|false)`.
- Config file only: no in-XMB toggle (the System Settings page rows are pinned by the gate).
