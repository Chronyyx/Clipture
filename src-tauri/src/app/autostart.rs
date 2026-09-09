use tauri::AppHandle;
use tauri_plugin_autostart::ManagerExt;

use crate::error::{AppError, AppResult};

pub fn apply(app: &AppHandle, enabled: bool) -> AppResult<()> {
    let manager = app.autolaunch();
    let result = if enabled {
        manager.enable()
    } else if manager
        .is_enabled()
        .map_err(|error| AppError::Integration(error.to_string()))?
    {
        manager.disable()
    } else {
        Ok(())
    };
    result.map_err(|error| AppError::Integration(error.to_string()))?;
    #[cfg(windows)]
    crate::platform::windows::remove_legacy_login_items()
        .map_err(|error| AppError::Integration(error.to_string()))?;
    Ok(())
}
