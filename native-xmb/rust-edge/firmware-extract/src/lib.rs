//! firmware-extract — safe Rust reimplementation of `PS3UPDAT.PUP` HMAC
//! verification, SCE `dev_flash_*` package decryption, and `ustar` TAR
//! extraction behind a narrow C ABI (`native-xmb/audit/RUST-HARDENING.md` P1).
//!
//! Re-exports `firmware_crypto` so a single Rust `staticlib` provides both the
//! `fw_hmac_sha1` / `fw_aes_*` and `fw_extract_pup` / `fw_pup_version` symbols
//! without duplicate Rust `std` symbols at link time.

pub extern crate firmware_crypto;

pub mod extract;
pub mod ffi;

pub use extract::{extract_pup, pup_version, ExtractError, ExtractStats, Keys};
