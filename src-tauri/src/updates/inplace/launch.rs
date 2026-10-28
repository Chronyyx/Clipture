//! What each launch does with in-place update state.
use super::{
    installation::{self, EXECUTABLE},
    journal::{self, Phase},
    stage, supervisor,
    transaction::{self, Paths, Recovered},
    RealDisk, LOCK,
};
use super::super::{activation, manifest::verify_file, runtime_store, service::{UpdateError, UpdateGate}};
use std::{path::PathBuf, sync::Mutex, time::Duration};

const RECOVERY_PATIENCE: Duration = Duration::from_secs(10);
static UI_LOCKS: Mutex<Vec<std::fs::File>> = Mutex::new(Vec::new());

/// Finishes or undoes an interrupted transaction before anything else runs.
/// Costs one directory probe when no update is in flight. Returns the
/// executable to restart when this process was started from a controller that
/// recovery has just rolled back: it would otherwise keep running the new
/// version's code over the old version's files.
pub fn recover() -> Option<PathBuf> {
    let paths = installation::candidate()?;
    if !transaction::exists(&paths.work()) || supervisor_alive(&paths) || !installation::writable(paths.install()) { return None; }
    let installed = paths.installed(EXECUTABLE);
    let running_installed = std::env::current_exe().ok().zip(installed.canonicalize().ok())
        .is_some_and(|(exe, installed)| exe.canonicalize().map_or(exe == installed, |exe| exe == installed));
    let restart = |journal: &super::journal::Journal| {
        (running_installed && journal.changed.iter().any(|file| file.path == EXECUTABLE)).then(|| installed.clone())
    };
    match transaction::recover(&RealDisk, &paths, RECOVERY_PATIENCE) {
        Ok(Recovered::Exhausted(journal)) => {
            tracing::error!("An update failed to prove itself twice; it was undone and quarantined");
            if let Err(error) = supervisor::quarantine_root()
                .and_then(|root| runtime_store::atomic_signed(&root, "failed.json", &journal.signed)) {
                tracing::error!(%error, "Could not quarantine the failed version");
            }
            restart(&journal)
        }
        Ok(Recovered::RolledBack(journal)) => {
            tracing::warn!(attempts = journal.attempts, "An interrupted update was undone; it will be retried");
            restart(&journal)
        }
        // A stage that can never apply (quarantined, not newer, bad
        // signature) would otherwise stay on disk forever.
        Ok(Recovered::Pending(_)) if staged_upgrade(&paths).is_none() => {
            transaction::discard(&paths);
            None
        }
        Ok(Recovered::Pending(journal)) => {
            tracing::info!(files = journal.changed.len(), "A verified update is staged");
            None
        }
        Ok(_) => None,
        Err(error) => {
            tracing::error!(%error, "Could not recover an interrupted update");
            None
        }
    }
}

/// The supervisor holds the lock exclusively for the whole activation.
fn supervisor_alive(paths: &Paths) -> bool {
    let path = paths.work().join(LOCK);
    if !path.exists() { return false; }
    let mut options = std::fs::OpenOptions::new();
    options.read(true);
    #[cfg(windows)]
    std::os::windows::fs::OpenOptionsExt::share_mode(&mut options, 0);
    options.open(path).is_err_and(|error| super::disk::is_locked(&error))
}

/// Starts a staged update at launch, before the engine or UI exist.
/// `None`: no in-place installation, use the runtime store.
/// `Some(false)`: in-place installation with nothing to apply.
pub fn activate_staged() -> Result<Option<bool>, UpdateError> {
    let Some(paths) = installation::managed() else { return Ok(None) };
    if let Some(manifest) = staged_upgrade(&paths) {
        tracing::info!(version = %manifest.version, "Applying staged update at launch");
        supervisor::prepare(&paths)?.commit()?;
        return Ok(Some(true));
    }
    let exe = std::env::current_exe()?.canonicalize()?;
    Ok((!installation::in_runtime_store(&exe)).then_some(false))
}

/// A staged, verified, unquarantined update newer than this build.
pub fn staged_upgrade(paths: &Paths) -> Option<super::super::manifest::Manifest> {
    let journal = journal::read(&paths.work()).ok().filter(|journal| journal.phase == Phase::Staged)?;
    let key = activation::public_key();
    let manifest = journal.signed.verify(&key).ok()?;
    let root = runtime_store::root().ok()?;
    (manifest.newer_than(env!("CARGO_PKG_VERSION")) && runtime_store::ensure_not_failed(&root, &manifest, &key).is_ok())
        .then_some(manifest)
}

/// True when this process is the version a live supervisor just swapped in.
pub fn accepts_handoff() -> bool {
    let Some(paths) = installation::managed() else { return false };
    let same_exe = std::env::current_exe().ok().and_then(|exe| exe.canonicalize().ok())
        .zip(paths.installed(EXECUTABLE).canonicalize().ok())
        .is_some_and(|(exe, installed)| exe == installed);
    same_exe && journal::read(&paths.work()).ok()
        .filter(|journal| journal.phase == Phase::Applied)
        .and_then(|journal| journal.signed.verify(&activation::public_key()).ok())
        .is_some_and(|manifest| manifest.version == env!("CARGO_PKG_VERSION"))
}

/// After a healthy handoff, removes backups once the supervisor has exited.
pub fn finish_after_handoff() {
    let Some(paths) = installation::managed() else { return };
    std::thread::spawn(move || {
        for _ in 0..180 {
            std::thread::sleep(Duration::from_secs(1));
            if !transaction::exists(&paths.work()) { return; }
            if supervisor_alive(&paths) { continue; }
            let done = match journal::read(&paths.work()) {
                Ok(journal) if journal.phase == Phase::Committed => transaction::cleanup(&paths),
                // An uncommitted phase is the next launch's recovery to judge.
                Ok(_) => true,
                Err(_) => transaction::has_backups(&paths) || transaction::cleanup(&paths),
            };
            if done { return; }
        }
    });
}

/// A staged controller that may serve as the disposable UI without
/// replacing the engine: only `clipture.exe` changes and the native
/// identity matches. The staged file stays locked while it is offered.
pub fn ui_executable(native_id: &str) -> Option<PathBuf> {
    let paths = installation::managed()?;
    let manifest = staged_upgrade(&paths)?;
    let journal = journal::read(&paths.work()).ok()?;
    if manifest.compatibility.host != native_id
        || journal.changed.iter().any(|file| file.path != EXECUTABLE)
        || journal.changed.is_empty() {
        return None;
    }
    let locks = stage::lock_staged(&paths, &manifest, &journal).ok()?;
    *UI_LOCKS.lock().unwrap_or_else(|error| error.into_inner()) = locks;
    Some(paths.staged(EXECUTABLE))
}

/// A runtime-store process copies its verified files over a writable
/// per-user base install, without restarting: those bytes already run
/// healthy, and the base files are not executing. Later launches start the
/// base directly. Returns whether anything changed.
pub async fn consolidate(gate: &dyn UpdateGate) -> Result<bool, UpdateError> {
    let Some(runtime) = activation::running_runtime() else { return Ok(false) };
    let Some(paths) = installation::managed() else { return Ok(false) };
    let key = activation::public_key();
    let manifest = runtime.signed.verify(&key)?;
    if manifest.files.iter().all(|file| verify_file(&paths.installed(&file.path), file).is_ok()) {
        return Ok(false);
    }
    if journal::read(&paths.work()).is_ok_and(|journal| journal.phase != Phase::Staged) {
        return Ok(false);
    }
    let mut journal = stage::stage(&paths, &runtime.signed, &key, gate, Some(&runtime.directory), |_| {}).await?;
    let _lock = stage::lock_staged(&paths, &manifest, &journal)?;
    transaction::apply(&RealDisk, &paths, &mut journal, RECOVERY_PATIENCE)?;
    transaction::commit(&RealDisk, &paths, &mut journal)?;
    tracing::info!(version = %manifest.version, install = %paths.install().display(), "Consolidated runtime into the installation");
    Ok(true)
}

/// Deletes runtime-store versions once the installation updates in place,
/// keeping only the quarantine marker. Directories still in use stay.
pub fn retire_runtime_store() {
    let Some(_) = installation::managed() else { return };
    let Ok(exe) = std::env::current_exe().and_then(|exe| exe.canonicalize()) else { return };
    if installation::in_runtime_store(&exe) || installation::machine_leftover().is_some() { return; }
    let Ok(root) = runtime_store::root() else { return };
    let Ok(entries) = std::fs::read_dir(&root) else { return };
    let mut freed = 0;
    for entry in entries.flatten() {
        if entry.file_name() == "failed.json" { continue; }
        let path = entry.path();
        let removed = if entry.file_type().is_ok_and(|kind| kind.is_dir()) { std::fs::remove_dir_all(&path) } else { std::fs::remove_file(&path) };
        if removed.is_ok() { freed += 1; }
    }
    if freed > 0 { tracing::info!(freed, "Retired runtime-store entries"); }
}
