//! firmware-crypto — byte-for-byte parity Rust reimplementation of the native
//! XMB firmware crypto primitives (HMAC-SHA1, AES-128-CTR, AES-128/256-CBC),
//! exposed over a C ABI as a `staticlib`.
//!
//! * `crypto` — the pure Rust functions (parity targets, directly unit-tested).
//! * `ffi` — the `#[no_mangle] extern "C"` boundary, kept separate so the
//!   parity tests target the pure functions only.

pub mod crypto;
pub mod ffi;

pub use crypto::{aes_cbc, aes_ctr, hmac_sha1};
