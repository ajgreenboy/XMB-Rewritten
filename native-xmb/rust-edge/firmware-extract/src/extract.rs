//! Pure-Rust PUP verification, SCE decryption, ustar TAR validation, and
//! transactional `dev_flash` extraction.
//!
//! Mirrors every structural check and error class of
//! `native-xmb/src/firmware_extract.cpp` with zero `unsafe` in the parse and
//! extraction path.

use firmware_crypto::{aes_cbc, aes_ctr, hmac_sha1};
use flate2::{Decompress, FlushDecompress, Status};
use std::collections::{BTreeMap, HashSet};
use std::fs::{self, File};
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Component, Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

const MAX_PUP_BYTES: u64 = 1024 * 1024 * 1024; // 1 GiB
const MAX_SCE_SECTION_BYTES: u64 = 1024 * 1024 * 1024; // 1 GiB
const MAX_DECOMPRESSED_BYTES: usize = 1024 * 1024 * 1024; // 1 GiB
const MAX_EXTRACTED_BYTES: u64 = 2 * 1024 * 1024 * 1024; // 2 GiB

static STAGE_SEQ: AtomicU64 = AtomicU64::new(1);

#[derive(Clone, Debug)]
pub struct Keys {
    pub pup: [u8; 64],
    pub erk: [u8; 32],
    pub riv: [u8; 16],
}

impl Keys {
    pub fn from_slices(pup: &[u8], erk: &[u8], riv: &[u8]) -> Result<Self, ExtractError> {
        if pup.len() != 64 || erk.len() != 32 || riv.len() != 16 {
            return Err(ExtractError::Keys(
                "Invalid key declarations in XMB_FIRMWARE_KEYS".to_string(),
            ));
        }
        if pup.iter().all(|&b| b == 0)
            || erk.iter().all(|&b| b == 0)
            || riv.iter().all(|&b| b == 0)
        {
            return Err(ExtractError::Keys(
                "decryption keys not provided (create native-xmb/src/firmware_keys.inc)"
                    .to_string(),
            ));
        }
        let mut out = Self {
            pup: [0u8; 64],
            erk: [0u8; 32],
            riv: [0u8; 16],
        };
        out.pup.copy_from_slice(pup);
        out.erk.copy_from_slice(erk);
        out.riv.copy_from_slice(riv);
        Ok(out)
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ExtractStats {
    pub entries_verified: u64,
    pub packages: u64,
    pub bytes_written: u64,
    pub explore_rco_present: bool,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ExtractError {
    Arg(String),
    Keys(String),
    Io(String),
    Format(String),
    Path(String),
}

impl ExtractError {
    pub fn message(&self) -> &str {
        match self {
            Self::Arg(s) | Self::Keys(s) | Self::Io(s) | Self::Format(s) | Self::Path(s) => s,
        }
    }
}

fn be16(b: &[u8], at: usize) -> Option<u16> {
    let s: [u8; 2] = b.get(at..at.checked_add(2)?)?.try_into().ok()?;
    Some(u16::from_be_bytes(s))
}

fn be32(b: &[u8], at: usize) -> Option<u32> {
    let s: [u8; 4] = b.get(at..at.checked_add(4)?)?.try_into().ok()?;
    Some(u32::from_be_bytes(s))
}

fn be64(b: &[u8], at: usize) -> Option<u64> {
    let s: [u8; 8] = b.get(at..at.checked_add(8)?)?.try_into().ok()?;
    Some(u64::from_be_bytes(s))
}

fn in_range(at: u64, size: u64, total: u64) -> bool {
    at <= total && size <= total - at
}

fn slice_range(b: &[u8], at: u64, size: u64) -> Option<&[u8]> {
    if !in_range(at, size, b.len() as u64) {
        return None;
    }
    let start = usize::try_from(at).ok()?;
    let len = usize::try_from(size).ok()?;
    b.get(start..start.checked_add(len)?)
}

fn tar_field(header: &[u8], at: usize, len: usize) -> Result<String, ExtractError> {
    let raw = header
        .get(at..at + len)
        .ok_or_else(|| ExtractError::Format("Invalid TAR header slice".to_string()))?;
    let trimmed = match raw.iter().position(|&b| b == 0) {
        Some(end) => &raw[..end],
        None => raw,
    };
    std::str::from_utf8(trimmed)
        .map(|s| s.to_string())
        .map_err(|_| ExtractError::Path("Unsafe TAR path: invalid UTF-8".to_string()))
}

fn tar_octal(header: &[u8], at: usize, len: usize) -> Option<u64> {
    let raw = header.get(at..at + len)?;
    let mapped: String = raw
        .iter()
        .map(|&b| if b == 0 { ' ' } else { b as char })
        .collect();
    let s = mapped.trim();
    if s.is_empty() {
        return None;
    }
    u64::from_str_radix(s, 8).ok()
}

fn is_windows_device_alias(base_upper: &str) -> bool {
    if matches!(base_upper, "CON" | "PRN" | "AUX" | "NUL") {
        return true;
    }
    if base_upper.len() == 4 {
        let prefix = &base_upper[..3];
        let last = base_upper.as_bytes()[3];
        if (prefix == "COM" || prefix == "LPT") && last.is_ascii_digit() {
            return true;
        }
    }
    false
}

pub fn safe_tar_name(name: &str) -> bool {
    if name.is_empty()
        || name.starts_with('/')
        || name.contains('\\')
        || name.contains(':')
        || name.contains('\0')
    {
        return false;
    }
    for part in name.split('/') {
        if part == ".." || part.ends_with(' ') || part.ends_with('.') {
            if part != "." {
                return false;
            }
        }
        let base = part.split('.').next().unwrap_or("").to_ascii_uppercase();
        if is_windows_device_alias(&base) {
            return false;
        }
        for ch in part.chars() {
            if matches!(ch, '<' | '>' | '"' | '|' | '?' | '*') || (ch as u32) <= 0x1f {
                return false;
            }
        }
    }
    true
}

/// Normalize forward-slash relative path like `QDir::cleanPath` without touching the filesystem.
fn clean_rel_path(path: &str) -> String {
    let mut stack: Vec<&str> = Vec::new();
    for seg in path.split('/') {
        if seg.is_empty() || seg == "." {
            continue;
        }
        if seg == ".." {
            if let Some(last) = stack.last() {
                if *last != ".." {
                    stack.pop();
                    continue;
                }
            }
            stack.push("..");
        } else {
            stack.push(seg);
        }
    }
    if stack.is_empty() {
        ".".to_string()
    } else {
        stack.join("/")
    }
}

fn walk_tar<F>(data: &[u8], mut visit: F) -> Result<(), ExtractError>
where
    F: FnMut(&str, u8, &[u8]) -> Result<(), ExtractError>,
{
    let mut at: u64 = 0;
    let total = data.len() as u64;
    while in_range(at, 512, total) {
        let h = slice_range(data, at, 512)
            .ok_or_else(|| ExtractError::Format("Truncated TAR header".to_string()))?;
        at += 512;
        if h.iter().all(|&b| b == 0) {
            let next = slice_range(data, at, 512)
                .ok_or_else(|| ExtractError::Format("Truncated TAR terminator".to_string()))?;
            if !next.iter().all(|&b| b == 0) {
                return Err(ExtractError::Format("Truncated TAR terminator".to_string()));
            }
            return Ok(());
        }
        if &h[257..262] != b"ustar" {
            return Err(ExtractError::Format("TAR is not ustar".to_string()));
        }
        let stored = tar_octal(h, 148, 8)
            .ok_or_else(|| ExtractError::Format("TAR checksum mismatch".to_string()))?;
        let sum: u64 = h
            .iter()
            .enumerate()
            .map(|(i, &b)| if (148..156).contains(&i) { 32u64 } else { u64::from(b) })
            .sum();
        if stored != sum {
            return Err(ExtractError::Format("TAR checksum mismatch".to_string()));
        }
        let size = tar_octal(h, 124, 12)
            .ok_or_else(|| ExtractError::Format("Invalid TAR entry size".to_string()))?;
        let payload = slice_range(data, at, size)
            .ok_or_else(|| ExtractError::Format("Invalid TAR entry size".to_string()))?;
        let name_part = tar_field(h, 0, 100)?;
        let prefix_part = tar_field(h, 345, 155)?;
        let full_name = if prefix_part.is_empty() {
            name_part
        } else {
            format!("{prefix_part}/{name_part}")
        };
        if !safe_tar_name(&full_name) {
            return Err(ExtractError::Path(format!("Unsafe TAR path: {full_name}")));
        }
        let entry_type = h[156];
        if entry_type != b'0' && entry_type != 0 && entry_type != b'5' {
            return Err(ExtractError::Path(format!(
                "Unsupported TAR entry type for {full_name} (links/devices forbidden)"
            )));
        }
        visit(&full_name, entry_type, payload)?;
        let padded = size
            .checked_add(511)
            .ok_or_else(|| ExtractError::Format("Truncated TAR padding".to_string()))?
            / 512
            * 512;
        if !in_range(at, padded, total) {
            return Err(ExtractError::Format("Truncated TAR padding".to_string()));
        }
        at += padded;
    }
    Err(ExtractError::Format("Missing TAR terminator".to_string()))
}

fn inflate_section(data: &[u8]) -> Result<Vec<u8>, ExtractError> {
    if data.len() >= 2 && data[0] == 0x1f && data[1] == 0x8b {
        let mut cursor = std::io::Cursor::new(data);
        let mut gz = flate2::read::GzDecoder::new(&mut cursor);
        let mut result: Vec<u8> = Vec::new();
        let mut block = [0u8; 65536];
        loop {
            let n = gz
                .read(&mut block)
                .map_err(|_| ExtractError::Format("Invalid compressed SCE section".to_string()))?;
            if n == 0 {
                break;
            }
            if result.len().saturating_add(n) > MAX_DECOMPRESSED_BYTES {
                return Err(ExtractError::Format(
                    "SCE decompression exceeds 1 GiB limit".to_string(),
                ));
            }
            result.extend_from_slice(&block[..n]);
        }
        if gz.into_inner().position() != data.len() as u64 {
            return Err(ExtractError::Format(
                "Invalid compressed SCE section".to_string(),
            ));
        }
        return Ok(result);
    }
    let mut dec = Decompress::new(true);
    let mut result: Vec<u8> = Vec::new();
    let mut block = [0u8; 65536];
    loop {
        let in_pos = usize::try_from(dec.total_in())
            .map_err(|_| ExtractError::Format("Invalid compressed SCE section".to_string()))?;
        let before_out = dec.total_out();
        let status = dec
            .decompress(&data[in_pos..], &mut block, FlushDecompress::None)
            .map_err(|_| ExtractError::Format("Invalid compressed SCE section".to_string()))?;
        let produced = usize::try_from(dec.total_out() - before_out)
            .map_err(|_| ExtractError::Format("Invalid compressed SCE section".to_string()))?;
        if produced > 0 {
            if result.len().saturating_add(produced) > MAX_DECOMPRESSED_BYTES {
                return Err(ExtractError::Format(
                    "SCE decompression exceeds 1 GiB limit".to_string(),
                ));
            }
            result.extend_from_slice(&block[..produced]);
        }
        match status {
            Status::StreamEnd => {
                if dec.total_in() != data.len() as u64 {
                    return Err(ExtractError::Format(
                        "Invalid compressed SCE section".to_string(),
                    ));
                }
                return Ok(result);
            }
            Status::Ok => {
                if produced == 0 && usize::try_from(dec.total_in()).unwrap_or(data.len()) == data.len() {
                    return Err(ExtractError::Format(
                        "Invalid compressed SCE section".to_string(),
                    ));
                }
            }
            Status::BufError => {
                return Err(ExtractError::Format(
                    "Invalid compressed SCE section".to_string(),
                ));
            }
        }
    }
}

fn decrypt_sce(sce: &[u8], keys: &Keys) -> Result<Vec<u8>, ExtractError> {
    if sce.len() < 32 || &sce[..4] != b"SCE\0" {
        return Err(ExtractError::Format("Invalid SCE header".to_string()));
    }
    let flags = be16(sce, 8).ok_or_else(|| ExtractError::Format("Invalid SCE header".to_string()))?;
    let meta = 32u64
        + u64::from(
            be32(sce, 12).ok_or_else(|| ExtractError::Format("Invalid SCE header".to_string()))?,
        );
    let end =
        be64(sce, 16).ok_or_else(|| ExtractError::Format("Invalid SCE header".to_string()))?;
    if !in_range(meta, 96, end) || end > sce.len() as u64 {
        return Err(ExtractError::Format(
            "Invalid SCE metadata bounds".to_string(),
        ));
    }
    let mut info = slice_range(sce, meta, 64)
        .ok_or_else(|| ExtractError::Format("Invalid SCE metadata bounds".to_string()))?
        .to_vec();
    if (flags & 0x8000) == 0 && !aes_cbc(&mut info, &keys.erk, &keys.riv, false) {
        return Err(ExtractError::Format(
            "Failed to decrypt SCE metadata (wrong keys?)".to_string(),
        ));
    }
    if info[16..32] != [0u8; 16] || info[48..64] != [0u8; 16] {
        return Err(ExtractError::Format(
            "Failed to decrypt SCE metadata (wrong keys?)".to_string(),
        ));
    }
    let mut metadata = slice_range(sce, meta + 64, end - meta - 64)
        .ok_or_else(|| ExtractError::Format("Invalid SCE metadata bounds".to_string()))?
        .to_vec();
    if !aes_ctr(&mut metadata, &info[0..16], &info[32..48]) {
        return Err(ExtractError::Format(
            "Failed to decrypt SCE metadata (wrong keys?)".to_string(),
        ));
    }
    let count = be32(&metadata, 12)
        .ok_or_else(|| ExtractError::Format("Invalid SCE section/key table".to_string()))?;
    let key_count = be32(&metadata, 16)
        .ok_or_else(|| ExtractError::Format("Invalid SCE section/key table".to_string()))?;
    let section_bytes = u64::from(count).saturating_mul(48);
    let key_offset = 32u64.saturating_add(section_bytes);
    let key_bytes = u64::from(key_count).saturating_mul(16);
    if count < 3
        || !in_range(32, section_bytes, metadata.len() as u64)
        || !in_range(key_offset, key_bytes, metadata.len() as u64)
    {
        return Err(ExtractError::Format(
            "Invalid SCE section/key table".to_string(),
        ));
    }

    let mut output = Vec::new();
    for i in 0..count {
        let h = 32usize + (i as usize) * 48;
        let offset = be64(&metadata, h)
            .ok_or_else(|| ExtractError::Format("Invalid SCE section bounds/flags/keys".to_string()))?;
        let size = be64(&metadata, h + 8)
            .ok_or_else(|| ExtractError::Format("Invalid SCE section bounds/flags/keys".to_string()))?;
        let enc = be32(&metadata, h + 32)
            .ok_or_else(|| ExtractError::Format("Invalid SCE section bounds/flags/keys".to_string()))?;
        let ki = be32(&metadata, h + 36)
            .ok_or_else(|| ExtractError::Format("Invalid SCE section bounds/flags/keys".to_string()))?;
        let iv = be32(&metadata, h + 40)
            .ok_or_else(|| ExtractError::Format("Invalid SCE section bounds/flags/keys".to_string()))?;
        let comp = be32(&metadata, h + 44)
            .ok_or_else(|| ExtractError::Format("Invalid SCE section bounds/flags/keys".to_string()))?;

        if !in_range(offset, size, sce.len() as u64)
            || (size > 0 && offset < end)
            || size > MAX_SCE_SECTION_BYTES
            || (enc == 3 && (ki >= key_count || iv >= key_count))
            || (enc != 1 && enc != 3)
            || (comp != 1 && comp != 2)
        {
            return Err(ExtractError::Format(
                "Invalid SCE section bounds/flags/keys".to_string(),
            ));
        }
        if i != 2 {
            continue;
        }
        let mut section = slice_range(sce, offset, size)
            .ok_or_else(|| ExtractError::Format("Invalid SCE section bounds/flags/keys".to_string()))?
            .to_vec();
        if enc == 3 {
            let k_slice = slice_range(&metadata, key_offset + u64::from(ki) * 16, 16)
                .ok_or_else(|| ExtractError::Format("Invalid SCE section bounds/flags/keys".to_string()))?;
            let iv_slice = slice_range(&metadata, key_offset + u64::from(iv) * 16, 16)
                .ok_or_else(|| ExtractError::Format("Invalid SCE section bounds/flags/keys".to_string()))?;
            if !aes_ctr(&mut section, k_slice, iv_slice) {
                return Err(ExtractError::Format(
                    "Invalid SCE section bounds/flags/keys".to_string(),
                ));
            }
        }
        if comp == 2 {
            section = inflate_section(&section)?;
        }
        output = section;
    }
    Ok(output)
}

fn absolute_clean_path(path: &Path) -> Result<PathBuf, ExtractError> {
    let abs = if path.is_absolute() {
        path.to_path_buf()
    } else {
        std::env::current_dir()
            .map_err(|e| ExtractError::Io(format!("Cannot resolve current directory: {e}")))?
            .join(path)
    };
    let mut out = PathBuf::new();
    for comp in abs.components() {
        match comp {
            Component::Prefix(p) => out.push(p.as_os_str()),
            Component::RootDir => out.push(comp.as_os_str()),
            Component::CurDir => {}
            Component::ParentDir => {
                out.pop();
            }
            Component::Normal(n) => out.push(n),
        }
    }
    Ok(out)
}

fn is_link_or_reparse(path: &Path) -> bool {
    if let Ok(meta) = fs::symlink_metadata(path) {
        if meta.file_type().is_symlink() {
            return true;
        }
        #[cfg(windows)]
        {
            use std::os::windows::fs::MetadataExt;
            const FILE_ATTRIBUTE_REPARSE_POINT: u32 = 0x0400;
            if (meta.file_attributes() & FILE_ATTRIBUTE_REPARSE_POINT) != 0 {
                return true;
            }
        }
    }
    false
}

fn no_links(path: &Path) -> bool {
    let mut cur = Some(path);
    while let Some(p) = cur {
        if is_link_or_reparse(p) {
            return false;
        }
        let parent = p.parent();
        if parent == Some(p) {
            break;
        }
        cur = parent;
    }
    true
}

fn write_atomic_file(target: &Path, bytes: &[u8]) -> Result<(), ExtractError> {
    let parent = target
        .parent()
        .ok_or_else(|| ExtractError::Path("Unsafe/unwritable output directory".to_string()))?;
    if !no_links(parent) {
        return Err(ExtractError::Path(
            "Unsafe/unwritable output directory".to_string(),
        ));
    }
    fs::create_dir_all(parent)
        .map_err(|_| ExtractError::Io("Unsafe/unwritable output directory".to_string()))?;
    if !no_links(target) {
        return Err(ExtractError::Path(
            "Unsafe/unwritable output directory".to_string(),
        ));
    }
    let seq = STAGE_SEQ.fetch_add(1, Ordering::Relaxed);
    let file_name = target
        .file_name()
        .and_then(|s| s.to_str())
        .unwrap_or("part");
    let tmp = parent.join(format!(".{file_name}.xmb-tmp-{}-{seq}", std::process::id()));
    let write_res = (|| -> std::io::Result<()> {
        let mut f = File::create(&tmp)?;
        f.write_all(bytes)?;
        f.sync_all()?;
        Ok(())
    })();
    if let Err(e) = write_res {
        let _ = fs::remove_file(&tmp);
        return Err(ExtractError::Io(format!(
            "Cannot write {}: {e}",
            target.display()
        )));
    }
    #[cfg(windows)]
    if target.exists() {
        let _ = fs::remove_file(target);
    }
    if let Err(e) = fs::rename(&tmp, target) {
        let _ = fs::remove_file(&tmp);
        return Err(ExtractError::Io(format!(
            "Cannot write {}: {e}",
            target.display()
        )));
    }
    Ok(())
}

pub fn extract_pup(
    pup_path: &Path,
    dest_dir: &Path,
    keys: &Keys,
) -> Result<ExtractStats, ExtractError> {
    if dest_dir.as_os_str().is_empty() {
        return Err(ExtractError::Path(
            "Unsafe destination (link/reparse point)".to_string(),
        ));
    }
    let mut file = File::open(pup_path)
        .map_err(|e| ExtractError::Io(format!("Cannot open PUP: {e}")))?;
    let file_len = file
        .metadata()
        .map_err(|e| ExtractError::Io(format!("Cannot open PUP: {e}")))?
        .len();
    if file_len > MAX_PUP_BYTES {
        return Err(ExtractError::Format("PUP exceeds 1 GiB limit".to_string()));
    }
    let mut pup = Vec::with_capacity(usize::try_from(file_len).unwrap_or(0));
    file.read_to_end(&mut pup)
        .map_err(|e| ExtractError::Format(format!("Invalid PUP header/magic: {e}")))?;
    if pup.len() < 48 || &pup[..8] != b"SCEUF\0\0\0" {
        return Err(ExtractError::Format("Invalid PUP header/magic".to_string()));
    }

    let count = be64(&pup, 24)
        .ok_or_else(|| ExtractError::Format("Invalid PUP header/table lengths".to_string()))?;
    let header = be64(&pup, 32)
        .ok_or_else(|| ExtractError::Format("Invalid PUP header/table lengths".to_string()))?;
    let data = be64(&pup, 40)
        .ok_or_else(|| ExtractError::Format("Invalid PUP header/table lengths".to_string()))?;
    let total = pup.len() as u64;
    if count == 0
        || count > (total - 48) / 64
        || header < 48 + count * 64
        || !in_range(header, data, total)
    {
        return Err(ExtractError::Format(
            "Invalid PUP header/table lengths".to_string(),
        ));
    }

    let mut update: Option<Vec<u8>> = None;
    let mut seen = HashSet::new();
    for i in 0..count {
        let fat = usize::try_from(48 + i * 32)
            .map_err(|_| ExtractError::Format("Invalid PUP entry bounds/id".to_string()))?;
        let hat = usize::try_from(48 + count * 32 + i * 32)
            .map_err(|_| ExtractError::Format("Invalid PUP entry bounds/id".to_string()))?;
        let id = be64(&pup, fat)
            .ok_or_else(|| ExtractError::Format("Invalid PUP entry bounds/id".to_string()))?;
        let offset = be64(&pup, fat + 8)
            .ok_or_else(|| ExtractError::Format("Invalid PUP entry bounds/id".to_string()))?;
        let size = be64(&pup, fat + 16)
            .ok_or_else(|| ExtractError::Format("Invalid PUP entry bounds/id".to_string()))?;
        let hash = pup
            .get(hat + 8..hat + 28)
            .ok_or_else(|| ExtractError::Format("Invalid PUP entry bounds/id".to_string()))?;
        if !seen.insert(id) || offset < header || !in_range(offset, size, header + data) {
            return Err(ExtractError::Format(
                "Invalid PUP entry bounds/id".to_string(),
            ));
        }
        let contents = slice_range(&pup, offset, size)
            .ok_or_else(|| ExtractError::Format("Invalid PUP entry bounds/id".to_string()))?;
        if hmac_sha1(&keys.pup, contents) != hash {
            return Err(ExtractError::Format(format!(
                "PUP HMAC-SHA1 mismatch for entry 0x{id:x}"
            )));
        }
        if id == 0x300 {
            update = Some(contents.to_vec());
        }
    }

    let update_bytes = update
        .ok_or_else(|| ExtractError::Format("PUP update TAR entry 0x300 is missing".to_string()))?;

    let root = absolute_clean_path(dest_dir)?;
    if !no_links(&root) {
        return Err(ExtractError::Path(
            "Unsafe destination (link/reparse point)".to_string(),
        ));
    }

    let mut packages: u64 = 0;
    let mut written: u64 = 0;
    let mut pending: BTreeMap<String, Vec<u8>> = BTreeMap::new();

    walk_tar(&update_bytes, |name, entry_type, bytes| {
        let base_name = name.rsplit('/').next().unwrap_or(name);
        if entry_type == b'5' || !base_name.starts_with("dev_flash_") {
            return Ok(());
        }
        let unpacked = decrypt_sce(bytes, keys)?;
        packages += 1;
        walk_tar(&unpacked, |raw_path, inner_type, contents| {
            let mut path = clean_rel_path(raw_path);
            if path == "dev_flash" || path == "." {
                return Ok(());
            }
            if let Some(stripped) = path.strip_prefix("dev_flash/") {
                path = stripped.to_string();
            }
            if !safe_tar_name(&path) {
                return Err(ExtractError::Path("Unsafe dev_flash path".to_string()));
            }
            let target = root.join(path.replace('/', std::path::MAIN_SEPARATOR_STR));
            if !target.starts_with(&root) || !no_links(&target) {
                return Err(ExtractError::Path(format!("Unsafe output path: {path}")));
            }
            if inner_type != b'5' {
                written = written
                    .checked_add(contents.len() as u64)
                    .ok_or_else(|| ExtractError::Format("Extracted firmware exceeds 2 GiB limit".to_string()))?;
                if written > MAX_EXTRACTED_BYTES {
                    return Err(ExtractError::Format(
                        "Extracted firmware exceeds 2 GiB limit".to_string(),
                    ));
                }
                if pending.contains_key(&path) {
                    return Err(ExtractError::Format(format!(
                        "Duplicate firmware file: {path}"
                    )));
                }
                pending.insert(path, contents.to_vec());
            }
            Ok(())
        })
    })?;

    if packages == 0 || pending.is_empty() {
        return Err(ExtractError::Format(format!(
            "PUP verified ({count} entries), but no dev_flash packages/files found"
        )));
    }

    // Transactional write: when `root` does not yet exist, stage the whole tree in a
    // sibling temporary directory and rename it into place atomically; if anything
    // fails, remove the staging directory so no partial tree ever appears at `root`.
    if !root.exists() {
        if let Some(parent) = root.parent() {
            if !parent.as_os_str().is_empty() {
                fs::create_dir_all(parent)
                    .map_err(|_| ExtractError::Io("Cannot create destination".to_string()))?;
            }
        }
        let seq = STAGE_SEQ.fetch_add(1, Ordering::Relaxed);
        let root_name = root
            .file_name()
            .and_then(|s| s.to_str())
            .unwrap_or("dev_flash");
        let stage = root.with_file_name(format!(".{root_name}.rust-stage-{}-{seq}", std::process::id()));
        let stage_res = (|| -> Result<(), ExtractError> {
            fs::create_dir_all(&stage)
                .map_err(|_| ExtractError::Io("Cannot create destination".to_string()))?;
            for (rel, contents) in &pending {
                let target = stage.join(rel.replace('/', std::path::MAIN_SEPARATOR_STR));
                write_atomic_file(&target, contents)?;
            }
            fs::rename(&stage, &root)
                .map_err(|e| ExtractError::Io(format!("Cannot commit destination: {e}")))?;
            Ok(())
        })();
        if let Err(err) = stage_res {
            let _ = fs::remove_dir_all(&stage);
            return Err(err);
        }
    } else {
        fs::create_dir_all(&root)
            .map_err(|_| ExtractError::Io("Cannot create destination".to_string()))?;
        for (rel, contents) in &pending {
            let target = root.join(rel.replace('/', std::path::MAIN_SEPARATOR_STR));
            write_atomic_file(&target, contents)?;
        }
    }

    let explore_rco = root
        .join("vsh")
        .join("resource")
        .join("explore_plugin_full.rco");
    Ok(ExtractStats {
        entries_verified: count,
        packages,
        bytes_written: written,
        explore_rco_present: explore_rco.is_file(),
    })
}

pub fn pup_version(pup_path: &Path) -> Result<String, ExtractError> {
    let mut file = File::open(pup_path)
        .map_err(|e| ExtractError::Io(format!("Cannot open PUP: {e}")))?;
    let total = file
        .metadata()
        .map_err(|e| ExtractError::Io(format!("Cannot open PUP: {e}")))?
        .len();
    let mut head = [0u8; 48];
    if file.read_exact(&mut head).is_err() || &head[..8] != b"SCEUF\0\0\0" {
        return Err(ExtractError::Format("Invalid PUP header/magic".to_string()));
    }
    let count = be64(&head, 24)
        .ok_or_else(|| ExtractError::Format("Invalid PUP header/table lengths".to_string()))?;
    if count == 0 || count > 4096 || 48 + count * 64 > total {
        return Err(ExtractError::Format(
            "Invalid PUP header/table lengths".to_string(),
        ));
    }
    let table_len = usize::try_from(count * 32)
        .map_err(|_| ExtractError::Format("Invalid PUP header/table lengths".to_string()))?;
    let mut table = vec![0u8; table_len];
    if file.read_exact(&mut table).is_err() {
        return Err(ExtractError::Format("Truncated PUP file table".to_string()));
    }
    for i in 0..count {
        let base = (i as usize) * 32;
        if be64(&table, base) != Some(0x100) {
            continue;
        }
        let offset = be64(&table, base + 8)
            .ok_or_else(|| ExtractError::Format("Invalid PUP version entry".to_string()))?;
        let size = be64(&table, base + 16)
            .ok_or_else(|| ExtractError::Format("Invalid PUP version entry".to_string()))?;
        if size == 0 || size > 256 || !in_range(offset, size, total) {
            return Err(ExtractError::Format("Invalid PUP version entry".to_string()));
        }
        file.seek(SeekFrom::Start(offset))
            .map_err(|_| ExtractError::Format("Invalid PUP version entry".to_string()))?;
        let mut raw = vec![0u8; size as usize];
        file.read_exact(&mut raw)
            .map_err(|_| ExtractError::Format("Invalid PUP version entry".to_string()))?;
        let text: String = raw.iter().map(|&b| char::from(b)).collect();
        let trimmed = text.trim_start();
        let mut ver = String::new();
        for ch in trimmed.chars() {
            if ch.is_ascii_digit() || ch == '.' {
                ver.push(ch);
            } else {
                break;
            }
        }
        let valid_ver = !ver.is_empty()
            && !ver.starts_with('.')
            && !ver.ends_with('.')
            && !ver.contains("..")
            && ver.chars().any(|c| c.is_ascii_digit());
        if !valid_ver {
            return Err(ExtractError::Format(
                "PUP version entry is not a version number".to_string(),
            ));
        }
        return Ok(ver);
    }
    Err(ExtractError::Format(
        "PUP has no version entry (0x100)".to_string(),
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn safe_tar_name_blocks_traversal_and_devices() {
        assert!(safe_tar_name("vsh/resource/explore_plugin_full.rco"));
        assert!(safe_tar_name("dev_flash/vsh/normal"));
        assert!(!safe_tar_name(""));
        assert!(!safe_tar_name("/absolute"));
        assert!(!safe_tar_name("../evil"));
        assert!(!safe_tar_name("a/../evil"));
        assert!(!safe_tar_name("C:/escape"));
        assert!(!safe_tar_name("dir\\..\\evil"));
        assert!(!safe_tar_name("dir/file:stream"));
        assert!(!safe_tar_name("CON"));
        assert!(!safe_tar_name("con.txt"));
        assert!(!safe_tar_name("LPT1.bin"));
        assert!(!safe_tar_name("trailing."));
        assert!(!safe_tar_name("trailing "));
    }
}
