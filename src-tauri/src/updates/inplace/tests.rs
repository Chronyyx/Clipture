//! The two promises of an in-place update: an interruption at any step ends in
//! exactly the previous or exactly the new installation, and no file is ever
//! left on disk twice once the transaction is finished.
use super::{
    disk::{Disk, RealDisk},
    journal::{self, ChangedFile, Journal, Phase},
    stage,
    transaction::{self, Paths, Recovered},
};
use crate::updates::{manifest::{SignedManifest, PATHS}, service::AllowUpdates};
use std::{cell::{Cell, RefCell}, collections::{BTreeMap, HashMap}, fs, io, path::{Path, PathBuf}, time::Duration};

const CHANGED: [&str; 3] = ["clipture.exe", "clipture_engine.exe", "assets/default.mp3"];
const PATIENCE: Duration = Duration::from_millis(300);

fn old_bytes(path: &str) -> Vec<u8> { format!("old {path}").into_bytes() }
fn new_bytes(path: &str) -> Vec<u8> { format!("new {path} with more bytes").into_bytes() }

/// Installed old version plus a staged update of `CHANGED`.
fn fixture() -> (tempfile::TempDir, Paths, Journal) {
    let root = tempfile::tempdir().unwrap();
    let paths = Paths::new(root.path().join("Clipture"));
    for path in PATHS {
        write(&paths.installed(path), &old_bytes(path));
    }
    for path in CHANGED {
        write(&paths.staged(path), &new_bytes(path));
    }
    let journal = Journal::new(
        SignedManifest { manifest: "fixture".into(), signature: "fixture".into() },
        CHANGED.iter().map(|path| ChangedFile { path: (*path).into(), replaces_existing: true }).collect(),
    );
    journal::write(&paths.work(), &journal).unwrap();
    (root, paths, journal)
}

fn write(path: &Path, bytes: &[u8]) {
    fs::create_dir_all(path.parent().unwrap()).unwrap();
    fs::write(path, bytes).unwrap();
}

/// Every file under `directory`, by relative path.
fn snapshot(directory: &Path) -> BTreeMap<String, Vec<u8>> {
    let mut files = BTreeMap::new();
    let mut pending = vec![directory.to_path_buf()];
    while let Some(next) = pending.pop() {
        for entry in fs::read_dir(&next).into_iter().flatten().flatten() {
            if entry.file_type().unwrap().is_dir() {
                pending.push(entry.path());
            } else {
                let relative = entry.path().strip_prefix(directory).unwrap().to_string_lossy().replace('\\', "/");
                files.insert(relative, fs::read(entry.path()).unwrap());
            }
        }
    }
    files
}

fn old_install() -> BTreeMap<String, Vec<u8>> {
    PATHS.iter().map(|path| ((*path).to_owned(), old_bytes(path))).collect()
}

fn new_install() -> BTreeMap<String, Vec<u8>> {
    PATHS.iter().map(|path| {
        let bytes = if CHANGED.contains(path) { new_bytes(path) } else { old_bytes(path) };
        ((*path).to_owned(), bytes)
    }).collect()
}

/// The whole install directory holds exactly one installation: no backup,
/// staged copy, journal or temporary file survives.
fn assert_single_installation(paths: &Paths, expected: &BTreeMap<String, Vec<u8>>) {
    assert_eq!(&snapshot(paths.install()), expected);
    assert!(!paths.work().exists(), "work directory must be gone");
}

/// A disk that loses power at operation `crash_at`: that operation and every
/// later one fail without touching the filesystem. `locked` makes renames of
/// a path fail with a sharing violation that many times first.
#[derive(Default)]
struct FaultyDisk {
    crash_at: Option<usize>,
    operations: Cell<usize>,
    crashed: Cell<bool>,
    locked: RefCell<HashMap<PathBuf, usize>>,
}

impl FaultyDisk {
    fn crashing_at(step: usize) -> Self { Self { crash_at: Some(step), ..Self::default() } }
    fn locking(path: PathBuf, times: usize) -> Self {
        let disk = Self::default();
        disk.locked.borrow_mut().insert(path, times);
        disk
    }
    fn step(&self) -> io::Result<()> {
        if self.crashed.get() { return Err(io::Error::other("power lost")); }
        self.operations.set(self.operations.get() + 1);
        if Some(self.operations.get()) == self.crash_at {
            self.crashed.set(true);
            return Err(io::Error::other("power lost"));
        }
        Ok(())
    }
}

impl Disk for FaultyDisk {
    fn rename(&self, from: &Path, to: &Path) -> io::Result<()> {
        self.step()?;
        if let Some(remaining) = self.locked.borrow_mut().get_mut(from) {
            if *remaining > 0 {
                *remaining -= 1;
                return Err(io::Error::from_raw_os_error(32));
            }
        }
        RealDisk.rename(from, to)
    }
    fn write_journal(&self, work: &Path, journal: &Journal) -> io::Result<()> {
        self.step()?;
        RealDisk.write_journal(work, journal)
    }
}

fn operations_for_apply() -> usize {
    let (_root, paths, mut journal) = fixture();
    let disk = FaultyDisk::default();
    transaction::apply(&disk, &paths, &mut journal, PATIENCE).unwrap();
    disk.operations.get()
}

#[test]
fn committed_update_leaves_exactly_one_copy_of_every_file() {
    let (_root, paths, mut journal) = fixture();
    transaction::apply(&RealDisk, &paths, &mut journal, PATIENCE).unwrap();
    assert_eq!(journal.phase, Phase::Applied);
    // Only replaced files are backed up, and by rename rather than copy.
    let backups: Vec<_> = snapshot(&paths.work().join("backup")).into_keys().collect();
    assert_eq!(backups, { let mut changed = CHANGED.map(String::from).to_vec(); changed.sort(); changed });
    assert!(snapshot(&paths.work().join("staged")).is_empty());
    assert!(transaction::commit(&RealDisk, &paths, &mut journal).unwrap());
    assert_single_installation(&paths, &new_install());
}

#[test]
fn power_loss_at_every_step_recovers_the_previous_version_exactly() {
    let total = operations_for_apply();
    assert!(total >= 2 + 2 * CHANGED.len());
    for step in 1..=total {
        let (_root, paths, mut journal) = fixture();
        let result = transaction::apply(&FaultyDisk::crashing_at(step), &paths, &mut journal, PATIENCE);
        // The final journal write is the last step; losing it is still a crash.
        assert!(result.is_err(), "step {step} should have been interrupted");
        match transaction::recover(&RealDisk, &paths, PATIENCE).unwrap() {
            Recovered::Pending(journal) | Recovered::RolledBack(journal) => assert_eq!(journal.phase, Phase::Staged),
            other => panic!("step {step}: unexpected {other:?}"),
        }
        let mut expected = old_install();
        for path in CHANGED {
            expected.insert(format!("{}/staged/{path}", transaction::WORK), new_bytes(path));
        }
        let mut installed = snapshot(paths.install());
        installed.remove(&format!("{}/{}", transaction::WORK, journal::FILE));
        assert_eq!(installed, expected, "step {step}: previous version plus the staged update, nothing else");

        // The staged update survives recovery and can be retried in full.
        let mut journal = journal::read(&paths.work()).unwrap();
        transaction::apply(&RealDisk, &paths, &mut journal, PATIENCE).unwrap();
        assert!(transaction::commit(&RealDisk, &paths, &mut journal).unwrap());
        assert_single_installation(&paths, &new_install());
    }
}

#[test]
fn power_loss_during_recovery_is_itself_recoverable() {
    for step in 1..=2 * CHANGED.len() + 1 {
        let (_root, paths, mut journal) = fixture();
        transaction::apply(&RealDisk, &paths, &mut journal, PATIENCE).unwrap();
        assert!(transaction::recover(&FaultyDisk::crashing_at(step), &paths, PATIENCE).is_err(), "step {step}");
        assert!(matches!(transaction::recover(&RealDisk, &paths, PATIENCE).unwrap(), Recovered::RolledBack(_)));
        assert!(transaction::discard(&paths));
        assert_single_installation(&paths, &old_install());
    }
}

#[test]
fn unproven_activation_is_retried_once_then_discarded() {
    let (_root, paths, mut journal) = fixture();
    transaction::apply(&RealDisk, &paths, &mut journal, PATIENCE).unwrap();
    let Recovered::RolledBack(mut journal) = transaction::recover(&RealDisk, &paths, PATIENCE).unwrap() else { panic!() };
    assert_eq!(journal.attempts, 1);
    transaction::apply(&RealDisk, &paths, &mut journal, PATIENCE).unwrap();
    let Recovered::Exhausted(journal) = transaction::recover(&RealDisk, &paths, PATIENCE).unwrap() else { panic!() };
    assert_eq!(journal.attempts, transaction::MAX_ATTEMPTS);
    assert_single_installation(&paths, &old_install());
}

#[test]
fn briefly_locked_file_is_retried_until_released() {
    let (_root, paths, mut journal) = fixture();
    let disk = FaultyDisk::locking(paths.installed("clipture_engine.exe"), 3);
    transaction::apply(&disk, &paths, &mut journal, Duration::from_secs(5)).unwrap();
    assert!(transaction::commit(&RealDisk, &paths, &mut journal).unwrap());
    assert_single_installation(&paths, &new_install());
}

#[test]
fn file_locked_past_patience_restores_the_previous_version() {
    let (_root, paths, mut journal) = fixture();
    let disk = FaultyDisk::locking(paths.installed("assets/default.mp3"), usize::MAX);
    assert!(transaction::apply(&disk, &paths, &mut journal, PATIENCE).is_err());
    assert_eq!(journal.phase, Phase::Staged);
    assert_eq!(journal.attempts, 0, "a lock is not the new version's fault");
    assert_eq!(journal::read(&paths.work()).unwrap().phase, Phase::Staged);
    assert!(transaction::discard(&paths));
    assert_single_installation(&paths, &old_install());
}

#[test]
fn a_real_open_handle_blocks_the_swap_and_rolls_back() {
    let (_root, paths, mut journal) = fixture();
    // An open handle without delete sharing, like a leftover engine or scanner.
    let mut options = fs::OpenOptions::new();
    options.read(true);
    #[cfg(windows)]
    std::os::windows::fs::OpenOptionsExt::share_mode(&mut options, 0x1);
    let handle = options.open(paths.installed("clipture_engine.exe")).unwrap();
    assert!(transaction::apply(&RealDisk, &paths, &mut journal, PATIENCE).is_err());
    drop(handle);
    assert!(transaction::discard(&paths));
    assert_single_installation(&paths, &old_install());
}

#[test]
fn in_use_backup_delays_cleanup_but_never_restores_the_old_version() {
    let (_root, paths, mut journal) = fixture();
    transaction::apply(&RealDisk, &paths, &mut journal, PATIENCE).unwrap();
    // The supervisor keeps running from the executable it moved into backup.
    let mut options = fs::OpenOptions::new();
    options.read(true);
    #[cfg(windows)]
    std::os::windows::fs::OpenOptionsExt::share_mode(&mut options, 0x1);
    let running = options.open(paths.backup("clipture.exe")).unwrap();
    assert!(!transaction::commit(&RealDisk, &paths, &mut journal).unwrap());
    assert!(matches!(transaction::recover(&RealDisk, &paths, PATIENCE).unwrap(), Recovered::CleanupPending));
    assert_eq!(journal::read(&paths.work()).unwrap().phase, Phase::Committed);
    let installed: BTreeMap<_, _> = snapshot(paths.install()).into_iter()
        .filter(|(path, _)| !path.starts_with(transaction::WORK)).collect();
    assert_eq!(installed, new_install());
    drop(running);
    assert!(matches!(transaction::recover(&RealDisk, &paths, PATIENCE).unwrap(), Recovered::Clean));
    assert_single_installation(&paths, &new_install());
}

#[test]
fn lost_journal_restores_backups_and_removes_everything_else() {
    let (_root, paths, mut journal) = fixture();
    transaction::apply(&RealDisk, &paths, &mut journal, PATIENCE).unwrap();
    fs::remove_file(paths.work().join(journal::FILE)).unwrap();
    assert!(matches!(transaction::recover(&RealDisk, &paths, PATIENCE).unwrap(), Recovered::Clean));
    assert_single_installation(&paths, &old_install());
}

#[test]
fn corrupt_journal_is_treated_like_a_lost_one() {
    let (_root, paths, mut journal) = fixture();
    transaction::apply(&RealDisk, &paths, &mut journal, PATIENCE).unwrap();
    fs::write(paths.work().join(journal::FILE), b"{\"schema\":1,\"pha").unwrap();
    assert!(matches!(transaction::recover(&RealDisk, &paths, PATIENCE).unwrap(), Recovered::Clean));
    assert_single_installation(&paths, &old_install());
}

#[test]
fn interrupted_stage_without_journal_is_discarded() {
    let (_root, paths, _) = fixture();
    fs::remove_file(paths.work().join(journal::FILE)).unwrap();
    write(&paths.work().join("journal-tmp123"), b"partial");
    assert!(matches!(transaction::recover(&RealDisk, &paths, PATIENCE).unwrap(), Recovered::Clean));
    assert_single_installation(&paths, &old_install());
}

#[test]
fn staging_copies_only_changed_files_and_reuses_a_matching_stage() {
    let server = crate::updates::component_download::tests::TestServer::start();
    let (key, signed) = crate::updates::component_download::tests::signed_fixture(&server, "9.9.9");
    let root = tempfile::tempdir().unwrap();
    let paths = Paths::new(root.path().join("Clipture"));
    // Two files already match the target; the fixture's target bytes are "test".
    for path in PATHS {
        write(&paths.installed(path), if matches!(path, "ffmpeg.exe" | "assets/option2.wav") { b"test" } else { b"old!" });
    }
    let local = root.path().join("verified-runtime");
    write(&local.join("clipture.exe"), b"test");
    let journal = tauri::async_runtime::block_on(stage::stage(&paths, &signed, &key, &AllowUpdates, Some(&local), |_| {})).unwrap();
    let mut changed: Vec<_> = journal.changed.iter().map(|file| file.path.as_str()).collect();
    changed.sort();
    assert_eq!(changed, ["assets/default.mp3", "clipture.exe", "clipture_engine.exe"]);
    let staged: Vec<_> = snapshot(&paths.work().join("staged")).into_keys().collect();
    assert_eq!(staged, changed, "unchanged files are never staged");

    // The server served each file once; staging again must not need it.
    let again = tauri::async_runtime::block_on(stage::stage(&paths, &signed, &key, &AllowUpdates, None, |_| {})).unwrap();
    assert_eq!(again.changed.len(), 3);

    let manifest = signed.verify(&key).unwrap();
    let locks = stage::lock_staged(&paths, &manifest, &again).unwrap();
    assert!(fs::write(paths.staged("clipture.exe"), b"evil").is_err(), "verified files cannot be rewritten while locked");
    drop(locks);
    fs::write(paths.staged("clipture.exe"), b"evil").unwrap();
    assert!(stage::lock_staged(&paths, &manifest, &again).is_err());
}
