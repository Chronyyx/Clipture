use super::{manifest::invalid, runtime_store::{self, VerifiedRuntime}, service::UpdateError};
use std::{io::{Read, Write}, path::PathBuf, process::Command, sync::{Mutex, OnceLock}, time::Duration};
use super::handoff_process::{self, OwnedProcess};

static RUNTIME: OnceLock<VerifiedRuntime> = OnceLock::new();
static UI_RUNTIME: Mutex<Option<VerifiedRuntime>> = Mutex::new(None);
const HANDOFF: &str = "--runtime-handoff";

pub fn public_key() -> String {
    let context: tauri::Context<tauri::Wry> = tauri::generate_context!();
    context.config().plugins.0.get("updater")
        .and_then(|value| value.get("pubkey")).and_then(serde_json::Value::as_str)
        .unwrap_or_default().to_owned()
}

pub fn enabled() -> bool {
    !cfg!(debug_assertions) && !cfg!(test) && cfg!(all(windows, target_arch = "x86_64"))
        && std::env::var_os(crate::paths::TEST_MODE_ENV).is_none()
        && std::env::var_os(crate::paths::DATA_DIR_OVERRIDE).is_none()
        && configured(&public_key(), option_env!("CLIPTURE_NATIVE_ID"))
}

fn configured(key: &str, native_id: Option<&str>) -> bool {
    use base64::Engine;
    native_id.is_some_and(super::manifest::valid_hash)
        && base64::engine::general_purpose::STANDARD.decode(key).ok()
            .and_then(|bytes| String::from_utf8(bytes).ok())
            .and_then(|text| minisign_verify::PublicKey::decode(&text).ok()).is_some()
}

fn unelevated() -> Result<(), UpdateError> {
    #[cfg(windows)]
    {
        if unsafe { windows::Win32::UI::Shell::IsUserAnAdmin().as_bool() } {
            return Err(invalid("Per-user runtime updates cannot run elevated"));
        }
    }
    Ok(())
}

fn own_runtime(key: &str) -> Result<Option<VerifiedRuntime>, UpdateError> {
    let exe = std::env::current_exe()?.canonicalize()?;
    let root = runtime_store::root()?;
    if !root.exists() { return Ok(None); }
    let root = root.canonicalize()?;
    if !exe.starts_with(&root) { return Ok(None); }
    let directory = exe.parent().ok_or_else(|| invalid("Runtime executable directory missing"))?;
    let signed = runtime_store::read_signed(&directory.join("manifest.json"))?;
    let runtime = runtime_store::verify(&root, signed, key)?;
    if runtime.directory.join("clipture.exe").canonicalize()? != exe {
        return Err(invalid("Selected runtime executable identity mismatch"));
    }
    Ok(Some(runtime))
}

pub fn startup() -> Result<bool, UpdateError> {
    if cfg!(debug_assertions) || cfg!(test) { return Ok(false); }
    let handoff = std::env::args().any(|arg| arg == HANDOFF);
    let key = public_key();
    if let Some(runtime) = own_runtime(&key)? {
        unelevated()?;
        RUNTIME.set(runtime).map_err(|_| invalid("Runtime initialized twice"))?;
    }
    if std::env::args().any(|arg| arg == "--ui-worker") { return Ok(false); }
    if !enabled() && !handoff { return Ok(false); }
    if handoff {
        if RUNTIME.get().is_none() { return Err(invalid("Handoff target is not a verified runtime")); }
        receive_handoff()?;
        return Ok(false);
    }
    Ok(false)
}

pub fn activate_at_launch() -> Result<bool, UpdateError> {
    if !enabled() || std::env::args().any(|arg| matches!(arg.as_str(), HANDOFF | "--runtime-recovery")) { return Ok(false); }
    let key = public_key();
    let root = runtime_store::root()?;
    let current = std::env::current_exe()?.canonicalize()?;
    let mut candidates: Vec<_> = ["pending.json", "active.json", "previous.json"].into_iter()
        .filter_map(|name| runtime_store::load_candidate(&root, name, &key).ok()).collect();
    candidates.sort_by(|left, right| {
        semver::Version::parse(&right.manifest.version).unwrap()
            .cmp(&semver::Version::parse(&left.manifest.version).unwrap())
    });
    candidates.dedup_by(|left, right| left.directory == right.directory);
    for candidate in candidates {
        if candidate.directory.join("clipture.exe").canonicalize()? == current {
            return Ok(false);
        }
        if !candidate.manifest.newer_than(env!("CARGO_PKG_VERSION")) { continue; }
        runtime_store::atomic_signed(&root, "pending.json", &candidate.signed)?;
        match prepare(&candidate).and_then(|child| child.commit()) {
            Ok(()) => return Ok(true),
            Err(error) => tracing::warn!(%error, "Runtime activation failed; retaining launcher"),
        }
    }
    Ok(false)
}

pub struct HandoffChild(OwnedProcess);
impl HandoffChild {
    pub fn commit(mut self) -> Result<(), UpdateError> {
        self.0.send(b"activate\n")?;
        self.0.expect(b"committed\n", Duration::from_secs(10))?;
        self.0.release()?;
        Ok(())
    }
}

pub fn prepare(candidate: &VerifiedRuntime) -> Result<HandoffChild, UpdateError> {
    unelevated()?;
    let mut command = Command::new(std::env::current_exe()?);
    command.arg("--runtime-supervisor").arg(std::process::id().to_string())
        .arg(candidate.manifest.directory_name());
    if std::env::args().any(|arg| matches!(arg.as_str(), "--hidden" | "--background")) { command.arg("--hidden"); }
    if let Some(invite) = invite_link_argument(&std::env::args().collect::<Vec<_>>()) { command.arg(invite); }
    #[cfg(windows)]
    { use std::os::windows::process::CommandExt; command.creation_flags(0x0800_0000); }
    let mut child = OwnedProcess::spawn(&mut command)?;
    child.expect(b"prepared\n", Duration::from_secs(30))?;
    Ok(HandoffChild(child))
}

fn receive_handoff() -> Result<(), UpdateError> {
    let mut output = handoff_process::output()?;
    output.write_all(b"prepared\n")?;
    output.flush()?;
    let mut message = [0; 9];
    handoff_process::input()?.read_exact(&mut message)?;
    if &message != b"activate\n" { return Err(invalid("Controller did not commit activation")); }
    Ok(())
}

pub(super) fn report_healthy() -> Result<(), UpdateError> {
    if !std::env::args().any(|arg| arg == HANDOFF) { return Ok(()); }
    let mut output = handoff_process::output()?;
    output.write_all(b"healthy\n")?;
    output.flush()?;
    Ok(())
}

#[cfg(windows)]
pub fn run_supervisor() -> Result<bool, UpdateError> {
    use std::os::windows::{fs::OpenOptionsExt, process::CommandExt};
    let args: Vec<_> = std::env::args().collect();
    let Some(index) = args.iter().position(|arg| arg == "--runtime-supervisor") else { return Ok(false); };
    if !enabled() { return Err(invalid("Runtime supervisor is disabled in this build")); }
    unelevated()?;
    let parent = args.get(index + 1).and_then(|value| value.parse().ok()).ok_or_else(|| invalid("Missing handoff parent"))?;
    let parent = handoff_process::ParentProcess::open(parent)?;
    let root = runtime_store::root()?;
    runtime_store::validate_directory(&root)?;
    let lock = std::fs::OpenOptions::new().write(true).create(true).truncate(false).share_mode(0).open(root.join("handoff.lock"))?;
    let key = public_key();
    let candidate = runtime_store::load_candidate(&root, "pending.json", &key)?;
    if args.get(index + 2) != Some(&candidate.manifest.directory_name()) { return Err(invalid("Pending runtime changed during handoff")); }
    receive_handoff()?;
    let mut output = handoff_process::output()?;
    output.write_all(b"committed\n")?;
    output.flush()?;
    parent.wait(Duration::from_secs(60))?;
    let mut command = Command::new(candidate.directory.join("clipture.exe"));
    command.arg(HANDOFF).current_dir(&candidate.directory).creation_flags(0x0800_0000);
    if args.iter().any(|arg| arg == "--hidden") { command.arg("--hidden"); }
    if let Some(invite) = invite_link_argument(&args) { command.arg(invite); }
    let result = (|| {
        let mut child = OwnedProcess::spawn(&mut command)?;
        child.expect(b"prepared\n", Duration::from_secs(30))?;
        child.send(b"activate\n")?;
        child.expect(b"healthy\n", Duration::from_secs(60))?;
        child.healthy_for(Duration::from_secs(5))?;
        runtime_store::select(&root, &candidate, &key)?;
        child.release()?;
        Ok::<_, UpdateError>(())
    })();
    if let Err(error) = result {
        if let Err(marker_error) = runtime_store::atomic_signed(&root, "failed.json", &candidate.signed) {
            tracing::error!(%marker_error, "Could not quarantine failed runtime; still attempting recovery");
        }
        tracing::error!(%error, "Runtime failed startup health validation; restarting previous controller");
        let mut rollback = Command::new(std::env::current_exe()?);
        rollback.arg("--runtime-recovery").creation_flags(0x0800_0000);
        if args.iter().any(|arg| arg == "--hidden") { rollback.arg("--hidden"); }
        if let Some(invite) = invite_link_argument(&args) { rollback.arg(invite); }
        rollback.spawn()?;
    }
    drop(lock);
    Ok(true)
}

#[cfg(not(windows))]
pub fn run_supervisor() -> Result<bool, UpdateError> { Ok(false) }

/// A `clipture:` invite link passed through a handoff verbatim, as a single
/// argument. It is parsed and validated only by the sharing domain.
fn invite_link_argument(args: &[String]) -> Option<&str> {
    args.iter()
        .map(String::as_str)
        .find(|arg| arg.starts_with("clipture:") && arg.len() <= 512)
}

pub fn ui_executable() -> Result<PathBuf, UpdateError> {
    let current = std::env::current_exe()?;
    if !enabled() { return Ok(current); }
    let mut slot = UI_RUNTIME.lock().unwrap_or_else(|error| error.into_inner());
    let Some(native_id) = option_env!("CLIPTURE_NATIVE_ID") else { return Ok(current); };
    let root = runtime_store::root()?;
    let key = public_key();
    let Ok(candidate) = runtime_store::load_candidate(&root, "pending.json", &key) else { return Ok(current); };
    if candidate.manifest.compatibility.host != native_id || !candidate.manifest.newer_than(env!("CARGO_PKG_VERSION")) {
        return Ok(current);
    }
    let directory = current.parent().ok_or_else(|| invalid("Controller directory missing"))?;
    for file in candidate.manifest.files.iter().filter(|file| file.path != "clipture.exe") {
        if super::manifest::verify_file(&directory.join(&file.path), file).is_err() { return Ok(current); }
    }
    unelevated()?;
    let path = candidate.directory.join("clipture.exe");
    *slot = Some(candidate);
    Ok(path)
}
