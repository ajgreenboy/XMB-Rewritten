# PS menu hitch: the system menu's laggy first frames

Symptom: opening the PS-button system menu (XMB on screen, no game) stuttered for its first frames. Measured with
`XMB_FRAME_TRACE=1` (`src/frame_trace.h`) through `bun work/ps-menu-hitch.ts --exe <exe>` at 3840x2160, live clock,
three opens. `XMB_FRAME_TRACE_MS=<n>` moves the 20 ms slow-frame line.

## Causes (all first-use work landing on the open frame)

| Frame | Cost | What |
|---|---|---|
| open | 42-49 ms | RCO name index over 122 containers, built on the draw path by `bestImage()` for the first small picture (button glyph) |
| open + 1 | 20 ms | 63 glyph tiles rasterised and uploaded for the menu's rows, clock and panel |
| ramp, ~50 frames | 9-10 ms each | the bloom's mask and three box blurs rebuilt on every frame of the 500 ms ramp |

## Fix

- `Assets::prewarmNameIndex()` builds the index on a worker thread; `ensureNameIndex()` joins it.
- `View::prewarmOverlays()` runs one stage per quiet frame (XMB shown, no intro, 800 ms without input, nothing open,
  150 ms apart): name index, then the menu's text and textures, then each row's bloom, then the on-screen keyboard's
  glyphs and the settings choice model. Scratch draws go to the UI layer's own target on a frame that does not rebuild
  it, so nothing reaches the screen. A resize re-warms the glyphs (the atlas is dropped when the layer scale changes).
  Skipped under a pinned capture clock so the gate's frames are untouched.
- `drawTextBloom` caches the blurred mask per label/size/scale and the glow image per strength.
- `drawPsMenuBody` is the menu's old inline draw, split out so the prewarm runs the same code.

## Numbers (same PC, 3840x2160, 3 opens; before = `XMB_OVERLAY_PREWARM=0 XMB_PS_BLOOM_CACHE=0`)

| | frames over 8 ms after first open | worst CPU frame |
|---|---|---|
| before | 69-70 | 46-49 ms |
| after | 0 | under 8 ms |

The prewarm itself costs 20-50 ms spread over a few frames, seconds after start-up and only when idle.

## Re-measuring

`bun work/ps-menu-hitch.ts --exe <exe> [--surface ps|ps-f10|options|message|osk|choice] [--slow-ms 8] [--assert]`.
Frame times belong to the machine: compare runs on one PC. `XMB_OVERLAY_PREWARM=0` and `XMB_PS_BLOOM_CACHE=0`
give the old behaviour.
