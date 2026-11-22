# Native application captures

## 2026-11-21 — root README gallery (real Steam titles)

The seven screenshots that illustrate the repository root `README.md` (`screenshots/v0.3.21/`) are real, unmodified
native renders, not AI generation and not mockups. They replace the earlier fixture gallery (`v0.3.1/`), which showed
fictional game titles.

- **Build.** `re-ps3-native.exe` version 0.3.21, built from `native-xmb/` with MSVC 19.44 and Qt 6.12 on 2026-11-21.
- **Method.** The application's own framebuffer capture. Each frame was produced by launching the app with
  `--dev-flash C:\dev\rpcs3\bin\dev_flash --vsh-modules C:\dev\xmb\fw493\vsh_module --data-dir <scratch>` and a
  scripted input sequence (`--script "full,wait,…,grab=<file>.png,quit"`, one token per 300 ms), so the grab is
  whatever the renderer actually drew at that moment — full screen, 1920x1080. No image was edited after capture.
- **"Real game assets".** The Game column reads Steam from the registry (`SteamPath`) plus
  `libraryfolders.vdf` / `appmanifest_*.acf`, so the ten titles at the top of the Game column are this machine's
  actually installed Steam library. Row artwork is resolved from Steam's local `appcache/librarycache` (real
  header/hero images); the detail lines ("Multiplayer", "Fighting", …) are Steam's own collection labels. Nothing was
  drawn or fetched to fake a title.
- **Verification.** Every grab writes a sidecar JSON (the app's own state dump) recording the category, the focused
  row, and each row's id, title, detail and artwork-readiness. All seven frames were checked against that sidecar:
  1920x1080, non-blank, the Game column listing the real titles with artwork ready, and the options-menu capture's
  log recording `shell: options opened … rows= 8 Sort By|Start|Open in Steam Library|Store Page|Screenshots|Verify
  Game Files|Delete|Information`.
- **Deliberately excluded.** The Friends column (a contact list), the Steam account list, and the Network column (its
  Tailscale row names the host and tailnet IP) were not captured, so no personal or identifying data appears.
- **Format.** The app writes PNG grabs; the repository copies are JPEG (quality 92) to keep the clone small. The
  lossless PNG grabs remain in the local (git-ignored) capture scratch, not in the repository.

October23,2026. Captures are unmodified application screenshots from Computer Use.

| Capture | Observed state |
|---|---|
| [Before decoding correction](screenshots/01-before-colour-fix.jpg) | Incorrect opaque cyan boxes from RGBA reversal |
| [Original Game icons](screenshots/02-original-icons-game.jpg) | RGBA transparency and DXT decoding corrected |
| [Original Settings icons](screenshots/03-original-icons-settings.jpg) | Settings textures decoded from local RCO/GIM |
| [Opaque theme icons](screenshots/04-opaque-theme-icons.jpg) | Green theme, tinted opaque category artwork, revised layout and procedural wave |
| [Original QGL Settings](screenshots/05-original-qgl-settings.jpg) | Original briefcase and embossed Settings shapes; first row-spacing revision |
| [Parent column](screenshots/06-theme-submenu-parent-column.jpg) | Theme submenu retains its parent; first row-spacing revision |
| [Restored selection](screenshots/07-submenu-selection-restored.jpg) | Color remains selected after returning from nested choices; first row-spacing revision |
| [Latest native Video](screenshots/08-original-qgl-video-final-spacing.jpg) | Original category normal maps, final recorded-spacing revision, persisted Green theme |
| [Corrected icon orientation](screenshots/09-icon-orientation-corrected.jpg) | October24: QGL maps normalized vertically; Game grips now point downward; Green theme preserved |
| [User-supplied RPCS3 reference](screenshots/reference-rpcs3-game.png) | Original VSH Game menu, gold theme,19:28 |
| [Current Game column](screenshots/10-game-column.png) | November 11: current build, default theme, fullscreen 1920x1080, Game column with corrected icon orientation |

The reference was attached by the user; it was not captured through automation.
The native catalog and background remain incomplete. Captures do not establish
full visual/functional parity, motion timing, audio or long-run stability.
