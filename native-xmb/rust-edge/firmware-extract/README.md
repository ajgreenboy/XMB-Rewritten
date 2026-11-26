# `firmware-extract` (Rust edge Phase 1)

Safe-Rust reimplementation of `xmb::extractPup` and `xmb::pupVersion`
(`native-xmb/src/firmware_extract.cpp`), paired with `../firmware-crypto` behind
a plain C ABI (`include/firmware_extract_abi.h`).

## Boundary rules (`native-xmb/audit/RUST-HARDENING.md` §3.1)

- Owned, length-prefixed UTF-8 / byte slices across the ABI; zero Qt types on
  the wire.
- `catch_unwind` around every `#[no_mangle] extern "C"` entry point.
- Zero `unsafe` in `src/extract.rs` (the PUP/SCE/TAR parser and unpacker);
  `unsafe` is confined to pointer validation in `src/ffi.rs`.
- Transactional first-install staging (`<dest>.rust-stage-*` renamed on success,
  removed on failure) and symlink / NTFS reparse-point rejection.
- Re-exports `firmware_crypto` so linking `firmware-extract` provides both
  `fw_hmac_sha1` / `fw_aes_*` and `fw_extract_pup` / `fw_pup_version` in a
  single `staticlib` without duplicate Rust `std` symbols.

## Build and test

```powershell
cargo test --manifest-path native-xmb/rust-edge/firmware-crypto/Cargo.toml
cargo test --manifest-path native-xmb/rust-edge/firmware-extract/Cargo.toml
```

With CMake (`-DXMB_RUST_FIRMWARE_CRYPTO=ON`), CMake builds the unified Phase-1
`staticlib` and links `xmb-firmware-rust-parity-tests`, asserting byte-for-byte
parity for both crypto primitives and synthetic PUP extraction/rejection paths.
`-DXMB_USE_RUST_FIRMWARE=ON` (requires `XMB_RUST_FIRMWARE_CRYPTO=ON`) routes
`xmb::firmware::*`, `xmb::extractPup`, and `xmb::pupVersion` in `re-ps3-native`
through the Rust staticlib.
