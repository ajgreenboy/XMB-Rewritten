# RAF-THEMES - animated (RAF) theme support, F6 remainder

Status 2026-11-25: **a dynamic theme's embedded RAF scene is evaluated through `vsmx_eval` and drawn per frame on `View`'s
OpenGL 3.3 context via `RafScenePlayer` (`XMB_RAF_ANIMATE=0` reverts to the `t=0` still pose; `XMB_RAF_THEMES=0` skips the
scene altogether).** On any decode/shader failure, the theme's still background (`bgimage` hd/sd) is shown cleanly.
`renderRafStill` (`--raf-render file.raf@out.png`, pinned by gate state `raf-static-scene`) remains deterministic at
`t=0` with script evaluation off, while `--raf-render file.raf@<seconds>@out.png` (`renderRafFrame`) renders an evaluated
frame at `t=<seconds>`. Air Paint (`01.p3t`) turned out to be a **static** theme (section 1.1), so it is unchanged. No
third-party dynamic `.p3t` exists on this machine, so animated playback is proven on synthetic VSMX fixtures (`xmb-raf-tests`)
and on `coldboot.raf` at multiple timestamps.

Evidence tags: TRACED (firmware module, by string or address), M (measured on a local file this pass), INFERRED,
UNPROVABLE (cannot be settled with what is on this machine).

No firmware or theme content is shipped. Every reader/decoder works on the user's own files at run time; reports, dumps,
translated programs and fixtures stay under `C:\dev\out\raf\analysis` (outside the repository).

## 1. Measured facts

### 1.1 Air Paint is a static theme (M)

`dev_flash/vsh/resource/theme/01.p3t` (2,016,992 B, SHA-256 `BB2059AB...922CA`, `info/name` "Air Paint"):

| Check | Result |
|---|---|
| `--p3t-dump` tree | 92 elements: `bgimage=2, bgimagetable=1, color=1, font=1, icon=63, icontable=1, info=1, infotable=1, localizedinfo=20, theme=1` |
| `bgimage` attributes | both are `hd` + `sd` only (packed hd 172,356 B / 205,948 B); **no `anim`** |
| `--raf-report` | `dynamic=false`, `hasScene=false`, exit 3 |
| raw scan for `_RAF`, `RAFO`, `VSMX`, `EA03`, `.edge`, `.gtf`, `.jsx` | 0 hits |

So Air Paint has no actors, models, effects, GTF textures, animation or script: there is nothing to animate. It already
renders completely today (still background + 63 icons). The only open Air Paint question is which of its two
backgrounds the console shows when (THEMES.md 1.2; not a RAF matter).

### 1.2 Which dynamic themes exist locally (M)

Magic scan (`P3TF`/`_RAF`/`RAFO`/`VSMX` at offset 0) of RPCS3's `dev_flash`, `dev_flash2`, `dev_flash3`, `dev_hdd0`,
`dev_hdd1`, `dev_usb000`, plus a `*.p3t`/`*.raf`/`*.pkg` name search of `C:\dev`, the user's Downloads/Desktop/Documents
and `D:`/`E:` (depth 6): the only RAF scene is `dev_flash/vsh/resource/coldboot.raf` (and copies of it); the only theme
is Air Paint. `dev_hdd0/theme` does not exist. The `.pkg` files found are games. **No dynamic theme is available.**

### 1.3 coldboot.raf through the extended `--raf-report` (M)

The only real RAF, used as the stand-in for "a real scene":

| Item | Value |
|---|---|
| Actors / models / lights / camera | 6 (logo, footer, blur + `_sd` copies) / 1 (`plane`) / 2 (point, ambient) / perspective, yfov 0.927 |
| Edge | `plane.edge`: 1 segment; native decoder: 16 vertices, 18 triangles (ok) |
| Textures | 6 GTF, all format `0xa1` = B8 **linear**, header mipmap byte 10 (HD 700x350) / 8 (SD 240x120); level 0 decodes |
| Effects | `sce01` only (tokens: base `sce01`, no modifiers) |
| `.skel` / `.anim` | 48-B skeleton; 0 animations (no EA03) |
| Script | `coldboot.jsx`, VSMX 4,848 B, 518 instructions, 4 function literals |
| Script constructors | `Actor` x6, `Camera` x1, `Light` x1, `OneShotTimer` x4 |
| Script methods | `setColor` x8, `setPosition` x2, `setAttenuation` x2 |
| Runtime globals used | `Actor, Camera, Light, System, OneShotTimer` |
| Properties read / written | `resolution, aspect, position, timer` / `scale, color, position` |
| Opcodes without a known name | `0x4a` (1x, in the `System.resolution` test), `0x4d` (6x, operand 2, in the SD branch) |
| Native evaluator (`vsmx_eval`) | keys for 7 actors/lights; not evaluated: `setAttenuation`, two calls, non-linear (Bezier) easing |

### 1.4 What the firmware's RAF runtime offers a script (TRACED, raf.prx strings)

From the user's decrypted `raf.prx` (4.93, local only; strings, not yet disassembled):

- **Script globals**: `Actor`, `Camera`, `Light`, `IntervalTimer`, `OneShotTimer`, `Date`, `Math`, `System`.
- **Time of day**: the clock reaches a scene only through the script's `Date` object ("getting the local time
  failed", "too many date objects"; the module imports `cellRtc`). There is no time-of-day attribute in the RAF tree.
  That this is the only clock input is INFERRED (no other clock string or import found).
- **Actor properties**: `position, rotation, scale, uv_scale, uv_offset, color, material, anim_weight, anim_speed,
  anim_time`; `zsort` = `unspecified | back_to_front | front_to_back`; `Base.prototype.setAttrib()`; interpolation and
  Bezier type errors (so timed property changes take an interpolation mode).
- **Effects** are composed from tokens: base `pure_texture | basic_lighting | sce01`, modifiers `edge_lit, alpha_0,
  alpha_1, alpha_add, depth_0, depth_1`. `--raf-report` now splits each effect into these tokens (`effectTokens`).
- **Textures**: `wrap_s/t/p, min/mag/mip_filter, format, dimension, width, height, depth, miplevel`.
- **Post**: `Blur` (`/blur.fpo`, `/blur_mask.fpo`), `AlphaAdd`, `AlphaMul`, `lib/glutils/ScreenCopy.fpo`, ConeFilter.
- **Edge** geometry and animation run as SPU jobs (`edgegeom.cpp`, `edgeanim_spu.cpp`).

### 1.5 The effect programs exist in the user's firmware (M)

`dev_flash/vsh/resource/qgl/raf.qrc` holds `lib/raf/vert0.vpo`, `sce01.fpo`, **sixteen `_gen_ubber1_XXXX.fpo`**
(four flag digits), and `glutils/ScreenCopy`, `ConeFilter(.vpo/.fpo)`, `ConeFilterSimple`. raf.prx builds the name from
`lib/raf/` + `_gen_ubber1` + flags + `.fpo`. `--dump-cg` (now listing all sixteen) translates **all 18 RAF programs**
with the existing `CgProgram` reader, at run time from dev_flash, exactly the way `coldboot.cpp` already loads
`vert0`/`sce01` (nothing committed). What the translations show:

| Digits | Instructions | Inputs | Reading |
|---|---:|---|---|
| `xxx0` (8 variants) | 13 | ColorMap, Material, vertex colour, LightingMisc | unlit textured (`pure_texture`-like) |
| `0xx1` | 68 | + SpotLight, SpecularMap, DiffuseWarp, normal, world pos/eye | lit (`basic_lighting`-like) |
| `10x1` | 78 | + NormalMap, ModelInverseTranspose | lit, object-space normal map |
| `11x1` | 90 | + NormalMap, tangent | lit, tangent-space normal map |

The two middle digits produce byte-identical translated code (`0001`=`0011`=`0101`=`0111`; `0000`=`0010`), and
`1000` differs from `0000` only in the sampler unit.

**Traced `_gen_ubber1_XXXX.fpo` and effect-token mapping from `raf.prx` (`FUN_00031e68` at VMA `0x031ea0..0x032c80`, `FUN_0003f5e0` at `0x03f5e0..0x03f874`, `0x0404a4..0x0405f8`, and `FUN_00040fb0` at `0x040fb0..0x041260`, implemented in `src/raf_effect.h`):**

1. **Effect token parser (`FUN_00031e68` at `0x031ea0..0x032c80`):**
   - Defaults: `baseEffect = "pure_texture"`, `edgeLit = 0`, `depthTest = 1`, `depthFunc = 0x0203` (`GL_LEQUAL`), `depthMask = 1`, `alphaTest = 1`, `alphaFunc = 0x0204` (`GL_GREATER`), `alphaRef = 0.0`, `blendEnable = 1`, `blendSrc = 0x0302` (`GL_SRC_ALPHA`), `blendDst = 0x0303` (`GL_ONE_MINUS_SRC_ALPHA`).
   - Substring checks:
     - `"pure_texture"` -> `baseEffect = "pure_texture"`
     - `"basic_lighting"` -> `baseEffect = "basic_lighting"`
     - `"sce01"` -> `baseEffect = "sce01"`
     - `"edge_lit"` (`0x0329a8`) -> `edgeLit = 1`
     - `"alpha_0"` (`0x032a24`) -> `blendEnable = 0`, `alphaTest = 0`
     - `"alpha_1"` (`0x0329d4`) -> `alphaTest = 1`, `alphaFunc = GL_GREATER (0x0204)`, `alphaRef = 0.0`, `blendEnable = 1`, `blendSrc = GL_SRC_ALPHA (0x0302)`, `blendDst = GL_ONE_MINUS_SRC_ALPHA (0x0303)`
     - `"alpha_add"` (`0x032a8c`) -> `alphaTest = 1`, `alphaFunc = GL_GREATER (0x0204)`, `alphaRef = 0.0`, `blendEnable = 1`, `blendSrc = GL_ONE (1)`, `blendDst = GL_ONE (1)`
     - `"depth_0"` (`0x032a54`) -> `depthTest = 0`, `depthFunc = GL_ALWAYS (0x0207)`, `depthMask = 0`
     - `"depth_1"` (`0x032970`) -> `depthTest = 1`, `depthFunc = GL_LEQUAL (0x0203)`, `depthMask = 1`
2. **Shader index & 4-digit `_gen_ubber1_XXXX.fpo` selection (`FUN_0003f5e0` at `0x03f740..0x03f874`, draw-time selector at `0x0404a4..0x0405f8`, name builder `FUN_00040fb0` at `0x0411a0..0x041260`):**
   - At startup (`0x042180..0x0422bc`), `raf.prx` preloads all 16 `lib/raf/_gen_ubber1_XXXX.fpo` programs for `flags = 0..15`, where digit `bit` (`0..3`, left to right) is `'1'` if `(flags & (1 << bit)) != 0` else `'0'`, plus index `16` = `lib/raf/sce01.fpo`.
   - `"sce01"` -> `shaderIndex = 16` (`lib/raf/sce01.fpo`).
   - `"pure_texture"` -> `shaderIndex = 1` (`lib/raf/_gen_ubber1_1000.fpo`, bit 0 set).
   - `"basic_lighting"` -> `shaderIndex = -1` at material init, resolved at draw time (`0x0404a4..0x0405f8`) from the material's bound texture slots:
     - Bits `0..1` (first two digits): `0` (`00xx`) when no normal map is bound; `1` (`10xx`) when object-space normal map (`+0x74`, type 1) is bound; `3` (`11xx`) when tangent-space normal map (`+0x7c`, type 3) is bound.
     - Bit `2` (third digit): `4` (`xx1x`) when reflection map (`+0x80`, type 4) is bound.
     - Bit `3` (fourth digit): `8` (`xxx1`) when lighting enable (`+0x180`, initialized to `1`) is set.

## 2. What exists in code (status of section-3 steps 1-4)

| Step | Piece | Where | Evidence |
|---|---|---|---|
| 1 | `--raf-report`: scene graph, per-texture GTF facts (`formatCode`, `layout` linear/swizzled, `mipmaps`, `dimension`, `cubemap`, `decode` ok/error), per-model `edgeDecode` ok/vertices/triangles or error, `effectTokens`, a truthful `renderer` block (`drawn: still`, `scriptRun: false`, `firmwarePrograms: false`, kill switch) | `P3t::rafReportJson` | M (coldboot.raf, fixtures) |
| 2 | VSMX disassembler (`disassembleVsmx`) + usage sweep (`vsmxUsage`: constructors, methods, calls, runtime globals, properties read/written, unknown opcodes, function literals) in the report's `script.usage`; the evaluator's `notEvaluated` list in `script.evaluator` | `src/vsmx_eval.*` | M (coldboot), unit tests |
| 3 | GTF decode: B8, A8R8G8B8 (linear and Morton-swizzled), DXT1/3/5; level 0 only | `src/gtf_decode.*` | M (coldboot B8), unit tests for the others |
| 4 | Edge decode: one segment, compressed index stream, the plane.edge vertex descriptor set only (float3 / X11Y11Z10 / half2); everything else refused | `src/edge_decode.*` | M (coldboot), unit tests |
| 4 | Still frame (`renderRafStill`): offscreen GL 3.3, generic GLSL lit/unlit/edge-lit, initial pose, no script (`--raf-render file.raf@out.png`, pinned by `raf-static-scene`, and `XMB_RAF_ANIMATE=0` fallback) | `src/raf_render.*`, `View::loadWallpaper` | M on fixtures (section 4) |
| 5 | Per-frame animated RAF playback (`RafScenePlayer` + extended `evalVsmx`): `Date`, `Math`, `System.resolution/aspect/timer`, `IntervalTimer` looping, `OneShotTimer`, user function `CALL`, `setPosition/setRotation/setScale/setColor/setUvOffset/setUvScale/setAttenuation`, Hermite smoothstep easing, per-frame GL 3.3 background draw beneath the XMB glass icons and UI cache, clean GL release in `View::~View()` | `src/vsmx_eval.*`, `src/raf_render.*`, `src/view.*` | M (`xmb-raf-tests`, `coldboot.raf` multi-time renders) |

The interpreter (`evalVsmx`) now covers both the coldboot subset and the core dynamic-theme runtime globals:
`new Actor/Light/Camera("id")`, `new Date()` (`getHours`, `getMinutes`, `getSeconds`, `getMilliseconds`, `getTime`),
`Math` (`PI`, `E`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `abs`, `sqrt`, `floor`, `ceil`, `round`, `exp`,
`log`, `pow`, `min`, `max`, deterministic `random`), `OneShotTimer(delay, fn)`, periodic `IntervalTimer(period, fn)`
(unrolled across a bounded tick window with `loopPeriod` wrapping in `sample()`), user function calls, property reads/writes
(`position`, `rotation`, `scale`, `color`, `uv_offset`, `uv_scale`, `attenuation`) with array indexing, and timed `set*`
methods seeded from the RAF tree's `initialPose` with linear (`curve == 0`) or Hermite smoothstep (`curve != 0`) easing.
What remains approximated is the shader layer (generic GLSL lit/unlit/edge-lit rather than untraced `_gen_ubber1` flag bits),
single-segment Edge meshes, and skeletal `.anim` keyframes.

## 3. The honest gap (why it does not animate)

1. **No dynamic theme to measure (UNPROVABLE here).** Every number a theme renderer must match (effects actually used,
   texture formats, mesh streams, script API surface, scene size) is unknown for real themes. Building playback against
   coldboot.raf alone would fit one 10-second logo scene.
2. **Time lives in the script** (M for coldboot, TRACED for the runtime): `Date`, timers and interpolated property
   changes. Drawing the initial pose is the still; anything more is the interpreter.
3. **Edge**: only the one vertex-descriptor set from plane.edge; multi-segment meshes, other stream formats and skinning
   are refused (refusal = still background).
4. **Skeleton/`.anim`**: EA03 headers only; no keyframes.
5. **Effects**: the firmware programs are available (1.5) but the effect-name -> flag mapping is untraced, so the still
   uses approximate GLSL (`renderRafStill`: lit = point light + ambient + fixed specular; edge-lit adds a rim term;
   `pure_texture*` unlit). Normal/specular maps are ignored.
6. **Textures**: mip levels > 0, cubemaps and 3D textures are not decoded (level 0 only).
7. **Composition**: the still is a picture in the wallpaper slot (drawn once per window size, under the same 35/255
   black veil as any wallpaper), not a per-frame 3D layer; zsort, blur/post effects, pause/resume do not exist.

## 4. Verification this pass

- `--raf-report` on Air Paint: exit 3, `dynamic=false` (M). On coldboot.raf: the table in 1.3 (M).
- `--raf-render` on coldboot.raf: 6 actors, 1 model, 6 textures, 18 triangles, camera and light from the scene (M).
- Local fixtures (`C:\dev\out\raf\analysis\fixtures`, built by a local script, never committed): a minimal P3TF with
  a magenta still `bgimage hd` and `anim` = the firmware's coldboot.raf (`dynamic-ok.p3t`), and the same with one
  vertex-descriptor byte of plane.edge changed (`dynamic-broken.p3t`). `--raf-report` parses both as embedded scenes;
  `--raf-render` draws the first and fails the second with "model plane: unsupported vertex stream format".
- In the running shell (theme selected, wallpaper mode): see section 4.1.
- Unit tests `xmb-raf-tests` (synthetic fixtures only): GTF B8/linear pitch/A8R8G8B8 swizzled and linear/DXT1 (both
  modes)/DXT3/DXT5 and five refusals; Edge quad (selectors, reuse codes, explicit delta, positions, normals, half UVs)
  and three refusals; VSMX disassembly operands, usage sweep (including a constructor whose argument is a function
  literal), evaluator interpolation and timer, unknown-opcode reporting, two refusals. 41 checks, all pass.
- Pixel gate: the gated states use the Original theme; the theme path is not on their route. Result in 4.2.
- No new pinned gate state. The existing `raf-static-scene` state already pins `renderRafStill` on coldboot.raf (read
  from dev_flash at run time); a state with a theme selected would need a committed dynamic theme, and the only scene
  available is firmware.

### 4.1 Shell run (fallback and kill switch) (M)

`--script` grabs of the real shell (scratch data dir, preferences `appearance/theme` = fixture, `appearance/background`
= 2, held under the gate lock), local only:

| Run | Log | Picture |
|---|---|---|
| `dynamic-ok.p3t` | `theme: animated scene shown as a still frame (initial pose; script not run)` | the coldboot scene's PS3 logo behind the XMB (generic GLSL, initial pose, SD and HD actors overlaid), no magenta |
| same, `XMB_RAF_THEMES=0` | no scene line | the magenta still background, as before this work |
| `dynamic-broken.p3t` | `theme: scene render failed; using still background: model plane: unsupported vertex stream format` | the magenta still background |

The still is drawn once per window size on the UI thread (offscreen GL context) when the theme is applied; it is not
redrawn per frame.

### 4.2 Gate (M)

Full run: 27/28 states + 6/6 layers; the one failure was `ingame-xmb` (frame PSNR 52.43, game frame and guide
identical), the known flaky state, with the same score in other worktrees' runs of unrelated branches. Alone on the
final build: PASS (frame 60.21 / SSIM 1.0000). So 28/28 + 6 layers. `raf-static-scene` passed in the full run. No
reference was saved or replaced.

## 5. Remaining work, in order

1. **Get one or more real dynamic themes** (the user's own `.p3t`) and run `--raf-report` on them. Record effects,
   `effectTokens`, texture `formatCode`/`layout`/`mipmaps`, `edgeDecode` results and `script.usage` here. This
   decides everything below; without it, stop.
2. Trace in raf.prx the code that formats `_gen_ubber1` + flags (string at 0x53278 in the local module) to map effect
   tokens to the four digits and to blend/depth state. Then load those programs from `raf.qrc` at run time like
   `coldboot.cpp` does (no committed blobs needed).
3. Extend Edge decode to the stream formats and segment counts the real themes use; GTF mip/cubemap only if used.
4. Interpreter: extend `evalVsmx` to the calls `script.usage` lists for the real themes (`Date` for time of day,
   `Math`, `IntervalTimer`, rotation/animation properties, Bezier easing via the `Track` evaluator in coldboot.cpp).
5. Move the scene from a one-off offscreen still to a per-frame layer behind the XMB (pause/resume, device loss,
   fallback mid-run), then add a V2 pinned state from a fixture that can be committed.

Size: **L** remains for playback; step 1 is a user action, step 2 a short trace.
