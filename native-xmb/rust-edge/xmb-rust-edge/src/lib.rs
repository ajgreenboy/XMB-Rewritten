//! Unified C ABI `staticlib` re-exporting all Rust untrusted-edge crates (`audit/RUST-HARDENING.md` Phases 1–3).

pub extern crate firmware_crypto;
pub extern crate firmware_extract;
pub extern crate image_decode;
pub extern crate steam_vdf;
