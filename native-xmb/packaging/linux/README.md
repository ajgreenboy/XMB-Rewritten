# Linux AppImage packaging

`build-appimage.sh` turns the Linux `re-ps3-native` build into a self-contained, distributable
**AppImage** that bundles the Qt 6.12 runtime (libraries, plugins and translations) plus the dependency
closure `linuxdeploy` resolves. The result launches from a clean desktop environment — no Qt install, no
`LD_LIBRARY_PATH`, no source tree.

**Firmware is never bundled.** The AppImage carries only the app and its runtime. `dev_flash` stays on the
target machine and is pointed at with `--dev-flash` (exactly like the Windows build), or chosen in the
first-run setup. No Sony `dev_flash`/PUP/PRX/SELF assets are downloaded or embedded.

This is a Linux-only addition. The Windows build, installer and renderer are unchanged; there are no new
CMake targets. The only code change is a portable default *data directory* on Linux (see below).

## Prerequisites (build machine)

- A built Linux tree: `native-xmb/build-linux/re-ps3-native` (plus `xmb-manager`, `xmb-updater`).
  Configure it against the same Qt the AppImage bundles:
  `cmake -S native-xmb -B native-xmb/build-linux -GNinja -DCMAKE_PREFIX_PATH=/opt/qt/6.12.0/gcc_64`
  then `ninja -C native-xmb/build-linux`.
- Qt 6.12.0 at `/opt/qt/6.12.0/gcc_64` (override with `QT_ROOT=…`). The script only needs its `qmake`
  and plugin/translation layout; the downloaded `linuxdeploy-plugin-qt` reads them from there.
- `curl`, `sha256sum`, `mktemp`. FUSE is **not** required to *build* (the tooling is extracted), but the
  produced AppImage mounts itself with FUSE when you run it; see "Running" below for the FUSE-free
  fallback.

## Build

```bash
native-xmb/packaging/linux/build-appimage.sh
```

Output:

```
out/appimage/XMB-Rewritten-x86_64.AppImage
```

Environment overrides: `QT_ROOT`, `BUILD_DIR`, `OUT_DIR`, `APP_VERSION`, `OUTPUT_NAME`.

## Pinned tooling

The three tools are downloaded from their official GitHub releases into the gitignored
`native-xmb/packaging/linux/tools/` directory, checksummed, and extracted (FUSE-free, deterministic). The
exact versions and recorded SHA-256 sums live at the top of `build-appimage.sh`:

| Tool | Version | SHA-256 |
|---|---|---|
| [linuxdeploy](https://github.com/linuxdeploy/linuxdeploy/releases) | `1-alpha-20251107-1` | `c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d` |
| [linuxdeploy-plugin-qt](https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases) | `1-alpha-20250213-1` | `15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724` |
| [appimagetool](https://github.com/AppImage/appimagetool/releases) | `1.9.1` | `ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0` |

These releases publish no standalone `.sha256` assets (linuxdeploy ships a `.zsync`, SHA-1), so the sums
above are recorded at pin time and re-verified on every run.

## Running

```bash
# point the app at your own RPCS3 dev_flash and run from a clean shell
XMB-Rewritten-x86_64.AppImage --dev-flash /path/to/rpcs3/dev_flash
```

The AppImage mounts itself via FUSE and runs `re-ps3-native`. On systems without FUSE (some containers,
restricted WSL configurations) use the extract-and-run mode instead:

```bash
XMB-Rewritten-x86_64.AppImage --appimage-extract-and-run --dev-flash /path/to/rpcs3/dev_flash
```

The bundled `AppRun` pins the bundled Qt first (`LD_LIBRARY_PATH`, `QT_PLUGIN_PATH`, `QT_QML2_IMPORT_PATH`
to `$APPDIR/usr/…`), so a stale `QT_*` environment or a system Qt never shadows it. `QT_QPA_PLATFORM` is
left alone: Qt auto-selects `xcb` (X11/XWayland). Override it yourself for headless/CI runs
(`QT_QPA_PLATFORM=offscreen …`). The AppImage bundles the `xcb` platform and its GL integration, so it runs
under X11 and XWayland; a pure-Wayland desktop with no XWayland would need the (unbundled) wayland plugin.

### dev_flash

There is **no firmware inside the AppImage**, matching the Windows convention. The XMB reads Sony's
`dev_flash` (icons, text, fonts, sounds) from a folder you supply:

- **`--dev-flash <dir>`** — the primary path (used above).
- The first-run setup (it runs automatically when no `dev_flash` is found) lets you pick the folder; its
  "save to `xmb-config.json` next to the program" writes beside the executable, which is read-only inside
  an AppImage, so **point `XMB_CONFIG` at a writable file** to persist that choice, e.g.
  `XMB_CONFIG="$HOME/.config/re-ps3/xmb-config.json"`. Without that, pass `--dev-flash` each launch.
- `xmb-config.json` accepts `devFlash` and `dataDir`; paths that are not absolute are resolved relative to
  the executable (so use absolute paths on Linux).

`dev_flash` is the folder that contains `vsh`, `data` and `sys` from your own PS3 or your RPCS3 install.
The app never downloads or includes it.

### Data directory

Interactive launches resolve writable data (profiles, preferences, cache, crashes, the optional firmware
download) as follows, in order:

1. `--data-dir <dir>` (explicit),
2. `xmb-config.json` → `dataDir`,
3. **default (Linux): `$XDG_DATA_HOME/re-ps3-native`, or `~/.local/share/re-ps3-native` when
   `XDG_DATA_HOME` is unset.** Windows keeps the beside-the-exe `data/` default unchanged.

This is the one C++ change this packaging addition makes (`native-xmb/src/main.cpp`), and it only takes
effect on Linux when neither `--data-dir` nor a config value is present — every test and the render gate
already pass an explicit `--data-dir`, so they are unaffected.

## Companion tools

`xmb-manager` and `xmb-updater` are bundled alongside `re-ps3-native` in `usr/bin` (they share the same
deployed Qt runtime, so this adds only their small binaries). They are not given their own desktop entry;
run them from the image via:

```bash
XMB-Rewritten-x86_64.AppImage --appimage-extract   # -> squashfs-root/
./squashfs-root/usr/bin/xmb-manager --help
./squashfs-root/usr/bin/xmb-updater --help
```

## Verification performed (WSLg, `DISPLAY=:0`)

The AppImage boots under WSLg on this branch. The smoke run

```bash
DISPLAY=:0 XMB-Rewritten-x86_64.AppImage --dev-flash /root/dev_flash --data-dir <tmp>
```

produced the expected boot evidence (llvmpipe software GL):

```
boot: QApplication ready after -1 ms
boot: window shown after -1 ms
boot: first frame after -1 ms
native renderer: "llvmpipe (LLVM 20.1.2, 256 bits)"
native frames: 6.02708 fps; decoded images 120 missing images 1
```

Both the FUSE-mounted run and `--appimage-extract-and-run` reach `boot: first frame`. The default data-dir
was confirmed to land under `$XDG_DATA_HOME/re-ps3-native` (files created there with no `--data-dir`).

## Still needs a real Linux desktop

- **FUSE mount vs extract-and-run** — both verified here, but a real desktop's AppImage launcher (e.g.
  AppImageLauncher/Go) and double-click flow are untested.
- **Real GPU** — verified only on llvmpipe (software GL). Hardware GL (NVIDIA/AMD/Mesa) is expected to work
  through the bundled `libGLX`/EGL dispatch but is unconfirmed.
- **Audio** — Qt Multimedia's FFmpeg backend and PulseAudio are bundled and the WSLg Pulse sink was picked
  up, but real speaker output and PipeWire-only desktops are unconfirmed.
- **Controllers** — the SDL2 backend is bundled, but no physical gamepad was attached here.
- **Icon/desktop integration** — the desktop entry and hicolor icon are correct on disk; they have not been
  exercised through a real `.desktop` launcher.
