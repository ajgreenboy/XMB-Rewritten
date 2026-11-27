//! Safe Rust binary decoders for PS3 GTF textures, GIM pictures, VAG audio, QRC/CXML containers,
//! Edge meshes, and PGS/VobSub bitmap subtitles (Phases 3–6 of `audit/RUST-HARDENING.md`).
//!
//! Zero `unsafe` in all decoder modules; `unsafe` is confined to `ffi.rs`.

pub mod cxml;
pub mod edge;
pub mod ffi;
pub mod gim;
pub mod gtf;
pub mod qrc;
pub mod sfo;
pub mod subtitles;
pub mod vag;
