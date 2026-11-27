# Rust hardening design: the untrusted edge

Status: **Phases 1–3 implemented** (`native-xmb/rust-edge/` Cargo workspace: `firmware-crypto`, `firmware-extract`, `steam-vdf`, `image-decode` [GTF, Edge, Blu-ray PGS, DVD VobSub], unified into `xmb-rust-edge` staticlib behind `-DXMB_RUST_FIRMWARE_CRYPTO=ON` and `-DXMB_USE_RUST_EDGE=ON`; Phases 4–5 remain proposals).

## 1. What this is and what it is not

This document scopes moving the XMB's **untrusted-input surface** — the code that parses foreign
bytes and talks to remote hosts — from C++ into Rust, behind a narrow C ABI. The Qt renderer stays
C++. The reason is stated up front because it defines every boundary below:

- **Motivation:** memory safety / robustness (owner's stated driver, 2026-11-20).
- **What moves:** firmware crypto and extraction, archive extraction, image/font decoders, network
  clients, metadata ingestion. The files that consume bytes this machine did not author.
- **What stays C++/Qt:** `shell.cpp`, `view.cpp`, the widget tree, themes, the OSK, dialogs, input,
  and everything the render gate measures. None of that is the memory-safety risk, and porting it
  re-breaks the fidelity bar for zero benefit.
- **What this is not:** a whole-project Rust conversion, a Qt replacement, or a new renderer.

## 2. Why the boundary is here (and not elsewhere)

A Rust migration has positive payoff only when it lands on the attack/parse surface. A file-level
scan of `native-xmb/src/` (`new`/`delete`/`memcpy`/`memmove`/`strcpy`/`sprintf`/`reinterpret_cast`
+ the parsing/network/crypto modules) put the hotspots where the scan puts the risk: not in the
renderer, in the I/O edge. The renderer is hot (`shell.cpp` 51 hits, `view.cpp` 44) but that churn is
frame work over already-owned heap buffers; it is not where unbounded external input enters.

The candidate surface, by risk order:

| Area | Files (`native-xmb/src/`) | Input it trusts |
|---|---|---|
| Firmware crypto + extract | `firmware_crypto.*`, `firmware_extract.*`, `firmware_keys.inc` | ~200 MB `PS3UPDAT.PUP` downloaded over plain HTTP; AES/HMAC over Sony's image |
| Archive extraction | `archive_extract.*`, `file_ops.*` (18 hits) | `.pkg`, zip, ISOs, game folders, repack `.bin`s |
| Image/font decode | `edge_decode.*`, `gtf_decode.*`, `rco_objects.*`, `image_subtitles.*` (13) | GTF/PNG + sidecar/PGS/VobSub subtitle bytes |
| Network clients | `jellyfin_client` (13), `dlna_client` (8), `spotify_client` (7), `discord_client` (6), `wol_client` (10), `cec`, `bluetooth_client` | XML/JSON/protocol framing from remote hosts |
| Metadata | `catalog.*`, `content_registry.*` | Imported game metadata |

The first two rows are the priority: they are the largest `memcpy`/allocation hotspots *and* the most
self-contained modules, so they deliver the biggest safety win with the cleanest FFI cut.

## 3. The shape: Option A — Rust `cdylib` crates behind a C ABI

Each area becomes a Rust crate compiled to a `cdylib` (or `staticlib`) exposing a minimal C ABI, linked
into the existing CMake build. The Qt shell keeps its own event loop and calls across the boundary on
narrow, owned buffers (length-prefixed, never borrowed into the other side).

```
+---------------------------+        +--------------------------------------+
| C++20 / Qt 6 shell        |        | Rust edge (safe, defensively parsed) |
|  renderer, dialogs, OSK   |        |                                      |
|                           |  C ABI |  firmware-crypto  (ring, sha2)         |
|  calls across the         | <----> |  firmware-extract (PUP/SCE, tar)       |
|  boundary on owned bytes  |        |  archives         (zip/iso/freearc)    |
|                           |        |  image-decode     (gtf/edge/subtitles)|
+---------------------------+        |  net-clients      (tokio, serde)      |
                                      |  catalog          (serde)            |
                                      +--------------------------------------+
```

### 3.1 The boundary rules (non-negotiable)

- **Owned, length-prefixed buffers only.** C++ passes `const uint8_t*` + `size_t` in; Rust returns a
  caller-freed buffer via `take_buffer`/`free_buffer`, never a pointer into Rust memory. No
  `std::string_view`/`&[u8]` crossing that outlives the call.
- **Error returns are `int` + an optional error string**, matching the existing `QString* error`
  pattern in `firmware_extract.h`. Rust never throws across the ABI; panics are caught at every
  `#[no_mangle]` export via `catch_unwind` and converted to a non-zero code.
- **No Qt types on the wire.** The crate headers include `<cstdint>`, not `<QByteArray>`/`<QString>`.
  The C++ side wraps them in a thin RAII shim that adapts to `QByteArray`/`QString`.
- **Crypto keys stay where they are.** `firmware_keys.inc` values are the public RPCS3 keys (owner
  decision, 2026-11-14, see `audit/FIRMWARE-FETCH.md`). The Rust crate embeds the same values; nothing
  new is committed, and they remain excluded from any public surface exactly as today.

### 3.2 Build integration

- CMake already drives the build via `build.ps1` → CMake/Ninja. Add a `FetchContent`/custom-command
  step that runs `cargo build --release` into `out/native-xmb/` and links the produced `.dll`/`.lib`.
  `corrosion` is the standard glue; if the repo prefers zero new CMake deps, a hand-written
  `add_custom_command` + `target_link_libraries` to the Rust `staticlib` is sufficient and keeps the
  toolchain footprint minimal.
- Rust toolchain becomes a build prerequisite alongside MSVC, CMake, Qt (recorded in
  `native-xmb/README.md` "Build from source").

## 4. Phase order (each phase ships and gates green on its own)

The ordering principle: highest risk first, most self-contained first, renderer untouched throughout.

| Phase | Crate | Replaces | Safety win |
|---|---|---|---|
| **P1** | `firmware-crypto` + `firmware-extract` | `firmware_crypto.cpp`, `firmware_extract.cpp` | HMAC-verified, bounds-checked unpack of a ~200 MB network image; correct rejection of a tampered PUP (already the design: `FIRMWARE-FETCH.md`) |
| **P2** | `archives` | `archive_extract.cpp`, `file_ops.cpp` parsing paths | Kills the largest `memcpy`/`new` cluster |
| **P3** | `image-decode` | `gtf_decode.cpp`, `edge_decode.cpp`, `image_subtitles.cpp` | Safe decode of attacker-shaped image/subtitle bytes |
| **P4** | `net-clients` | Jellyfin/DLNA/Spotify/Discord clients | `tokio`/`serde` async isolation behind the ABI |
| **P5** (optional) | `catalog` | `catalog.cpp`, `content_registry.cpp` | Schema-validated metadata ingestion |

P1 is deliberately scoped to the module that is already the most isolated and the most dangerous.
Proof of the FFI pattern on P1 de-risks P2–P5.

### 4.1 Phase 1 in detail (what "done" means)

- `firmware-crypto` exports `hmac_sha1`, `aes_ctr`, `aes_cbc` over a C ABI, `ring`-based, matching the
  exact byte output of the current `QByteArray` implementations (`firmware_crypto.h`). A byte-for-byte
  parity test against the C++ output is the acceptance gate — this is crypto; "close" is failure.
- `firmware-extract` exports `extract_pup` and `pup_version`, mirroring `xmb::extractPup` /
  `xmb::pupVersion` (`firmware_extract.h`). It reuses the existing PUP/SCE layout knowledge in
  `audit/FIRMWARE-FETCH.md` (48-byte header, 32-byte entries, per-entry HMAC-SHA1, `dev_flash_*` SCE
  packages, third-section TAR per package — each package is its own TAR, not one concatenated TAR).
- Safe TAR path handling (no path traversal) and transactional extraction (write to temp, rename on
  success) — the two explicit gaps the existing `firmware_extract` notes leave open.
- The render gate (`bun work/ab-layers.ts --all`) stays green throughout: the extraction output feeds
  the same `dev_flash` folder the renderer reads, so no renderer change is required.

## 5. Verification and honesty

- **Render gate and parity are the regression net** (`work/ab-layers.ts`, `work/parity.ts`). Because
  the renderer is untouched, every phase must keep the 6 layers + named states and the per-surface
  parity numbers where they are today. A Rust phase that moves a fidelity number is a bug, not a
  trade-off.
- **Byte-parity tests for P1 crypto.** The Rust `hmac_sha1`/`aes_ctr`/`aes_cbc` outputs are compared
  against the C++ `firmware_crypto` outputs on fixed vectors. This is the only place "match the old
  code exactly" is the correct bar (cryptographic correctness).
- **Provenance labels** (repo rule, `README.md` "Provenance and honesty"): a Rust port inherits the
  label of what it replaces. P1 is **traced** (the PUP/SCE layout is traced from RPCS3/console
  evidence); a new decoder that approximates a surface would be **fit**, not traced, and is recorded
  as such.
- **No fabricated results.** A green gate after P1 does not claim "more secure" — it claims "same
  output, safe-Rust implementation." The safety claim is structural (no unsafe in the parse path),
  stated as such, not measured.

## 6. Risks and honest costs

- **FFI is a new failure mode.** The boundary itself becomes a place to get pointer/lifetime bugs
  wrong. The `catch_unwind` + owned-buffer rules in §3.1 exist to contain it, but the C++ shim layer
  still needs the same review discipline as the unsafe Rust it fronts.
- **Toolchain footprint grows** (Cargo added to the build). Accepted as the price of the whole
  exercise; spelled out here rather than discovered later.
- **Two languages in one repo.** The repo rule "C++ for application code, Bun/TS for scripts" gains a
  third tool for exactly the edge modules. This is a deliberate, documented exception, not a drift —
  it should be recorded in `native-xmb/README.md` and re-justified if the edge layer grows beyond the
  modules listed in §2.
- **Parity risk is real but contained.** The renderer is untouched, so the gate should hold; the
  residual risk is an extraction/subtitle decode changing a byte stream in a way that shifts a
  fidelity number. The parity net catches it; the fix is a decoder fix, not a renderer fix.
- **Not a whole-project conversion.** If the goal silently widens ("let's do the renderer too"),
  revisit `README.md` and this section first: that is a different project with a fidelity cliff this
  document does not endorse.

## 7. Open questions to settle before P1

1. **Static vs dynamic link** for the first crate (`staticlib` keeps the MSVC/Ninja path simpler;
   `cdylib` isolates the toolchain). Recommend `staticlib` for P1.
2. **`ring` vs `openssl`/`RustCrypto`** for AES/HMAC. `ring` is self-contained and audited; confirm it
   covers AES-256-CBC + AES-128-CTR + HMAC-SHA1 with the exact modes `firmware_crypto.cpp` uses.
3. **Crate layout**: one `crates/` dir under `native-xmb/` vs a sibling top-level `rust-edge/`. Either
   is fine; needs an owner pick so `git status` stays unambiguous for the other agents on this checkout.
4. **Who owns the C++ shim** (`QByteArray`/`QString` adapters) — kept in `native-xmb/src/` as thin
   wrappers. Confirm the "C++ for application code" rule reads them as application code, not a
   violation.

## 8. Reference points

- `audit/FIRMWARE-FETCH.md` — PUP/SCE layout, HMAC verification, HTTP-only transport, key provenance.
- RPCS3 `Loader/PUP.{h,cpp}`, `Crypto/unself.cpp`, `Crypto/key_vault.{h,cpp}` — the public-key and
  layout sources the current implementation traces.
- `work/ab-layers.ts`, `work/parity.ts` — the regression net that must stay green.
- Rust bindings considered: `cxx`/`cxx-qt` (rejected for P1 — they drag Qt types onto the wire;
  a plain C ABI is narrower and safer), `corrosion` (optional CMake glue).

---

## 9. Phase 1–3 implementation record (2026-11-25 .. 2026-11-26)

Choices settled and verified for Phases 1–3:

1. **Unified Cargo workspace & single `staticlib` (`native-xmb/rust-edge/Cargo.toml`):**
   - `xmb-rust-edge` re-exports `firmware_crypto`, `firmware_extract`, `steam_vdf`, and `image_decode` into a single `xmb_rust_edge.lib` (Windows MSVC) / `libxmb_rust_edge.a` (Linux) with zero duplicate Rust `std` symbols.
2. **Phase 1 (`firmware-crypto` + `firmware-extract`):**
   - `RustCrypto` (`aes` 0.9, `hmac` 0.13, `sha1` 0.11) for FIPS-197 AES-128/256 and RFC 2104 HMAC-SHA1; `flate2` (`rust_backend` / `miniz_oxide`, pure Rust) for SCE zlib/gzip decompression bounded at 1 GiB; zero-`unsafe` PUP/SCE/TAR unpacker with traversal/symlink/device/reparse-point rejection and transactional staging.
3. **Phase 2 (`steam-vdf`):**
   - Zero-`unsafe` Valve KeyValues (`.vdf` / `.acf`) parser implementing all 6 `Options` fields (`skipBom`, `quotedBracesAreStructural`, `maxTokenBytes`, `maxTokens`, `maxEntries`, `maxDepth`) with owned length-prefixed binary serialization over C ABI (`include/steam_vdf_abi.h`, `src/steam_vdf_rust.{h,cpp}`). Verified against all **4,908 golden fixtures** in `tests/steam_vdf_golden.txt` (`0` failures).
4. **Phase 3 (`image-decode`):**
   - Zero-`unsafe` binary decoders (`include/image_decode_abi.h`, `src/image_decode_rust.{h,cpp}`) for:
     - **PS3 GTF textures** (`B8`, `A8R8G8B8` linear & Morton-swizzled, `DXT1/3/5`),
     - **PS3 Edge geometry** (`plane.edge` compressed MSB delta/reuse index stream + `float3`/`X11Y11Z10`/`half2` vertex streams),
     - **Blu-ray PGS (`.sup`)** (`HDMV RLE` + studio-range BT.709/BT.601 YCbCrA composition, cropping, window clipping, pixel-work budgets),
     - **DVD VobSub (`.idx` + `.sub`)** (`.idx` palette/custom-colors/delay/timestamp parser + MPEG-1/2 PES private-stream-1 walker + interlaced 2-bit SPU RLE).
   - Verified via 64/64 C++/Rust byte-for-byte parity checks in `xmb-firmware-rust-parity-tests` and 53/53 checks in Rust-delegated `xmb-raf-tests`.
5. **CMake & CI integration:**
   - `-DXMB_RUST_FIRMWARE_CRYPTO=ON` builds `xmb-rust-edge` and `xmb-firmware-rust-parity-tests`; `-DXMB_USE_RUST_EDGE=ON` routes live C++ calls (`firmware_crypto`, `firmware_extract`, `steam_vdf`, `gtf_decode`, `edge_decode`, `image_subtitles`) through the Rust `staticlib`. Both Windows and Ubuntu 24.04 Linux CI jobs in `.github/workflows/tests.yml` run `cargo test --workspace` and the C++/Rust parity suites on every push.
