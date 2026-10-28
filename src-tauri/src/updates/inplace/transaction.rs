//! Swaps verified files into the installation and undoes the swap.
//!
//! Layout under the install directory:
//! `.update/staged/<path>` new bytes, `.update/backup/<path>` replaced bytes,
//! `.update/journal.json` the phase. Every step is a same-volume write-through
//! rename, so no file is ever copied or half-written in place, and a file's
//! location alone says how far its swap got. Recovery never rolls forward: an
//! interrupted or unproven update is always undone to the previous version.
use super::{disk::{retry_locked, Disk}, journal::{self, ChangedFile, Journal, Phase}};
use super::super::{manifest::invalid, service::UpdateError};
use std::{fs, path::{Path, PathBuf}, time::Duration};

pub const WORK: &str = ".update";
/// Activations that may end without a health verdict before a version is
/// treated as failed. One power loss mid-check must not quarantine a release.
pub const MAX_ATTEMPTS: u32 = 2;

#[derive(Clone, Debug)]
pub struct Paths { install: PathBuf }

impl Paths {
    pub fn new(install: impl Into<PathBuf>) -> Self { Self { install: install.into() } }
    pub fn install(&self) -> &Path { &self.install }
    pub fn work(&self) -> PathBuf { self.install.join(WORK) }
    pub fn installed(&self, path: &str) -> PathBuf { self.install.join(path) }
    pub fn staged(&self, path: &str) -> PathBuf { self.work().join("staged").join(path) }
    pub fn backup(&self, path: &str) -> PathBuf { self.work().join("backup").join(path) }
}

pub fn exists(path: &Path) -> bool { fs::symlink_metadata(path).is_ok() }

/// Staged -> Applying -> Applied. On any failure the previous files are
/// restored before returning, and the journal is back at `Staged`.
pub fn apply(disk: &dyn Disk, paths: &Paths, journal: &mut Journal, patience: Duration) -> Result<(), UpdateError> {
    if journal.phase != Phase::Staged {
        return Err(invalid("Only a staged update can be applied"));
    }
    for file in &journal.changed {
        if !exists(&paths.staged(&file.path)) {
            return Err(invalid("A staged update file is missing"));
        }
    }
    let attempts = journal.attempts;
    journal.phase = Phase::Applying;
    disk.write_journal(&paths.work(), journal)?;
    let swapped = (|| -> Result<(), UpdateError> {
        for file in &journal.changed {
            if let Some(parent) = paths.backup(&file.path).parent() { fs::create_dir_all(parent)?; }
            if let Some(parent) = paths.installed(&file.path).parent() { fs::create_dir_all(parent)?; }
            if file.replaces_existing {
                retry_locked(patience, || disk.rename(&paths.installed(&file.path), &paths.backup(&file.path)))?;
            }
            retry_locked(patience, || disk.rename(&paths.staged(&file.path), &paths.installed(&file.path)))?;
        }
        journal.phase = Phase::Applied;
        journal.attempts += 1;
        disk.write_journal(&paths.work(), journal)?;
        Ok(())
    })();
    if let Err(error) = swapped {
        // Nothing was launched, so this failure says nothing about the version.
        journal.phase = Phase::Applying;
        journal.attempts = attempts;
        restore(disk, paths, &journal.changed, patience)?;
        journal.phase = Phase::Staged;
        disk.write_journal(&paths.work(), journal)?;
        return Err(error);
    }
    Ok(())
}

/// Puts the previous bytes back and returns new bytes to `staged/`, so a
/// rolled-back update can be retried without downloading it again.
/// Idempotent: safe to repeat after a crash at any point, including midway.
pub fn restore(disk: &dyn Disk, paths: &Paths, changed: &[ChangedFile], patience: Duration) -> Result<(), UpdateError> {
    for file in changed.iter().rev() {
        let (installed, backup, staged) = (paths.installed(&file.path), paths.backup(&file.path), paths.staged(&file.path));
        if exists(&backup) {
            if exists(&installed) {
                if exists(&staged) {
                    retry_locked(patience, || fs::remove_file(&installed))?;
                } else {
                    if let Some(parent) = staged.parent() { fs::create_dir_all(parent)?; }
                    retry_locked(patience, || disk.rename(&installed, &staged))?;
                }
            }
            retry_locked(patience, || disk.rename(&backup, &installed))?;
        } else if !file.replaces_existing && exists(&installed) && !exists(&staged) {
            if let Some(parent) = staged.parent() { fs::create_dir_all(parent)?; }
            retry_locked(patience, || disk.rename(&installed, &staged))?;
        }
    }
    Ok(())
}

/// Applied -> Committed, then removes the backups and the work directory.
pub fn commit(disk: &dyn Disk, paths: &Paths, journal: &mut Journal) -> Result<bool, UpdateError> {
    if journal.phase != Phase::Applied {
        return Err(invalid("Only an applied update can be committed"));
    }
    journal.phase = Phase::Committed;
    disk.write_journal(&paths.work(), journal)?;
    Ok(cleanup(paths))
}

/// Removes everything but the journal first, the journal last. If a backup is
/// still in use (the supervisor runs from the replaced executable), the
/// journal stays and a later launch finishes the job. Removing the journal
/// before its backups could make recovery restore the old version.
pub fn cleanup(paths: &Paths) -> bool {
    let work = paths.work();
    let Ok(entries) = fs::read_dir(&work) else { return !exists(&work) };
    let mut complete = true;
    for entry in entries.flatten() {
        let name = entry.file_name();
        if name == journal::FILE || name == super::LOCK { continue; }
        let path = entry.path();
        let removed = if entry.file_type().is_ok_and(|kind| kind.is_dir()) { fs::remove_dir_all(&path) } else { fs::remove_file(&path) };
        complete &= removed.is_ok();
    }
    if !complete { return false; }
    let _ = fs::remove_file(work.join(journal::FILE));
    let _ = fs::remove_file(work.join(super::LOCK));
    fs::remove_dir(&work).is_ok() || !exists(&work)
}

#[derive(Debug)]
pub enum Recovered {
    /// No update in progress and nothing left on disk.
    Clean,
    /// Cleanup of a committed update is still blocked by a file in use.
    CleanupPending,
    /// A verified update is staged and may be applied.
    Pending(Journal),
    /// An interrupted or unproven activation was undone; it may be retried.
    RolledBack(Journal),
    /// The version exhausted its attempts. It was undone and discarded.
    Exhausted(Journal),
}

/// Brings the installation to a consistent state. Call only when no
/// supervisor owns the transaction.
pub fn recover(disk: &dyn Disk, paths: &Paths, patience: Duration) -> Result<Recovered, UpdateError> {
    let work = paths.work();
    if !exists(&work) { return Ok(Recovered::Clean); }
    let mut journal = match journal::read(&work) {
        Ok(journal) => journal,
        // No trustworthy phase: undo whatever backups exist, drop the rest.
        Err(_) => {
            restore(disk, paths, &backed_up(paths), patience)?;
            return Ok(if discard(paths) { Recovered::Clean } else { Recovered::CleanupPending });
        }
    };
    match journal.phase {
        Phase::Staged => Ok(Recovered::Pending(journal)),
        Phase::Committed => Ok(if cleanup(paths) { Recovered::Clean } else { Recovered::CleanupPending }),
        Phase::Applying | Phase::Applied => {
            restore(disk, paths, &journal.changed, patience)?;
            if journal.attempts >= MAX_ATTEMPTS {
                journal.phase = Phase::Staged;
                disk.write_journal(&work, &journal)?;
                discard(paths);
                return Ok(Recovered::Exhausted(journal));
            }
            journal.phase = Phase::Staged;
            disk.write_journal(&work, &journal)?;
            Ok(Recovered::RolledBack(journal))
        }
    }
}

/// Drops a staged or undone update. Never call while backups are installed.
pub fn discard(paths: &Paths) -> bool {
    if has_backups(paths) { return false; }
    let _ = fs::remove_dir_all(paths.work().join("staged"));
    cleanup(paths)
}

pub fn has_backups(paths: &Paths) -> bool { !backed_up(paths).is_empty() }

fn backed_up(paths: &Paths) -> Vec<ChangedFile> {
    super::super::manifest::PATHS.iter()
        .filter(|path| exists(&paths.backup(path)))
        .map(|path| ChangedFile { path: (*path).to_owned(), replaces_existing: true })
        .collect()
}
