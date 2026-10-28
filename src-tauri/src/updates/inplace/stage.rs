//! Prepares an in-place update while Clipture keeps running: only files whose
//! bytes differ are staged, each is verified against the signed manifest, and
//! the journal is written last, so an interrupted stage is just garbage.
use super::{journal::{self, ChangedFile, Journal, Phase}, transaction::{self, exists, Paths}};
use super::super::{
    component_download,
    manifest::{invalid, verify_file, Manifest, SignedManifest},
    runtime_store::validate_directory,
    service::{UpdateError, UpdateGate},
};
use std::{fs, path::Path};

/// Stages `signed`, copying files that already exist verified under `local`
/// and downloading the rest, reusing matching blocks of the installed copy.
pub async fn stage(
    paths: &Paths, signed: &SignedManifest, key: &str, gate: &dyn UpdateGate,
    local: Option<&Path>, mut progress: impl FnMut(u64),
) -> Result<Journal, UpdateError> {
    let manifest = signed.verify(key)?;
    let work = paths.work();
    fs::create_dir_all(&work)?;
    validate_directory(&work)?;
    match journal::read(&work) {
        Ok(existing) if existing.phase != Phase::Staged => return Err(invalid("Another update is still being applied")),
        Ok(existing) if existing.signed.manifest == signed.manifest && lock_staged(paths, &manifest, &existing).is_ok() => {
            progress(manifest.files.iter().map(|file| file.size).sum());
            return Ok(existing);
        }
        _ => {}
    }
    // Clear a superseded stage but keep the directory: deleting and at once
    // recreating it can fail while Windows still holds the delete pending.
    if transaction::has_backups(paths) {
        return Err(invalid("A previous update could not be cleaned up yet"));
    }
    match fs::remove_dir_all(work.join("staged")) {
        Err(error) if error.kind() != std::io::ErrorKind::NotFound => return Err(error.into()),
        _ => {}
    }
    match fs::remove_file(work.join(journal::FILE)) {
        Err(error) if error.kind() != std::io::ErrorKind::NotFound => return Err(error.into()),
        _ => {}
    }
    let mut client = None;
    // Fetched once, only if something must come from the network.
    let mut patches: Option<Option<super::super::patches::PatchList>> = None;
    let mut changed = Vec::new();
    for expected in &manifest.files {
        let installed = paths.installed(&expected.path);
        if verify_file(&installed, expected).is_ok() {
            progress(expected.size);
            continue;
        }
        let staged = paths.staged(&expected.path);
        fs::create_dir_all(staged.parent().ok_or_else(|| invalid("Staged file has no parent"))?)?;
        match local.map(|directory| directory.join(&expected.path)).filter(|source| verify_file(source, expected).is_ok()) {
            Some(source) => {
                fs::copy(&source, &staged)?;
                // FlushFileBuffers needs write access on Windows.
                fs::OpenOptions::new().write(true).open(&staged)?.sync_all()?;
                verify_file(&staged, expected)?;
                progress(expected.size);
            }
            None => {
                if client.is_none() { client = Some(component_download::client()?); }
                let client = client.as_ref().expect("client was just created");
                if patches.is_none() { patches = Some(super::super::patches::fetch(client, &manifest, key).await); }
                let patch = patches.as_ref().and_then(Option::as_ref).and_then(|list| list.find(expected, &installed));
                let patched = match patch {
                    Some(patch) => super::super::patches::apply(client, patch, &installed, expected, &staged, gate).await
                        .inspect_err(|error| tracing::warn!(%error, path = %expected.path, "Patch failed; downloading instead")),
                    None => Err(invalid("No patch from the installed file")),
                };
                if patched.is_ok() {
                    progress(expected.size);
                } else {
                    component_download::download_differential(client, expected, &installed, &staged, gate, &mut progress).await?;
                }
            }
        }
        changed.push(ChangedFile { path: expected.path.clone(), replaces_existing: exists(&installed) });
    }
    let journal = Journal::new(signed.clone(), changed);
    journal::write(&work, &journal)?;
    Ok(journal)
}

/// Verifies the complete target set and keeps every file open so it can be
/// renamed but not rewritten until the handles drop. Staged files must match
/// the manifest; untouched installed files must still match it too.
pub fn lock_staged(paths: &Paths, manifest: &Manifest, journal: &Journal) -> Result<Vec<fs::File>, UpdateError> {
    let mut locks = Vec::new();
    for expected in &manifest.files {
        let changed = journal.changed.iter().any(|file| file.path == expected.path);
        let path = if changed { paths.staged(&expected.path) } else { paths.installed(&expected.path) };
        locks.push(open_locked(&path)?);
        verify_file(&path, expected)?;
    }
    if journal.changed.iter().any(|file| !manifest.files.iter().any(|expected| expected.path == file.path)) {
        return Err(invalid("Journal names a file outside the signed manifest"));
    }
    Ok(locks)
}

/// Read access; others may read or rename, nobody may write.
fn open_locked(path: &Path) -> std::io::Result<fs::File> {
    let mut options = fs::OpenOptions::new();
    options.read(true);
    #[cfg(windows)]
    {
        use std::os::windows::fs::OpenOptionsExt;
        options.share_mode(0x1 | 0x4);
    }
    options.open(path)
}
