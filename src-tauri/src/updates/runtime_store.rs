use super::{
    component_download,
    manifest::{invalid, is_reparse, verify_file, Manifest, SignedManifest, MANIFEST_LIMIT},
    service::{UpdateError, UpdateGate},
};
use std::{fs, io::{Read, Write}, path::{Path, PathBuf}};

pub struct VerifiedRuntime {
    pub manifest: Manifest,
    pub signed: SignedManifest,
    pub directory: PathBuf,
    pub _locks: Vec<fs::File>,
}

pub fn root() -> Result<PathBuf, UpdateError> {
    dirs::data_local_dir().map(|path| path.join("Clipture/runtime"))
        .ok_or_else(|| invalid("LocalAppData is unavailable"))
}

pub fn validate_directory(path: &Path) -> Result<(), UpdateError> {
    for ancestor in path.ancestors() {
        let metadata = fs::symlink_metadata(ancestor)?;
        if !metadata.is_dir() || is_reparse(&metadata) {
            return Err(invalid("Runtime directory contains a reparse point"));
        }
    }
    Ok(())
}

pub fn read_signed(path: &Path) -> Result<SignedManifest, UpdateError> {
    let metadata = fs::symlink_metadata(path)?;
    if !metadata.is_file() || is_reparse(&metadata) || metadata.len() > (MANIFEST_LIMIT * 2 + 8192) as u64 {
        return Err(invalid("Invalid runtime selection file"));
    }
    let mut bytes = Vec::new();
    fs::File::open(path)?.take((MANIFEST_LIMIT * 2 + 8193) as u64).read_to_end(&mut bytes)?;
    if bytes.len() > MANIFEST_LIMIT * 2 + 8192 { return Err(invalid("Oversized runtime selection")); }
    serde_json::from_slice(&bytes).map_err(|error| invalid(format!("Invalid runtime selection: {error}")))
}

pub fn atomic_signed(root: &Path, name: &str, signed: &SignedManifest) -> Result<(), UpdateError> {
    validate_directory(root)?;
    let mut temporary = tempfile::NamedTempFile::new_in(root)?;
    temporary.write_all(&serde_json::to_vec(signed).map_err(|error| invalid(error.to_string()))?)?;
    temporary.as_file().sync_all()?;
    temporary.persist(root.join(name)).map_err(|error| UpdateError::Io(error.error))?;
    Ok(())
}

pub fn verify(root: &Path, signed: SignedManifest, key: &str) -> Result<VerifiedRuntime, UpdateError> {
    let manifest = signed.verify(key)?;
    let directory = root.join(manifest.directory_name());
    validate_directory(&directory)?;
    let mut locks = Vec::new();
    for expected in &manifest.files {
        let path = directory.join(&expected.path);
        validate_directory(path.parent().ok_or_else(|| invalid("Runtime file has no parent"))?)?;
        let mut options = fs::OpenOptions::new();
        options.read(true);
        #[cfg(windows)]
        {
            use std::os::windows::fs::OpenOptionsExt;
            options.share_mode(1);
        }
        locks.push(options.open(&path)?);
        verify_file(&path, expected)?;
    }
    Ok(VerifiedRuntime { manifest, signed, directory, _locks: locks })
}

pub fn load(root: &Path, name: &str, key: &str) -> Result<VerifiedRuntime, UpdateError> {
    validate_directory(root)?;
    verify(root, read_signed(&root.join(name))?, key)
}

/// Quarantine only the exact signed runtime that failed, not future releases.
pub fn ensure_not_failed(root: &Path, manifest: &Manifest, key: &str) -> Result<(), UpdateError> {
    let failed = read_signed(&root.join("failed.json")).ok()
        .and_then(|signed| signed.verify(key).ok());
    if failed.is_some_and(|failed| failed.directory_name() == manifest.directory_name()) {
        return Err(invalid("This update failed startup validation. Keeping the previous version; check again for a newer release."));
    }
    Ok(())
}

pub fn load_candidate(root: &Path, name: &str, key: &str) -> Result<VerifiedRuntime, UpdateError> {
    let candidate = load(root, name, key)?;
    ensure_not_failed(root, &candidate.manifest, key)?;
    Ok(candidate)
}

pub async fn stage(
    root: &Path, signed: &SignedManifest, key: &str, gate: &dyn UpdateGate,
    progress: impl FnMut(u64),
) -> Result<(), UpdateError> {
    stage_from(root, signed, key, gate, &std::env::current_exe()?.parent().ok_or_else(|| invalid("Executable has no parent"))?.to_owned(), progress).await
}

pub async fn stage_from(
    root: &Path, signed: &SignedManifest, key: &str, gate: &dyn UpdateGate,
    source: &Path, mut progress: impl FnMut(u64),
) -> Result<(), UpdateError> {
    let manifest = signed.verify(key)?;
    fs::create_dir_all(root)?;
    validate_directory(root)?;
    ensure_not_failed(root, &manifest, key)?;
    if let Ok(existing) = verify(root, signed.clone(), key) {
        atomic_signed(root, "pending.json", &existing.signed)?;
        return Ok(());
    }
    let staging = tempfile::Builder::new().prefix("staging-").tempdir_in(root)?;
    fs::create_dir(staging.path().join("assets"))?;
    let client = component_download::client()?;
    for expected in &manifest.files {
        let destination = staging.path().join(&expected.path);
        let source = source.join(&expected.path);
        if verify_file(&source, expected).is_ok() {
            fs::copy(&source, &destination)?;
            verify_file(&destination, expected)?;
            progress(expected.size);
        } else {
            component_download::download_differential(&client, expected, &source, &destination, gate, &mut progress).await?;
        }
    }
    atomic_signed(staging.path(), "manifest.json", signed)?;
    let destination = root.join(manifest.directory_name());
    if destination.exists() {
        return Err(invalid("An invalid immutable runtime already occupies this version; refusing overwrite"));
    }
    fs::rename(staging.path(), &destination)?;
    verify(root, signed.clone(), key)?;
    atomic_signed(root, "pending.json", signed)
}

pub fn select(root: &Path, candidate: &VerifiedRuntime, key: &str) -> Result<(), UpdateError> {
    if let Ok(active) = load(root, "active.json", key) {
        if active.manifest.version != candidate.manifest.version {
            atomic_signed(root, "previous.json", &active.signed)?;
        }
    }
    atomic_signed(root, "active.json", &candidate.signed)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::updates::service::AllowUpdates;

    #[test]
    fn atomic_selection_preserves_complete_signed_envelope() {
        let root = tempfile::tempdir().unwrap();
        let signed = SignedManifest { manifest: "test".into(), signature: "signature".into() };
        atomic_signed(root.path(), "pending.json", &signed).unwrap();
        assert_eq!(read_signed(&root.path().join("pending.json")).unwrap().manifest, "test");
        assert!(load(root.path(), "pending.json", "invalid").is_err());
        assert!(!root.path().join("active.json").exists());
    }

    #[test]
    fn quarantine_requires_valid_signature_and_exact_runtime_identity() {
        let server = crate::updates::component_download::tests::TestServer::start();
        let (key, signed) = crate::updates::component_download::tests::signed_fixture(&server, "9.9.9");
        let root = tempfile::tempdir().unwrap();
        let manifest = signed.verify(&key).unwrap();
        ensure_not_failed(root.path(), &manifest, &key).unwrap();
        atomic_signed(root.path(), "failed.json", &signed).unwrap();
        assert!(ensure_not_failed(root.path(), &manifest, &key).is_err());
        let mut newer = manifest.clone();
        newer.version = "9.9.10".into();
        ensure_not_failed(root.path(), &newer, &key).unwrap();
        let mut tampered = signed.clone();
        tampered.manifest.push(' ');
        atomic_signed(root.path(), "failed.json", &tampered).unwrap();
        ensure_not_failed(root.path(), &manifest, &key).unwrap();
    }

    #[test]
    fn failed_download_preserves_selections_and_removes_partial_stage() {
        let server = crate::updates::component_download::tests::TestServer::start();
        let (key, signed) = crate::updates::component_download::tests::signed_fixture(&server, "9.9.9");
        server.serve("v9.9.9/clipture_engine.exe", b"bad!", 1);
        let root = tempfile::tempdir().unwrap();
        let source = tempfile::tempdir().unwrap();
        // Selection bytes must remain untouched even after one file was fetched.
        let previous = SignedManifest { manifest: "previous selection".into(), signature: "fixture".into() };
        atomic_signed(root.path(), "active.json", &previous).unwrap();
        atomic_signed(root.path(), "pending.json", &previous).unwrap();
        let result = tauri::async_runtime::block_on(stage_from(
            root.path(), &signed, &key, &AllowUpdates, source.path(), |_| {},
        ));
        assert!(result.is_err());
        for name in ["active.json", "pending.json"] {
            assert_eq!(read_signed(&root.path().join(name)).unwrap().manifest, previous.manifest);
        }
        assert_eq!(fs::read_dir(root.path()).unwrap().count(), 2);
    }

    #[test]
    fn signed_release_fully_stages_and_selects_a_verified_runtime() {
        let server = crate::updates::component_download::tests::TestServer::start();
        let (key, signed) = crate::updates::component_download::tests::signed_fixture(&server, "9.9.9");
        let client = crate::updates::component_download::client().unwrap();
        tauri::async_runtime::block_on(async {
        let (fetched_signed, manifest) = crate::updates::component_download::fetch_manifest(
            &client,
            &server.serve("v9.9.9/components-v1.json", signed.manifest.as_bytes().to_vec(), 1),
            &server.serve("v9.9.9/components-v1.json.sig", signed.signature.as_bytes().to_vec(), 1),
            &key, "1.5.4",
        ).await.unwrap();
        assert_eq!(manifest.version, "9.9.9");
        assert_eq!(fetched_signed.manifest, signed.manifest);
        let root = tempfile::tempdir().unwrap();
        let source = tempfile::tempdir().unwrap();
        let mut downloaded = 0;
        stage_from(
            root.path(), &fetched_signed, &key, &AllowUpdates, source.path(), |size| downloaded += size,
        ).await.unwrap();
        assert_eq!(downloaded, 20);
        let pending = load(root.path(), "pending.json", &key).unwrap();
        assert_eq!(pending.manifest.version, "9.9.9");
        select(root.path(), &pending, &key).unwrap();
        let active = load(root.path(), "active.json", &key).unwrap();
        assert_eq!(active.manifest.version, "9.9.9");
        atomic_signed(root.path(), "failed.json", &signed).unwrap();
        assert!(load_candidate(root.path(), "pending.json", &key).is_err());
        assert!(stage_from(root.path(), &signed, &key, &AllowUpdates, source.path(), |_| {}).await.is_err());
        let directory = active.directory.clone();
        drop(active);
        drop(pending);
        std::fs::write(directory.join("clipture.exe"), b"Test").unwrap();
        assert!(load(root.path(), "active.json", &key).is_err());
        });
    }
}
