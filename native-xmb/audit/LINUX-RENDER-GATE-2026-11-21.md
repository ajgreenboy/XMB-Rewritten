# Linux render fidelity gate — validation

Date: 2026-11-21 · Branch: linux-port · Bun 1.4.5 · Qt 6.12.0 · llvmpipe (software GL)

## Result
The render fidelity gate (work/ab-layers.ts) runs end-to-end against the Linux binary and
produces real, deterministic dB/SSIM numbers. Linux llvmpipe matches the RPCS3 ground truth.

Per-layer canonical capture vs ab-rpcs3-root.png (1920x1080), 6/6 layers PASS:
frame 21.57 dB / 0.8705 SSIM (floor 18.71) · top background 23.55 · category icon row 23.55 ·
item label row 14.08 (informative) · clock text 11.47 · info bar 15.18.

The 3D renderer is pixel-exact on llvmpipe: raf-static-scene (offscreen, no text) scores
67.77 dB / 1.0000 SSIM — shaders/geometry/particles are byte-identical to the reference.

The only divergence from Windows is text glyph rasterization (Linux FreeType vs Windows),
which lowers text-bearing self-baseline states to ~22-35 dB (below the 55 dB near-exact floor)
but does not breach any RPCS3 fidelity budget.

## Setup (reproducibility)
- Bun installed via bun.sh install script; corpus copied from the Windows tree:
  out/native-reference, out/v2/music, out/v2/empty-steam, out/gal/photos.
- A Linux-only thresholds copy (out/linux/ab-layers.thresholds.linux.json) remaps capture.exe,
  capture.flash, vshModules, and the absolute Windows seed paths to /root/... The tracked
  thresholds file and the Windows reference corpus were NOT modified.
- Gate invokes the app via its own cross-platform grabFramebuffer() path
  (--script ... grab=<png>,quit) with pinned render/background/ui clocks and a scratch data-dir.

## State classification (28 states)
- Portable & reproducible (14): raf-static-scene, video-options-panel, continue-watching,
  continue-watching-empty, subtitle-cue, jellyfin-signin, steam-download-page,
  steam-store-sections, music-player-page, music-visual-player-panel, auto-off-guide,
  settings-autologon-info, settings-autologon-row, first-run-autologon-step.
- Portable but non-deterministic on llvmpipe (3): photo-gallery-piles, photo-gallery-grid
  (readdir/EXIF bucketing order vs NTFS), music-visual-player-on (idle timing at ~5 fps).
- Windows-specific (11): steam-game-root, steam-game-root-dpr3, steam-all-games,
  steam-not-installed, xmb-notification (reference embeds this PC's Steam root),
  xmb-volume-hud, ingame-xmb, ingame-enter-text, ingame-enter-text-keyboard (Windows Graphics
  Capture + stubgame), game-boot-logo, game-boot-fade-out.

No thresholds were weakened and no references were overwritten. The Linux-only thresholds copy
and seed remapping live under out/ (untracked); this note records the procedure.
