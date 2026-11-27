//! Safe Rust binary decoders for PS3 GTF textures, GIM pictures, VAG audio, Edge meshes,
//! and PGS/VobSub bitmap subtitles (Phases 3–4 of `audit/RUST-HARDENING.md`).
//!
//! Zero `unsafe` in all decoder modules; `unsafe` is confined to `ffi.rs`.

pub mod edge;
pub mod ffi;
pub mod gim;
pub mod gtf;
pub mod subtitles;
pub mod vag;
