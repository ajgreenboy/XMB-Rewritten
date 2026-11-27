//! Safe Rust binary decoders for PS3 GTF textures, Edge meshes, and PGS/VobSub bitmap subtitles
//! (Phase 3 of `audit/RUST-HARDENING.md`).
//!
//! Zero `unsafe` in all decoder modules; `unsafe` is confined to `ffi.rs`.

pub mod edge;
pub mod ffi;
pub mod gtf;
pub mod subtitles;
