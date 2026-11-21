# Browse by Genre — native interaction evidence (2026-11-20)

Closes the interaction evidence gap for the approved "Browse by Genre" slice under Game → Download
Games (commit `d6142c1`). The backend behaviour was already proven by the deterministic `xmb-release-tests`
fixtures; this note records the same route driven through the real `re-ps3-native.exe` against a
deterministic loopback Hydra catalogue, asserted from the capture sidecars rather than pixels.

## 1. What this is

A focused functional capture, **not** a parity or whole-release gate. It proves the native route
navigates, labels and returns exactly as the code says it does:

```
Game → Download Games → Browse by Genre → genre list
     → "Popular Games: Action" (ranked) → "Havoc Runner" details
     → source/edition options (2 sources + 1 Deluxe edition) → Back × 3
```

It deliberately does **not** contact a live feed, and never activates a download or installer row.

## 2. Producer

`work/browse-by-genre-e2e.ts` (committed). It replaces the historic `work/legacy-store-capture.ps1`
producer, which only reached the hub and could not drive the genre route. The new producer:

- serves a four-title hot catalogue and per-title `/games` responses from an in-process `Bun.serve`
  loopback, pinned with `XMB_HYDRA_TEST_FIXTURE=1`, `XMB_HYDRA_HOT_URL` and `XMB_HYDRA_GAMES_URL`;
- launches the real app with the reviewer's dev_flash (`C:\dev\rpcs3\bin\dev_flash`), a scratch
  `--data-dir`, `XMB_STEAM_ROOT` pointed at an empty dir, and `TMP`/`TEMP` pointed at the scratch dir;
- drives the route with the ordinary `--script` keys (DOWN / ENTER / ESC) and the existing `grab=`
  sidecar token, then asserts on the sidecars.

Reproduce:

```powershell
& .\native-xmb\build.ps1 -OutputDirectory C:/dev/xmb-wt/browse-by-genre/out/bbg-tests   # or any built exe
bun work/browse-by-genre-e2e.ts --exe C:/dev/xmb-wt/browse-by-genre/out/bbg-tests/re-ps3-native.exe
```

## 3. Evidence (all 28 checks pass)

`out/bbg-e2e/` (gitignored) holds seven framebuffer grabs, their `.png.json` sidecars, and
`requests.log`. The sidecars are the assertion authority; the PNGs (≈360 KB each) confirm the pages
rendered.

| Step | Assertion | Observed |
|---|---|---|
| 01-genres | page title / depth / 3 genres | `Browse by Genre` / depth 2 / Action·2, Adventure·2, RPG·1 |
| 02-results | honest title / ranked rows | `Popular Games: Action` / Havoc Runner, Iron Bastion |
| 03-details | title / Download first / metadata rows | `Havoc Runner` / `game:detail:download` / 7 rows |
| 04-sources | complete options | 3 rows: Source Alpha·2 GB, Source Beta·3 GB, Source Beta·4 GB (Deluxe) |
| 05–07 | Back × 3 | sources→details→results→genre list, each with the right title/depth/focus |
| loopback | only `/hot` + `/games` hit | 1× `/hot`, `/games` for Havoc Runner and Iron Bastion only |

The `requests.log` is the honesty proof for "no fan-out over non-matching titles": the "Action"
selection resolved `/games` for exactly the two Action games and never asked for Quiet Grove (RPG) or
Starlight Drift (Adventure). No other endpoint was contacted, so no live feed and no download/installer
path ran.

## 4. One supporting change

`native-xmb/src/main.cpp` adds `pageTitle` to the `grab=` capture sidecar. It is a non-visual,
additive capture field (no UI or visual change, no baseline or gate change) and is what lets the
harness assert the honest `Popular Games: <genre>` title rather than infer it from row content.

## 5. Scoping and remaining gaps

- **Edition/source union** is proven two ways: here (Havoc Runner carries two sources and a Deluxe
  edition that stays a separate option in the source list) and in the deterministic backend fixtures
  (`tests/release_tests.cpp`: `RPG` selects the base tile plus its edition, unioning duplicate sources).
- **Not run here:** the render gate (`bun work/ab-layers.ts --all`) and the parity corpus
  (`bun work/parity.ts`) require the local `out/native-reference` baselines, which are absent on this
  machine. These captures are focused functional evidence; they are **not** a substitute for the
  parity/whole-release evidence and the gate was not lowered or replaced.
- **Pre-existing, unchanged:** `xmb-release-tests` reports 29 failures in the download/RealDebrid/
  archive-cleanup section from the documented `QTemporaryDir` sandbox issue
  (`audit/PHASES-GAME-SEARCH-2026-11-17.md` §3); the 22 genre checks added here all pass.
- **Cosmetic, consistent with existing idiom:** the genre count detail renders `2 game(s)` / `1 game(s)`
  because the source string reuses the app's existing `%n … (s)` plural pattern (as `optionCount` does);
  no new wording was introduced.
- **Deferred by the approved scope:** paged Popular (`take > 40`), offline cache, recent search.
