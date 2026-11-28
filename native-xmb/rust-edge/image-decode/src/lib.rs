//! Safe Rust binary decoders for PS3 GTF/DDS/TGA textures, GIM pictures, VAG audio, QRC/CXML/RCO containers,
//! PARAM.SFO metadata, Edge meshes, and PGS/VobSub bitmap subtitles (Phases 3–9 of `audit/RUST-HARDENING.md`).
//!
//! Zero `unsafe` in all decoder modules; `unsafe` is confined to `ffi.rs`.

pub mod cxml;
pub mod dds;
pub mod edge;
pub mod ffi;
pub mod gim;
pub mod gtf;
pub mod qrc;
pub mod rco;
pub mod sfo;
pub mod subtitles;
pub mod tga;
pub mod vag;
