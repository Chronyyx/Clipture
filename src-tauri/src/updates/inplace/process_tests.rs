//! Real processes and real file sizes: the Windows behaviour the supervisor
//! relies on, and how long the swap keeps Clipture down.
use super::{
    journal::{self, ChangedFile, Journal},
    transaction::{self, Paths, Recovered},
    RealDisk,
};
use crate::updates::manifest::{verify_file, RuntimeFile, SignedManifest, PATHS};
use sha2::{Digest, Sha256};
use std::{fs, path::Path, process::Command, time::{Duration, Instant}};

const PATIENCE: Duration = Duration::from_secs(5);

fn journal_for(paths: &[&str]) -> Journal {
    Journal::new(
        SignedManifest { manifest: "fixture".into(), signature: "fixture".into() },
        paths.iter().map(|path| ChangedFile { path: (*path).into(), replaces_existing: true }).collect(),
    )
}

/// The supervisor runs from the executable it replaces. Windows lets a
/// running image be renamed but not deleted, so the new version starts from
/// the same path at once and the backup is removed after the old one exits.
#[cfg(windows)]
#[test]
fn running_executable_is_swapped_in_place_and_cleaned_after_exit() {
    let root = tempfile::tempdir().unwrap();
    let paths = Paths::new(root.path().join("Clipture"));
    let system = std::env::var_os("SystemRoot").map(std::path::PathBuf::from).unwrap().join("System32");
    for path in PATHS {
        fs::create_dir_all(paths.installed(path).parent().unwrap()).unwrap();
        fs::write(paths.installed(path), b"unchanged").unwrap();
    }
    // Old version: a real running executable at the stable path.
    fs::copy(system.join("PING.EXE"), paths.installed("clipture.exe")).unwrap();
    let mut old = Command::new(paths.installed("clipture.exe")).args(["-n", "30", "127.0.0.1"])
        .stdout(std::process::Stdio::null()).spawn().unwrap();
    // New version: a different real executable, staged.
    fs::create_dir_all(paths.work().join("staged")).unwrap();
    fs::copy(system.join("HOSTNAME.EXE"), paths.staged("clipture.exe")).unwrap();
    let mut journal = journal_for(&["clipture.exe"]);
    journal::write(&paths.work(), &journal).unwrap();

    transaction::apply(&RealDisk, &paths, &mut journal, PATIENCE).unwrap();
    let new = Command::new(paths.installed("clipture.exe")).output().unwrap();
    assert!(new.status.success() && !new.stdout.is_empty(), "the new version starts from the stable path");
    assert!(old.try_wait().unwrap().is_none(), "the old version kept running from its backup");

    assert!(!transaction::commit(&RealDisk, &paths, &mut journal).unwrap(), "a running backup cannot be deleted yet");
    assert!(matches!(transaction::recover(&RealDisk, &paths, PATIENCE).unwrap(), Recovered::CleanupPending));
    old.kill().unwrap();
    old.wait().unwrap();
    let deadline = Instant::now() + Duration::from_secs(10);
    while !matches!(transaction::recover(&RealDisk, &paths, PATIENCE).unwrap(), Recovered::Clean) {
        assert!(Instant::now() < deadline, "cleanup must finish once the image is released");
        std::thread::sleep(Duration::from_millis(50));
    }
    assert!(!paths.work().exists());
    assert_eq!(fs::read(paths.installed("clipture.exe")).unwrap(), fs::read(system.join("HOSTNAME.EXE")).unwrap());
}

fn runtime_file(path: &str, bytes: &[u8]) -> RuntimeFile {
    RuntimeFile {
        path: path.into(), size: bytes.len() as u64, sha256: format!("{:x}", Sha256::digest(bytes)),
        url: String::new(), blocks: bytes.chunks(1_048_576).map(|block| format!("{:x}", Sha256::digest(block))).collect(),
        block_size: 1_048_576,
    }
}

/// `cargo test --release --lib update_downtime -- --ignored --nocapture`
///
/// Uses the installed release binaries when present, otherwise same-sized
/// generated files. Copies only; never touches the installation itself.
#[test]
#[ignore = "benchmark; prints timings"]
fn update_downtime_benchmark() {
    let installed = Path::new(r"C:\Program Files\Clipture");
    let root = tempfile::tempdir().unwrap();
    let paths = Paths::new(root.path().join("Clipture"));
    let sizes = [("clipture.exe", 15 << 20), ("clipture_engine.exe", 16 << 20), ("ffmpeg.exe", 101 << 20), ("assets/default.mp3", 47 << 10), ("assets/option2.wav", 56 << 10)];
    let mut targets = Vec::new();
    for (path, size) in sizes {
        let bytes = fs::read(installed.join(path)).unwrap_or_else(|_| (0..size).map(|index| (index * 31 % 251) as u8).collect());
        fs::create_dir_all(paths.installed(path).parent().unwrap()).unwrap();
        fs::write(paths.installed(path), &bytes).unwrap();
        let mut next = bytes.clone();
        next[0] ^= 1;
        targets.push(runtime_file(path, &next));
        fs::create_dir_all(paths.staged(path).parent().unwrap()).unwrap();
        fs::write(paths.staged(path), &next).unwrap();
    }
    let total: u64 = targets.iter().map(|file| file.size).sum();

    // Before shutdown, while the old version still records.
    let started = Instant::now();
    for target in &targets { verify_file(&paths.staged(&target.path), target).unwrap(); }
    let verify = started.elapsed();

    // Downtime: worst case, every file changed.
    let mut journal = journal_for(&PATHS);
    journal::write(&paths.work(), &journal).unwrap();
    let started = Instant::now();
    transaction::apply(&RealDisk, &paths, &mut journal, PATIENCE).unwrap();
    let swap = started.elapsed();

    let started = Instant::now();
    transaction::restore(&RealDisk, &paths, &journal.changed, PATIENCE).unwrap();
    let rollback = started.elapsed();

    // ADR 0009 re-hashed the whole runtime on every launch before starting.
    let started = Instant::now();
    for target in &targets { verify_file(&paths.staged(&target.path), target).unwrap(); }
    let per_launch_hash = started.elapsed();

    println!("target set: {} files, {:.1} MiB", targets.len(), total as f64 / 1_048_576.0);
    println!("verify + lock stage (old version still running): {verify:?}");
    println!("swap all files (downtime added by in-place):     {swap:?}");
    println!("rollback all files:                               {rollback:?}");
    println!("per-launch runtime hash removed from startup:     {per_launch_hash:?}");
}
