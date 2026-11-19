# Legacy PlayStation Store (GEN 2, 2008–2012) — catalogue / detail / search entry

## Scope (this pass)

Faithful GEN 2 PlayStation®Store **visual browse** over the existing JSON/Hydra download-games feed
(`release_shell.cpp`'s `openBrowseGames` → `HydraLibrary::popularGames`). Three surfaces:

1. **Catalogue** — the 2008 browse layout: a **blue-gradient selected-game panel on the left** (square cover,
   title, Free·size, description) and a **compact 5-column cover grid on the right** over a dark-navy ground
   with a silver action footer and a top Search button. Each game appears once (deduplicated by the existing
   `popularGames`/search merge); the left panel follows the grid cursor.
2. **Product detail** — the 2008 product page: a dark inset panel with a title/publisher/price strip, square
   cover, vertical Download / Add to Cart / Preview buttons, and a long description.
3. **Search entry** — the top Search button opens the existing on-screen-keyboard search screen
   (`openGameSearch("all")` → `requestJellyfinFieldEdit("game:search:all")` → `runGameSearch`).

Out of scope and **left for the separate worker**: the source-choice list, the drive picker/confirm, and the
Download Management job lifecycle (`openGameSources` / `beginGameDownload` / `release_jobs.cpp`). The detail
page's Download action hands off to `openGameSources` unchanged; none of those files were modified.

## Evidence

- `C:\dev\out\legacy-store-reference\period\*.jpg` (2008-03-31 / 2008-04-03, 1024×576 Sony period references)
  — the primary visual references, described by the vision-capable orchestrator: the home's dark-navy full-bleed
  wave + luminous cyan PS3 streak, upper-left orb + PlayStation Store logo, upper-right avatar/ID, left vertical
  category buttons, right 2-column promotional art, silver footer; the **browse**'s selected-game panel
  (x157..505 y123..459) + 5-column cover grid (x510..860 y123..404) + Store Top / View Downloads / View Cart +
  silver footer; the **product detail**'s dark inset panel (x155..870 y122..459) with title/publisher/price strip,
  cover, vertical Buy Now/Add to Cart/Preview buttons, long description.
- `C:\dev\out\legacy-store-reference\manifest.md` §3.1–3.2 — the firmware 3.10/3.15 manual callout topology:
  (1) Game/Video switch, (2) **Search / Redeem Codes / View Downloads / View Cart** control buttons,
  (3) View Mode, (4) products, (5) avatar+online ID, (6) cart marker, (7) "C" / (8) "buy" markers.
- `newstore_plugin.sprx` / `.elf.dec` (decrypted `Legacy_PlayStation_Store.pkg`, `REPORT.md`) — the actual
  native store plugin is an HTML/Flash shell with `ButtonIconSearch` / `ButtonIconRedeem` / `ButtonIconCart` /
  `ButtonIconDownload`, `grid_color` / `list_color` (a grid/list view mode), and category colour/font slots.
  Confirms the four control buttons and the grid view. **Not** redistributed; used as private reference only.
- `spec.md` was **ignored** where it maps the catalogue to `SteamStore` — this feed is JSON/Hydra only, per
  the task override.

## Fidelity limits (do not claim more than this)

- **Colours and geometry are authored approximations, not measured captures.** The navy palette, the cyan
  PS3 streak (a soft radial glow) and every rectangle in `View::drawLegacySurface` follow the period
  references' region descriptions; this model cannot inspect the rasters and no hex/pixel metadata is
  recoverable from them. They are not pixel-perfect claims.
- **The D-pad cursor is a native choice.** No captured frame traces the 2008 store's 2D cursor, so the moves
  in `Shell::legacyStoreMove` are a clean bounded 2D cursor over the documented regions (Search ⇄ 5-column
  grid), not firmware-traced.
- **The grid label is the title, not the genre.** The Hydra feed carries no genre, so the reference's genre
  labels under the tiles are replaced by the title (which is available), drawn inside a dark band at the
  bottom of each square card, with a thin tile frame around every card.
- **The store surface uses no XMB drop-shadow.** `View::drawLegacySurface`'s text helper passes
  `shadow=false` to the text renderer (the firmware's 4 px dark drop-shadow reads as doubled text on the
  light silver footer).
- **The description is a placeholder.** Neither the selected-game panel nor the product detail has a real
  description (none exists in the feed); both show "Description not available for this title.", and the
  product detail shows "Publisher: Not available" and no platform/developer/genre/year. These are honest
  unavailable states, not invented copy.
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
- **The ground is dark navy, not white.** The white area at x>300 in `store001.jpg` is Sony's explanatory
  callout canvas outside the storefront crop (x≈0..300), not a store surface, so it is not reproduced; the
  period references' dark-navy full-bleed ground is.
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
  `pageKind == "legacyStore"` → cursor moves `selection` 0 → grid cells (left panel follows) → `pageKind ==
  "legacyDetail"` on Cross → back → `legacySearchFocused` on Up. Last grid cell (`selection 13` of 14) does not
  break the `selection() < entries().size()` invariant.
- Pixel sampling of the grabbed frames confirms the dark-navy ground `RGB(8,16,34)`, the silver footer
  `RGB(170,175,183)`, the blue selected-game panel `RGB(26,71,131)`, the dark detail inset `RGB(6,13,31)`, and
  distinct fixture covers across the 5 grid columns (not a black/blank frame).
- The live Hydra network could not be used for the visual check in this environment (the `catalogue/hot`
  round-trip hangs); the fixture is the deterministic stand-in and is gated behind `XMB_LEGACY_STORE_FIXTURE`.
- Reproducible captures (frames + `grab=` sidecars + a reproduction `manifest.md` + comparative pixel
  measurements) are written outside the repo at `C:\dev\out\legacy-store-ui-check\` — see that manifest for the
  exact command.

## Integration note

Isolated branch base `941e764` predates `main` `d59850f` (flow corrections). The only shared-file overlap is
`src/release_shell.cpp` (`openBrowseGames` body + the fixture helper) and the `horizontal`/`vertical`/
`activate`/`pageKind`/`focusedRowId` hunks in `src/shell.cpp`; a later cherry-pick must re-check those
locations. No Download Management or `release_jobs.cpp` changes were made.

## Visual polish (2026-11-19) — measured geometry/colour corrections

A vision-capable pass measured the rendered 960x540 captures against the 2008 period references
(`2008-04-03_us-store-detail-preview_flickr-2386461216.jpg` browse, `2008-04-03_us-store-2_flickr-2386461340.jpg`
detail, 1024x576 scaled x0.9375 to 960x540) and corrected four material differences in
`View::drawLegacySurface` (all in the 1920x1080 canvas; capture = /2):

- **Header logo** moved from x35..281 to the reference's x147..414 (canvas x294..828): the bag glyph now
  draws at `(294,76,88,88)` and the "PlayStation®Store" wordmark at `(390,84)`.
- **"Store Top"** moved from x83 y72 to x147 y83 (canvas `(294,164)`).
- **Cyan wave** replaced the previous upper-left orb/diagonal beam with an upper-centre wave: a soft
  cyan glow centred at `(1010,110)` plus a horizontal streak over `y0..250` (reference x~600..1430,
  y~0..250 in canvas). The exact flowing wave art is proprietary and is not reproduced without its
  asset — this is a material approximation, not a pixel trace.
- **Footer** corrected from a full-height silver band (y861..1080) to the reference's silver action
  strip `y930..1014` (canvas, ~84 px) over a dark navy lower policy bar `y1014..1080` (~66 px). The
  action hint ("Cross: Open / Circle: Back / D-pad: Browse" for catalogue, "Cross: Download / Circle:
  Back" for detail) is drawn once in the silver strip, still with `shadow=false` so it never doubles.

Left panel / grid / detail inset were already within tolerance and are unchanged. The top controls keep
the wired **Search** action (2010 legacy variant); Redeem Codes / View Downloads / View Cart and Add to
Cart / Preview stay label-only (no fake cart). The detail page's title/publisher/size and metadata stay
plain QPainter text (no rich text), so they remain readable and cannot inject markup.

Verification: the fixture captures and the analytical pixel comparison are reproduced by
`work/legacy-store-capture.ps1` (offline, `XMB_LEGACY_STORE_FIXTURE=1`) and
`work/legacy-store-geometry.ts`, which writes `geometry.json`/`geometry.md` next to the captures at
`C:\dev\out\legacy-store-visual-polish\`. Measured footer silver y465..506 (target 465..507), navy
policy bar `RGB(8,16,34)` to the frame edge, logo left at x158 (target x147), and a blue-dominant cyan
wave in the upper centre.
