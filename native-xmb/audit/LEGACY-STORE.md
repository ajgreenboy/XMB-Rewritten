# Legacy PlayStation Store (GEN 2, 2008–2012) — catalogue / detail / search entry

## Scope (this pass)

Faithful GEN 2 PlayStation®Store **visual browse** over the existing JSON/Hydra download-games feed
(`release_shell.cpp`'s `openBrowseGames` → `HydraLibrary::popularGames`). Three surfaces:

1. **Catalogue** — featured pane left, compact cover grid right, blue chrome, top Search button. Each game
   appears once (deduplicated by the existing `popularGames`/search merge).
2. **Item detail** — one game's cover, title, size, source summary, and a Download action.
3. **Search entry** — the top Search button opens the existing on-screen-keyboard search screen
   (`openGameSearch("all")` → `requestJellyfinFieldEdit("game:search:all")` → `runGameSearch`).

Out of scope and **left for the separate worker**: the source-choice list, the drive picker/confirm, and the
Download Management job lifecycle (`openGameSources` / `beginGameDownload` / `release_jobs.cpp`). The detail
page's Download action hands off to `openGameSources` unchanged; none of those files were modified.

## Evidence

- `C:\dev\out\legacy-store-reference\manifest.md` §3.1–3.2 — the firmware 3.10/3.15 manual callout topology:
  (1) Game/Video switch, (2) **Search / Redeem Codes / View Downloads / View Cart** control buttons,
  (3) View Mode, (4) products, (5) avatar+online ID, (6) cart marker, (7) "C" / (8) "buy" markers.
- `store001.jpg` / `purchase001.jpg` (Sony manual illustrations) — confirmed by the orchestrator to show
  **featured left + compact cover grid right**, a **top Search icon** (not an inline field), and a **blue**
  item-detail shell. These are downscaled crops; pixel coordinates are not recoverable from them.
- `newstore_plugin.sprx` / `.elf.dec` (decrypted `Legacy_PlayStation_Store.pkg`, `REPORT.md`) — the actual
  native store plugin is an HTML/Flash shell with `ButtonIconSearch` / `ButtonIconRedeem` / `ButtonIconCart` /
  `ButtonIconDownload`, `grid_color` / `list_color` (a grid/list view mode), and category colour/font slots.
  Confirms the four control buttons and the grid view. **Not** redistributed; used as private reference only.
- `spec.md` was **ignored** where it maps the catalogue to `SteamStore` — this feed is JSON/Hydra only, per
  the task override.

## Fidelity limits (do not claim more than this)

- **Colours and geometry are native choices.** The manual illustrations carry no pixel/hex metadata and this
  authoring model cannot inspect raster images, so the blue palette and every rectangle in
  `View::drawLegacySurface` are authored, not measured. They are not pixel-perfect claims.
- **D-pad cursor is a native choice.** No captured frame traces the 2008 store's 2D cursor, so the moves in
  `Shell::legacyStoreMove` are a clean bounded 2D cursor over the documented regions (Search ⇄ Featured ⇄
  grid), not firmware-traced.
- **Grid window scrolls 3 rows at a time** (`legacyStoreGridRows`), one row per boundary crossing; the scroll
  rate is a native choice (the store scrolled, but no trace fixes its step).
- **Control buttons: correct order, partial icons.** The four controls are drawn in the manual's order
  **Search · Redeem Codes · View Downloads · View Cart**, as buttons. Firmware glyphs are used where they
  exist (`explore_category_network:tex_kensaku` for Search, `tex_dl_manage` for View Downloads); Redeem and
  View Cart have no firmware glyph, so they are label-only. The manual's own `in_pss_search.gif` etc. are
  copyrighted Sony material and are **not** bundled. Only Search is focusable; the other three are drawn
  dimmed as their documented labels but are unfocusable, because their PSN surfaces are out of scope and
  would otherwise be faked.
- **View Mode toggle is grid-only.** The manual's View Mode control (list vs grid) is drawn as a static
  "View Mode: Grid" reading; the list mode is not implemented, so it is not a live toggle and is not
  presented as one.
- **The grid is dark blue, not white.** The white area at x>300 in `store001.jpg` is Sony's explanatory
  callout canvas outside the storefront crop (x≈0..300), not a store surface, so it is not reproduced.
- **No wallet/cart.** Every item's primary action is Download (the feed has no prices), matching the manual's
  free-item path.
- **Triangle** on the store still opens the focused game row's ordinary item menu (it inherits the
  `GameSearchResult` row's options); the console store had no such panel, so this is a known, unclaimed
  divergence rather than a fabricated store behaviour.

## Metadata backend gaps

`HydraLibrary::GameResult` (and the `catalogue/hot` + `/games` endpoints it reads) carry only:
`name`, `title`, `originalTitle`, `coverUrl`, and per-option `source`, `sourceUrl`, `fileSize`,
`sourceStatus`. There is **no** `description`, `genre`, `releaseYear`, `developer`, `publisher` or `platform`
in the model or the API payloads. The detail page therefore shows those six fields as **"Not available"**
and does not invent values. Filling them needs a different endpoint (or a per-title metadata lookup) and is a
stated gap for a later pass.

## Files

- `src/legacy_store.cpp` — new: cursor navigation + detail handoff (`legacyStoreMove/Activate`,
  `legacyDetailActivate`, `openLegacyDetail`).
- `src/shell.h` — `Page::{legacyStore,legacyDetail,legacyScroll,legacySearchFocus}`; accessors; grid constants;
  private method declarations.
- `src/shell.cpp` — routes `horizontal`/`vertical`/`activate` to the store while it is open; `pageKind()` and
  `focusedRowId()` report the surface.
- `src/view.cpp` / `src/view.h` — `View::drawLegacySurface` (catalogue + detail), short-circuited over the XMB
  column/category-bar/info-bar in `drawScene`.
- `src/release_shell.cpp` — `openBrowseGames` now pushes a `legacyStore` page (featured+grid) instead of the
  flat list, plus the `XMB_LEGACY_STORE_FIXTURE` offline test hook.
- `CMakeLists.txt` — adds `legacy_store.cpp`.

## Verification

- Full build (`build.ps1`) passes.
- Scripted run (`--script`, `XMB_LEGACY_STORE_FIXTURE=1`) reaches the store and asserts by state dump:
  `pageKind == "legacyStore"` → cursor moves `selection` 0 (featured) → grid cells → `pageKind ==
  "legacyDetail"` on Cross → back → `legacySearchFocused` on Up. Last grid cell (`selection 13` of 14) does not
  break the `selection() < entries().size()` invariant.
- Pixel sampling of the grabbed frames confirms the blue chrome header, the dark-blue ground (not white),
  and the fixture covers render (not a black/blank frame).
- The live Hydra network could not be used for the visual check in this environment (the `catalogue/hot`
  round-trip hangs); the fixture is the deterministic stand-in and is gated behind `XMB_LEGACY_STORE_FIXTURE`.
- Reproducible captures (frames + `grab=` sidecars + a reproduction `manifest.md`) are written outside the
  repo at `C:\dev\out\legacy-store-ui-check\` — see that manifest for the exact command.

## Integration note

Isolated branch base `941e764` predates `main` `d59850f` (flow corrections). The only shared-file overlap is
`src/release_shell.cpp` (`openBrowseGames` body + the fixture helper) and the `horizontal`/`vertical`/
`activate`/`pageKind`/`focusedRowId` hunks in `src/shell.cpp`; a later cherry-pick must re-check those
locations. No Download Management or `release_jobs.cpp` changes were made.
