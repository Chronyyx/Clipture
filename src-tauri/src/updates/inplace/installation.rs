//! Which installation an update replaces, and the one path that shortcuts,
//! autostart and the firewall should name.
//!
//! - A user-writable install updates itself in place.
//! - A process started from the ADR 0009 runtime store updates the per-user
//!   installation registered under HKCU, which it consolidates into first.
//! - An unmigrated Program Files install has no in-place target; it keeps
//!   the runtime store until it moves to a per-user install.
use super::transaction::Paths;
use std::path::{Path, PathBuf};

pub const UNINSTALL_KEY: &str = r"Software\Microsoft\Windows\CurrentVersion\Uninstall\Clipture";
pub const EXECUTABLE: &str = "clipture.exe";

/// The installation updated in place, if this process has one.
pub fn managed() -> Option<Paths> {
    candidate().filter(|paths| writable(paths.install()))
}

/// The directory `managed` would choose, without probing that it is
/// writable, so a launch with no update in flight writes nothing.
pub fn candidate() -> Option<Paths> {
    let exe = std::env::current_exe().ok()?.canonicalize().ok()?;
    if in_runtime_store(&exe) {
        return registered(Scope::User).filter(|directory| directory.join(EXECUTABLE).is_file()).map(Paths::new);
    }
    exe.parent().map(Paths::new)
}

/// Shortcuts and autostart must survive updates, so they never name a
/// versioned runtime directory. The Program Files launcher still forwards to
/// the runtime store, so it is stable too until migration.
pub fn stable_executable() -> PathBuf {
    let current = std::env::current_exe().unwrap_or_default();
    if let Some(paths) = managed() {
        return paths.installed(EXECUTABLE);
    }
    if current.canonicalize().is_ok_and(|exe| in_runtime_store(&exe)) {
        if let Some(machine) = registered(Scope::Machine).map(|directory| directory.join(EXECUTABLE)).filter(|path| path.is_file()) {
            return machine;
        }
    }
    current
}

pub fn in_runtime_store(exe: &Path) -> bool {
    super::super::runtime_store::root().ok()
        .and_then(|root| root.canonicalize().ok())
        .is_some_and(|root| exe.starts_with(root))
}

/// Creates and deletes an unnamed temporary file; Program Files refuses it.
pub fn writable(directory: &Path) -> bool {
    tempfile::tempfile_in(directory).is_ok()
}

#[derive(Clone, Copy)]
pub enum Scope { User, Machine }

/// The NSIS `InstallLocation`, without the quotes the installer writes.
pub fn registered(scope: Scope) -> Option<PathBuf> {
    #[cfg(windows)]
    {
        use winreg::{enums::{HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}, RegKey};
        let hive = RegKey::predef(match scope { Scope::User => HKEY_CURRENT_USER, Scope::Machine => HKEY_LOCAL_MACHINE });
        let location: String = hive.open_subkey(UNINSTALL_KEY).ok()?.get_value("InstallLocation").ok()?;
        let location = location.trim().trim_matches('"');
        (!location.is_empty()).then(|| PathBuf::from(location))
    }
    #[cfg(not(windows))]
    { let _ = scope; None }
}

/// A Program Files install that a per-user install has superseded.
pub fn machine_leftover() -> Option<PathBuf> {
    let machine = registered(Scope::Machine)?;
    let user = registered(Scope::User)?;
    let same = machine.canonicalize().ok().zip(user.canonicalize().ok()).is_some_and(|(a, b)| a == b);
    (!same && user.join(EXECUTABLE).is_file()).then_some(machine)
}
