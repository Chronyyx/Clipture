//! The only filesystem mutations an in-place transaction makes. Tests swap in
//! a disk that crashes or reports locks at a chosen step.
use super::journal::{self, Journal};
use std::{io, path::Path, thread, time::{Duration, Instant}};

pub trait Disk {
    fn rename(&self, from: &Path, to: &Path) -> io::Result<()>;
    fn write_journal(&self, work: &Path, journal: &Journal) -> io::Result<()>;
}

pub struct RealDisk;

impl Disk for RealDisk {
    fn rename(&self, from: &Path, to: &Path) -> io::Result<()> { rename(from, to, false) }
    fn write_journal(&self, work: &Path, journal: &Journal) -> io::Result<()> { journal::write(work, journal) }
}

pub fn rename(from: &Path, to: &Path, replace: bool) -> io::Result<()> {
    #[cfg(windows)]
    return crate::platform::windows::move_file(from, to, replace);
    #[cfg(not(windows))]
    {
        if !replace && to.exists() { return Err(io::ErrorKind::AlreadyExists.into()); }
        std::fs::rename(from, to)
    }
}

/// Access denied, sharing and lock violations: an old process, a scanner or
/// the indexer still has the file open. These usually clear within seconds.
pub fn is_locked(error: &io::Error) -> bool {
    matches!(error.raw_os_error(), Some(5 | 32 | 33))
}

/// Retries an operation while its target is locked, until `patience` elapses.
pub fn retry_locked<T>(patience: Duration, mut operation: impl FnMut() -> io::Result<T>) -> io::Result<T> {
    let deadline = Instant::now() + patience;
    let mut delay = Duration::from_millis(20);
    loop {
        match operation() {
            Err(error) if is_locked(&error) && Instant::now() < deadline => {
                thread::sleep(delay.min(deadline.saturating_duration_since(Instant::now())));
                delay = (delay * 2).min(Duration::from_millis(500));
            }
            result => return result,
        }
    }
}
