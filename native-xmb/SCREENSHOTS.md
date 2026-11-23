# Native application captures

## 2026-11-23: root README gallery (v0.3.29)

The ten images in `screenshots/readme/` illustrate the repository root `README.md`. They replace the earlier gallery
(`v0.3.21/`, which showed account-level views from a personal machine). Regenerate them with
`bun work/readme-shots.ts --exe <re-ps3-native.exe> --publish native-xmb/screenshots/readme`.

- **Build.** `re-ps3-native.exe` 0.3.29 (source revision e574c03), built with `native-xmb/build.ps1`.
- **Method.** The app's own framebuffer grab (`--script "...,grab=<file>.png,quit"`, one token per 300 ms), 1920x1080, with
  `--render-clock 30 --background-clock 2026-01-15T14:00:00 --ui-clock 2026-01-15T14:00:00 --focus-time 2000`, so the
  clock, background and glow are pinned and no real date or time is drawn. `--dev-flash` and `--vsh-modules` point at the
  local firmware, which is why firmware UI textures appear. After capture each PNG is recompressed by
  `work/png-optimise.ts` (a 256-colour adaptive palette with error-diffusion dither, about 600 KB each); nothing else is edited.
- **Privacy.** No friends, account or persona names, avatars, e-mail addresses, IPs, host names or user paths appear. The Friends,
  Users and Steam account views are not captured.

| File | Shows | Source of the content |
|---|---|---|
| `01-game-column.png` | Game column: PS3-style logo tiles, the focused row's "PS3" badge and date line, the game backdrop | The machine's real Steam library, read from the Steam files on disk (`XMB_STEAM_ARTWORK=0`, so nothing is fetched). Steam is never started. |
| `02-settings.png` | Settings column | Firmware UI only |
| `03-jellyfin-video-player.png` | Player control matrix with Chapters | `work/jellyfin-standin.ts` on loopback, serving a generated test-pattern clip (a stand-in server, not a real Jellyfin) |
| `04-jellyfin-music-player.png` | Music page with synced lyrics | The same stand-in server; invented artists and tracks |
| `05-photo-gallery.png` | Photo Gallery piles | `work/gallery-fixture.ts` (374 generated colour-field images) |
| `06-on-screen-keyboard.png` | OSK with predictions for "hi" | `osk=Search|,type=hi`; predictions from the firmware's English dictionary |
| `07-psn-sign-in.png` | PSN Sign In page (no PSN connection, nothing typed) | Firmware UI only |
| `08-pending-installers-panel.png` | Installing panel | `native-xmb/tests/installer_standin.cpp`, a synthetic installer, through the real Install (show installer) flow |
| `09-notification-toast.png` | A notification toast | `NOTIFY=Download complete|Sample Title is ready to play` |
| `10-cold-boot-wordmark.png` | Cold-boot wordmark stage | `XMB_INTRO_GRAB` at the frame nearest intro clock 1.2 s, `XMB_BOOT_CLOCK` pinned |

The Jellyfin and installer captures start from stand-ins, so they show that the XMB drives those flows, not a run against a real
Jellyfin server or a real repack installer.

## 2026-11-21: earlier root README gallery (removed)

The v0.3.21 gallery (`screenshots/v0.3.21/`) was removed on 2026-11-23.

## Earlier captures

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
