//! Durable record of one in-place update. It is replaced atomically and
//! flushed before each phase change, so after a crash or power loss the file
//! names the last phase whose preconditions were fully on disk.
use super::super::{manifest::{invalid, SignedManifest, MANIFEST_LIMIT}, service::UpdateError};
use serde::{Deserialize, Serialize};
use std::{fs, io::{Read, Write}, path::Path};

pub const FILE: &str = "journal.json";

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum Phase {
    /// Every changed file is verified under `staged/`; the install is untouched.
    Staged,
    /// Files are being swapped. Recovery restores the previous version.
    Applying,
    /// The new version is in place and launched, but not yet proven healthy.
    Applied,
    /// The new version passed startup health. Only cleanup remains.
    Committed,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct ChangedFile {
    pub path: String,
    /// False when the target version adds a file the installed one lacks.
    pub replaces_existing: bool,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Journal {
    pub schema: u32,
    pub phase: Phase,
    pub signed: SignedManifest,
    pub changed: Vec<ChangedFile>,
    /// Activations of this version that ended without a health verdict.
    pub attempts: u32,
}

impl Journal {
    pub fn new(signed: SignedManifest, changed: Vec<ChangedFile>) -> Self {
        Self { schema: 1, phase: Phase::Staged, signed, changed, attempts: 0 }
    }
}

pub fn read(work: &Path) -> Result<Journal, UpdateError> {
    let path = work.join(FILE);
    let metadata = fs::symlink_metadata(&path)?;
    let limit = (MANIFEST_LIMIT * 2 + 64 * 1024) as u64;
    if !metadata.is_file() || super::super::manifest::is_reparse(&metadata) || metadata.len() > limit {
        return Err(invalid("Invalid update journal"));
    }
    let mut bytes = Vec::new();
    fs::File::open(&path)?.take(limit + 1).read_to_end(&mut bytes)?;
    let journal: Journal = serde_json::from_slice(&bytes)
        .map_err(|error| invalid(format!("Invalid update journal: {error}")))?;
    if journal.schema != 1 {
        return Err(invalid("Unsupported update journal"));
    }
    Ok(journal)
}

/// Writes beside the journal, flushes, then renames over it with write-through.
pub fn write(work: &Path, journal: &Journal) -> std::io::Result<()> {
    let mut temporary = tempfile::Builder::new().prefix("journal-").tempfile_in(work)?;
    temporary.write_all(&serde_json::to_vec(journal).map_err(std::io::Error::other)?)?;
    temporary.as_file().sync_all()?;
    let (_, path) = temporary.keep().map_err(|error| error.error)?;
    super::disk::rename(&path, &work.join(FILE), true).inspect_err(|_| {
        let _ = fs::remove_file(&path);
    })
}
