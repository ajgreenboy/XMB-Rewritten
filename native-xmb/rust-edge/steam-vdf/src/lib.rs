//! Safe Rust Valve KeyValues (`.vdf` / `.acf`) parser (Phase 2 of `audit/RUST-HARDENING.md`).
//!
//! Zero `unsafe` in the parser; `unsafe` is confined to the thin C ABI boundary in `ffi.rs`.

pub mod ffi;
pub mod vdf;
