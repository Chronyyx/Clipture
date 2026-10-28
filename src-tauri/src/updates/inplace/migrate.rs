//! Moves a Program Files installation to a per-user one (ADR 0020).
//!
//! The running version installs its own signed per-user installer silently,
//! while it keeps recording; that needs no administrator rights. Removing the
//! Program Files copy does, so it is offered once, only when the user opens
//! the main window, never mid-game. Declining leaves both copies working.
use super::installation::{self, Scope};
use super::super::{activation, manifest::{invalid, verify_file}, service::{UpdateError, UpdateGate}, stream_download};
use std::{path::Path, process::Command, sync::atomic::{AtomicBool, Ordering}, time::{Duration, Instant}};

const INSTALL_TIMEOUT: Duration = Duration::from_secs(600);
/// Runs `migration::machine_install` in an elevated copy of the per-user exe.
pub const CLEANUP_FLAG: &str = "--remove-machine-install";
const DECLINED: &str = "machine-cleanup-declined";
static OFFERED: AtomicBool = AtomicBool::new(false);

/// Installs this exact version per-user. Returns whether it did.
pub async fn install_per_user(gate: &dyn UpdateGate) -> Result<bool, UpdateError> {
    let Some(runtime) = activation::running_runtime() else { return Ok(false) };
    remove_stale_installers(&std::env::temp_dir());
    if installation::registered(Scope::User).is_some() || installation::registered(Scope::Machine).is_none() {
        return Ok(false);
    }
    let key = activation::public_key();
    let manifest = runtime.signed.verify(&key)?;
    let name = format!("Clipture_{}_x64-setup.exe", manifest.version);
    let base = format!("{}/v{}", super::super::release_source::asset_base(), manifest.version);
    let client = super::super::component_download::client()?;
    let signature_url = reqwest::Url::parse(&format!("{base}/{name}.sig")).map_err(|_| invalid("Invalid installer signature URL"))?;
    let signature = String::from_utf8(super::super::component_download::bounded(&client, &signature_url, 4096).await?)
        .map_err(|_| invalid("Installer signature is not UTF-8"))?;
    let url = reqwest::Url::parse(&format!("{base}/{name}")).map_err(|_| invalid("Invalid installer URL"))?;
    let installer = stream_download::download_installer(&url, &key, signature.trim(), gate).await?;
    let path = std::env::temp_dir().join(format!("clipture-setup-{}-{}.exe", manifest.version, std::process::id()));
    installer.persist(&path).map_err(|error| UpdateError::Io(error.error))?;
    let result = run_verified(&path, &key, signature.trim());
    let _ = std::fs::remove_file(&path);
    result?;
    let user = installation::registered(Scope::User).ok_or_else(|| invalid("The per-user installer did not register"))?;
    // The bundler stamps its bundle type into the installer's copy of the
    // controller, so only the other files match the manifest byte for byte.
    // Consolidation replaces the controller with the signed bytes next.
    for file in manifest.files.iter().filter(|file| file.path != installation::EXECUTABLE) {
        verify_file(&user.join(&file.path), file)?;
    }
    let machine = installation::registered(Scope::Machine).map(|directory| directory.join(installation::EXECUTABLE));
    if let Some(machine) = machine {
        migrate_shortcuts(&machine, &user.join(installation::EXECUTABLE));
    }
    tracing::info!(version = %manifest.version, install = %user.display(), "Moved Clipture to a per-user installation");
    Ok(true)
}

/// The user's own shortcuts to the Program Files copy follow it; a public
/// desktop shortcut gets a per-user twin, so removing machine-wide shortcuts
/// later leaves exactly the shortcuts the user had.
#[cfg(windows)]
fn migrate_shortcuts(machine: &Path, user: &Path) {
    use crate::platform::windows::{shortcut_target, write_shortcut};
    let same = |target: Option<std::path::PathBuf>| {
        target.is_some_and(|target| target.to_string_lossy().eq_ignore_ascii_case(&machine.to_string_lossy()))
    };
    let mut personal = Vec::new();
    if let Some(desktop) = dirs::desktop_dir() { personal.push(desktop.join("Clipture.lnk")); }
    if let Some(roaming) = dirs::data_dir() {
        personal.push(roaming.join(r"Microsoft\Internet Explorer\Quick Launch\User Pinned\TaskBar\Clipture.lnk"));
    }
    for link in &personal {
        if same(shortcut_target(link)) {
            if let Err(error) = write_shortcut(link, user) { tracing::warn!(%error, link = %link.display(), "Could not retarget shortcut"); }
        }
    }
    let public = std::env::var_os("PUBLIC").map(|public| std::path::PathBuf::from(public).join(r"Desktop\Clipture.lnk"));
    if let (Some(public), Some(desktop)) = (public, dirs::desktop_dir().map(|desktop| desktop.join("Clipture.lnk"))) {
        if same(shortcut_target(&public)) && !desktop.exists() {
            if let Err(error) = write_shortcut(&desktop, user) { tracing::warn!(%error, "Could not create the per-user desktop shortcut"); }
        }
    }
}

#[cfg(not(windows))]
fn migrate_shortcuts(_: &Path, _: &Path) {}

/// A migration interrupted mid-install (closed, crashed, power loss) leaves
/// its downloaded installer in the temp folder; the next attempt removes it.
/// An installer still running is in use and stays.
fn remove_stale_installers(temp: &Path) {
    let Ok(entries) = std::fs::read_dir(temp) else { return };
    for entry in entries.flatten() {
        let name = entry.file_name().to_string_lossy().into_owned();
        if name.starts_with("clipture-setup-") && name.ends_with(".exe") && entry.file_type().is_ok_and(|kind| kind.is_file()) {
            let _ = std::fs::remove_file(entry.path());
        }
    }
}

/// Re-authenticates the exact bytes about to run while holding them open
/// against writes, then runs the installer without closing this process.
fn run_verified(path: &Path, key: &str, signature: &str) -> Result<(), UpdateError> {
    let mut options = std::fs::OpenOptions::new();
    options.read(true);
    #[cfg(windows)]
    std::os::windows::fs::OpenOptionsExt::share_mode(&mut options, 0x1);
    let mut held = options.open(path)?;
    let mut bytes = Vec::new();
    std::io::Read::read_to_end(&mut held, &mut bytes)?;
    stream_download::verify_before_install(&bytes, key, signature)?;
    drop(bytes);
    let mut child = Command::new(path).args(["/S", "/MIGRATE"]).spawn()?;
    let started = Instant::now();
    loop {
        if let Some(status) = child.try_wait()? {
            return if status.success() { Ok(()) } else { Err(invalid(format!("Per-user installer failed: {status}"))) };
        }
        if started.elapsed() > INSTALL_TIMEOUT {
            let _ = child.kill();
            return Err(invalid("Per-user installer timed out"));
        }
        std::thread::sleep(Duration::from_millis(200));
    }
}

/// Asks once, in the background, for the elevation that removes a
/// superseded Program Files copy. Declining is remembered.
pub fn offer_machine_cleanup() {
    if !activation::enabled() || OFFERED.swap(true, Ordering::AcqRel) { return; }
    let Some(machine) = installation::machine_leftover() else { return };
    let Some(user) = installation::registered(Scope::User) else { return };
    let Ok(marker) = super::supervisor::quarantine_root().map(|root| root.join(DECLINED)) else { return };
    if marker.exists() { return; }
    std::thread::spawn(move || match confirm(&machine).then(|| elevate(&user.join(installation::EXECUTABLE), &machine)).unwrap_or(Ok(false)) {
        Ok(true) => tracing::info!("Removed the superseded Program Files installation"),
        Ok(false) => {
            tracing::info!("Removal of the Program Files installation was declined");
            let _ = std::fs::write(&marker, b"");
        }
        Err(error) => tracing::warn!(%error, "Could not remove the Program Files installation"),
    });
}

/// The superseded Program Files folder Settings may offer to remove.
pub fn machine_copy() -> Option<std::path::PathBuf> {
    if !activation::enabled() { return None; }
    installation::machine_leftover()
}

/// Removal asked for from Settings. Pressing the button is the consent, so
/// Windows' prompt follows directly. Ok(false) when it was declined.
pub fn remove_machine_copy() -> Result<bool, UpdateError> {
    let machine = machine_copy().ok_or_else(|| invalid("No old Program Files copy is registered"))?;
    let user = installation::registered(Scope::User).ok_or_else(|| invalid("No per-user installation is registered"))?;
    elevate(&user.join(installation::EXECUTABLE), &machine)
}

/// Says what the Windows administrator prompt is for before it appears, so
/// it is never a surprise. Declining here is remembered like declining it.
#[cfg(windows)]
fn confirm(machine: &Path) -> bool {
    use windows::{core::PCWSTR, Win32::UI::WindowsAndMessaging::{MessageBoxW, IDYES, MB_ICONQUESTION, MB_SETFOREGROUND, MB_YESNO}};
    let text = format!(
        "Clipture now runs from your user folder, so future updates install smoothly in the \
         background, without administrator prompts.\n\n\
         The old copy in {} is no longer used. Remove it now?\n\n\
         Windows will ask for administrator permission this one time.",
        machine.display()
    );
    let wide = |value: &str| value.encode_utf16().chain(Some(0)).collect::<Vec<u16>>();
    let (text, title) = (wide(&text), wide("Finish moving Clipture"));
    // SAFETY: both strings are NUL-terminated and outlive the call.
    unsafe { MessageBoxW(None, PCWSTR(text.as_ptr()), PCWSTR(title.as_ptr()), MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) == IDYES }
}

#[cfg(not(windows))]
fn confirm(_: &Path) -> bool { false }

/// Ok(false) when the user declined the elevation prompt.
#[cfg(windows)]
fn elevate(helper: &Path, machine: &Path) -> Result<bool, UpdateError> {
    use std::os::windows::ffi::OsStrExt;
    use windows::{core::PCWSTR, Win32::{Foundation::{CloseHandle, WAIT_OBJECT_0}, System::Threading::{GetExitCodeProcess, WaitForSingleObject}, UI::{Shell::{ShellExecuteExW, SEE_MASK_NOCLOSEPROCESS, SHELLEXECUTEINFOW}, WindowsAndMessaging::SW_HIDE}}};
    let wide = |text: &std::ffi::OsStr| text.encode_wide().chain(Some(0)).collect::<Vec<u16>>();
    let arguments = format!("{CLEANUP_FLAG} \"{}\"", machine.display());
    if machine.to_string_lossy().contains('"') {
        return Err(invalid("Unexpected quote in an installation path"));
    }
    let (verb, file, parameters) = (wide("runas".as_ref()), wide(helper.as_os_str()), wide(arguments.as_ref()));
    let mut info = SHELLEXECUTEINFOW {
        cbSize: std::mem::size_of::<SHELLEXECUTEINFOW>() as u32,
        fMask: SEE_MASK_NOCLOSEPROCESS,
        lpVerb: PCWSTR(verb.as_ptr()),
        lpFile: PCWSTR(file.as_ptr()),
        lpParameters: PCWSTR(parameters.as_ptr()),
        nShow: SW_HIDE.0,
        ..Default::default()
    };
    // SAFETY: every string outlives the call; the process handle is closed below.
    if let Err(error) = unsafe { ShellExecuteExW(&mut info) } {
        const CANCELLED: i32 = 1223;
        return if error.code().0 & 0xFFFF == CANCELLED { Ok(false) } else { Err(invalid(error.to_string())) };
    }
    let process = info.hProcess;
    let mut code = 1_u32;
    // SAFETY: `process` is the owned handle ShellExecuteExW returned.
    let finished = unsafe {
        let waited = WaitForSingleObject(process, 5 * 60 * 1000) == WAIT_OBJECT_0;
        let read = waited && GetExitCodeProcess(process, &mut code).is_ok();
        let _ = CloseHandle(process);
        read
    };
    if !finished || code != 0 {
        return Err(invalid(format!("Elevated cleanup ended with status {code}")));
    }
    Ok(true)
}

#[cfg(not(windows))]
fn elevate(_: &Path, _: &Path) -> Result<bool, UpdateError> { Ok(false) }

#[cfg(test)]
mod tests {
    #[test]
    fn interrupted_migration_installers_are_removed_and_nothing_else() {
        let temp = tempfile::tempdir().unwrap();
        for name in ["clipture-setup-1.7.4-123.exe", "clipture-setup-1.7.5-9.exe", "other-setup.exe", "clipture-setup-notes.txt"] {
            std::fs::write(temp.path().join(name), b"x").unwrap();
        }
        super::remove_stale_installers(temp.path());
        let mut left: Vec<_> = std::fs::read_dir(temp.path()).unwrap().flatten().map(|entry| entry.file_name().into_string().unwrap()).collect();
        left.sort();
        assert_eq!(left, ["clipture-setup-notes.txt", "other-setup.exe"]);
    }
}
