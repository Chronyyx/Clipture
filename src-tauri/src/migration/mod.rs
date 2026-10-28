//! Installer-only commands: read-only compatibility checks, and the elevated
//! removal of a superseded Program Files install. Never starts the desktop host.
mod legacy_preflight;
mod machine_install;

pub use machine_install::command as machine_cleanup_command;

pub fn installer_command() -> Option<i32> {
    let mut args = std::env::args_os().skip(1);
    if args.next().as_deref() != Some(std::ffi::OsStr::new("--verify-legacy-upgrade")) {
        return None;
    }
    let result = (|| {
        let legacy = args.next().ok_or("Legacy install directory is required")?;
        if args.next().is_some() {
            return Err("Unexpected installer argument".to_owned());
        }
        let data = dirs::data_dir()
            .ok_or("Cannot resolve legacy app data")?
            .join("Clipture/data");
        legacy_preflight::verify(std::path::Path::new(&legacy), &data)
    })();
    match result {
        Ok(()) => Some(0),
        Err(error) => {
            eprintln!("Cross-grade safety check: {error}");
            Some(12)
        }
    }
}
