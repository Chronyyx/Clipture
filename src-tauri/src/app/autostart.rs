use tauri::AppHandle;

use crate::error::{AppError, AppResult};

pub fn apply(app: &AppHandle, enabled: bool) -> AppResult<()> {
    #[cfg(windows)]
    {
        let _ = app;
        let launcher = launcher();
        let result = if enabled {
            launcher.enable()
        } else if launcher.is_enabled().map_err(|error| AppError::Integration(error.to_string()))? {
            launcher.disable()
        } else {
            Ok(())
        };
        result.map_err(|error| AppError::Integration(error.to_string()))?;
        crate::platform::windows::remove_legacy_login_items()
            .map_err(|error| AppError::Integration(error.to_string()))?;
        Ok(())
    }
    #[cfg(not(windows))]
    {
        use tauri_plugin_autostart::ManagerExt;
        let manager = app.autolaunch();
        let result = if enabled {
            manager.enable()
        } else if manager.is_enabled().map_err(|error| AppError::Integration(error.to_string()))? {
            manager.disable()
        } else {
            Ok(())
        };
        result.map_err(|error| AppError::Integration(error.to_string()))
    }
}

/// Points an existing startup entry at the stable executable, after an
/// update moved Clipture out of a versioned runtime directory.
#[cfg(windows)]
pub fn retarget() -> AppResult<()> {
    let launcher = launcher();
    if launcher.is_enabled().map_err(|error| AppError::Integration(error.to_string()))? {
        launcher.enable().map_err(|error| AppError::Integration(error.to_string()))?;
    }
    Ok(())
}

/// Same HKCU entry and `--hidden` argument as the installer's startup option,
/// but always naming the path that survives updates, never `current_exe`.
#[cfg(windows)]
fn launcher() -> auto_launch::AutoLaunch {
    let executable = crate::updates::stable_executable();
    auto_launch::AutoLaunch::new("Clipture", &format!("\"{}\"", executable.display()), &["--hidden"])
}
