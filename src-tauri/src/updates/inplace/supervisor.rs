//! Owns one in-place activation. Everything slow happens while the old
//! version still runs: the supervisor verifies and locks the stage first.
//! After the old controller exits it only renames files, starts the new
//! version from the same path and commits once that version reports health.
use super::{installation, journal::{self, Phase}, stage, transaction::{self, Paths}, RealDisk, LOCK};
use super::super::{
    activation::{self, HandoffChild},
    handoff_process::OwnedProcess,
    manifest::invalid,
    runtime_store,
    service::UpdateError,
};
use std::{path::PathBuf, process::Command, time::{Duration, Instant}};

pub const FLAG: &str = "--inplace-supervisor";
/// How long a locked file may delay the swap before the update is undone.
const SWAP_PATIENCE: Duration = Duration::from_secs(15);
const NO_WINDOW: u32 = 0x0800_0000;

/// Starts a supervisor that has already verified the staged update.
pub fn prepare(paths: &Paths) -> Result<HandoffChild, UpdateError> {
    activation::unelevated()?;
    let mut command = Command::new(std::env::current_exe()?);
    command.arg(FLAG).arg(std::process::id().to_string()).arg(paths.install());
    forward_arguments(&mut command, &std::env::args().collect::<Vec<_>>());
    #[cfg(windows)]
    { use std::os::windows::process::CommandExt; command.creation_flags(NO_WINDOW); }
    let mut child = OwnedProcess::spawn(&mut command)?;
    // Hashing the target set happens here, before anything stops.
    child.expect(b"prepared\n", Duration::from_secs(120))?;
    Ok(HandoffChild(child))
}

fn forward_arguments(command: &mut Command, args: &[String]) {
    if args.iter().any(|arg| matches!(arg.as_str(), "--hidden" | "--background")) { command.arg("--hidden"); }
    if let Some(invite) = activation::invite_link_argument(args) { command.arg(invite); }
}

#[cfg(windows)]
pub fn run_if_requested() -> Result<bool, UpdateError> {
    use std::os::windows::{fs::OpenOptionsExt, process::CommandExt};
    let args: Vec<String> = std::env::args().collect();
    let Some(index) = args.iter().position(|arg| arg == FLAG) else { return Ok(false) };
    if !activation::enabled() { return Err(invalid("In-place supervisor is disabled in this build")); }
    activation::unelevated()?;
    let parent = args.get(index + 1).and_then(|value| value.parse().ok()).ok_or_else(|| invalid("Missing handoff parent"))?;
    let parent = super::super::handoff_process::ParentProcess::open(parent)?;
    let install = PathBuf::from(args.get(index + 2).ok_or_else(|| invalid("Missing installation"))?);
    // Only the installation this build would update on its own.
    let managed = installation::managed().ok_or_else(|| invalid("No user-writable installation"))?;
    if managed.install().canonicalize()? != install.canonicalize()? {
        return Err(invalid("Supervisor target is not this installation"));
    }
    let paths = managed;
    let lock = std::fs::OpenOptions::new().write(true).create(true).truncate(false).share_mode(0).open(paths.work().join(LOCK))?;
    let key = activation::public_key();
    let mut journal = journal::read(&paths.work())?;
    if journal.phase != Phase::Staged { return Err(invalid("No staged update to apply")); }
    let manifest = journal.signed.verify(&key)?;
    runtime_store::ensure_not_failed(&runtime_store::root()?, &manifest, &key)?;
    if !manifest.newer_than(env!("CARGO_PKG_VERSION")) {
        return Err(invalid("In-place activation must advance the installed version"));
    }
    let locks = stage::lock_staged(&paths, &manifest, &journal)?;
    activation::receive_handoff()?;
    {
        use std::io::Write;
        let mut output = super::super::handoff_process::output()?;
        output.write_all(b"committed\n")?;
        output.flush()?;
    }
    parent.wait(Duration::from_secs(60))?;

    let started = Instant::now();
    if let Err(error) = transaction::apply(&RealDisk, &paths, &mut journal, SWAP_PATIENCE) {
        // `apply` restored the previous files and kept the stage for a later
        // launch. If even that failed, the relaunched process recovers first,
        // once this supervisor has released the lock.
        tracing::error!(%error, "In-place swap failed; restarting the previous version");
        drop(locks);
        drop(lock);
        return relaunch(&paths, &args, "--runtime-recovery").map(|_| true);
    }
    let swapped = started.elapsed();
    drop(locks);

    let mut command = Command::new(paths.installed(installation::EXECUTABLE));
    command.arg(activation::HANDOFF).current_dir(paths.install()).creation_flags(NO_WINDOW);
    forward_arguments(&mut command, &args);
    let result = (|| {
        let mut child = OwnedProcess::spawn(&mut command)?;
        child.expect(b"prepared\n", Duration::from_secs(30))?;
        child.send(b"activate\n")?;
        child.expect(b"healthy\n", Duration::from_secs(60))?;
        let healthy = started.elapsed();
        child.healthy_for(Duration::from_secs(5))?;
        Ok::<_, UpdateError>((child, healthy))
    })();
    match result {
        Ok((child, healthy)) => {
            // Release first: an unreleased child is killed when dropped, and a
            // failed journal write must not stop a healthy version. An
            // uncommitted journal only makes the next launch retry once.
            child.release()?;
            if let Err(error) = transaction::commit(&RealDisk, &paths, &mut journal) {
                tracing::error!(%error, "Healthy update could not be committed; the next launch will retry it");
            }
            tracing::info!(version = %manifest.version, swap_ms = swapped.as_millis() as u64,
                healthy_ms = healthy.as_millis() as u64, "In-place update committed");
        }
        Err(error) => {
            tracing::error!(%error, "Updated version failed startup health; restoring the previous version");
            match transaction::restore(&RealDisk, &paths, &journal.changed, SWAP_PATIENCE) {
                Ok(()) => {
                    journal.phase = Phase::Staged;
                    if journal::write(&paths.work(), &journal).is_ok() { transaction::discard(&paths); }
                    if let Err(marker) = quarantine_root().and_then(|root| runtime_store::atomic_signed(&root, "failed.json", &journal.signed)) {
                        tracing::error!(%marker, "Could not quarantine the failed version");
                    }
                }
                // The relaunched process recovers before it starts anything,
                // once this supervisor no longer holds the lock.
                Err(restore) => tracing::error!(%restore, "Restore incomplete; leaving it to launch recovery"),
            }
            drop(lock);
            relaunch(&paths, &args, "--runtime-recovery")?;
        }
    }
    Ok(true)
}

#[cfg(not(windows))]
pub fn run_if_requested() -> Result<bool, UpdateError> { Ok(false) }

/// The quarantine marker outlives the work directory, beside the runtime store.
pub fn quarantine_root() -> Result<PathBuf, UpdateError> {
    let root = runtime_store::root()?;
    std::fs::create_dir_all(&root)?;
    Ok(root)
}

fn relaunch(paths: &Paths, args: &[String], flag: &str) -> Result<(), UpdateError> {
    let mut command = Command::new(paths.installed(installation::EXECUTABLE));
    command.arg(flag).current_dir(paths.install());
    forward_arguments(&mut command, args);
    #[cfg(windows)]
    { use std::os::windows::process::CommandExt; command.creation_flags(NO_WINDOW); }
    command.spawn()?;
    Ok(())
}
