//! Unelevated installer action. No Tauri runtime, WebView, tray or capture.
use crate::{paths::AppPaths, settings::SettingsStore};

pub fn command() -> Option<i32> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.first().map(String::as_str) != Some("--installer-startup") {
        return None;
    }
    let result = parse(&args).and_then(|(enabled, launch)| {
        let paths = AppPaths::discover().map_err(|e| e.to_string())?;
        if paths.test_mode || paths.isolated_profile {
            return Err("Installer startup changes are disabled for isolated profiles.".into());
        }
        let executable = std::env::current_exe().map_err(|e| e.to_string())?;
        let store = SettingsStore::load(paths).map_err(|e| e.to_string())?;
        // Same HKCU entry and --hidden argument as the runtime autostart plugin.
        let autostart = auto_launch::AutoLaunch::new(
            "Clipture",
            &format!("\"{}\"", executable.display()),
            &["--hidden"],
        );
        save_choice(&store, enabled, || {
            let result = if enabled {
                autostart.enable()
            } else {
                autostart.disable()
            };
            match result {
                // Unchecking on a clean install is already the desired state.
                Err(auto_launch::Error::Io(e))
                    if !enabled && e.kind() == std::io::ErrorKind::NotFound =>
                {
                    Ok(())
                }
                result => result.map_err(|e| e.to_string()),
            }
        })?;
        crate::platform::windows::remove_legacy_login_items().map_err(|e| e.to_string())?;
        if launch {
            use std::os::windows::process::CommandExt;
            std::process::Command::new(executable)
                .creation_flags(0x0800_0000)
                .spawn()
                .map_err(|e| e.to_string())?;
        }
        Ok(())
    });
    Some(match result {
        Ok(()) => 0,
        Err(error) => {
            use windows::{
                core::{w, PCWSTR},
                Win32::UI::WindowsAndMessaging::{MessageBoxW, MB_ICONERROR, MB_OK},
            };
            let message: Vec<u16> = format!("Could not apply Clipture's startup option: {error}\0")
                .encode_utf16()
                .collect();
            unsafe {
                MessageBoxW(
                    None,
                    PCWSTR(message.as_ptr()),
                    w!("Clipture setup"),
                    MB_OK | MB_ICONERROR,
                );
            }
            13
        }
    })
}

fn parse(args: &[String]) -> Result<(bool, bool), String> {
    let enabled = match args.get(1).map(String::as_str) {
        Some("true") => true,
        Some("false") => false,
        _ => return Err("Expected --installer-startup true or false.".into()),
    };
    match &args[2..] {
        [] => Ok((enabled, false)),
        [flag] if flag == "--launch" => Ok((enabled, true)),
        _ => Err("Unexpected installer startup argument.".into()),
    }
}

fn save_choice(
    store: &SettingsStore,
    enabled: bool,
    register: impl FnOnce() -> Result<(), String>,
) -> Result<(), String> {
    let previous = store.get();
    let mut next = previous.clone();
    next.start_on_login = enabled;
    store.save(next).map_err(|e| e.to_string())?;
    if let Err(error) = register() {
        store
            .save(previous)
            .map_err(|rollback| format!("{error}; restoring settings failed: {rollback}"))?;
        return Err(error);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn installer_startup_arguments_are_strict() {
        let args = |values: &[&str]| values.iter().map(|s| s.to_string()).collect::<Vec<_>>();
        assert_eq!(
            parse(&args(&["--installer-startup", "true"])).unwrap(),
            (true, false)
        );
        assert_eq!(
            parse(&args(&["--installer-startup", "false", "--launch"])).unwrap(),
            (false, true)
        );
        assert!(parse(&args(&["--installer-startup"])).is_err());
        assert!(parse(&args(&["--installer-startup", "yes"])).is_err());
        assert!(parse(&args(&["--installer-startup", "true", "other"])).is_err());
    }

    #[test]
    fn installer_startup_preserves_settings_and_rolls_back_registration_failure() {
        let root = tempfile::tempdir().unwrap();
        let store = SettingsStore::load(AppPaths::test_fixture(root.path())).unwrap();
        let mut settings = store.get();
        settings.clip_length_seconds = 77;
        store.save(settings).unwrap();
        save_choice(&store, false, || Ok(())).unwrap();
        assert!(!store.get().start_on_login);
        assert_eq!(store.get().clip_length_seconds, 77);
        assert!(save_choice(&store, true, || Err("registry denied".into())).is_err());
        assert!(!store.get().start_on_login);
        save_choice(&store, true, || Ok(())).unwrap();
        assert!(store.get().start_on_login);
    }
}
