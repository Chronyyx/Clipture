use tauri::{AppHandle, State};

use super::{
    inplace::migrate,
    model::{OldInstallation, OldInstallationOutcome, OldInstallationRemoval, UpdateState},
    service::{UpdateError, UpdateService},
};

/// The superseded Program Files copy, if Settings should offer to remove it.
#[tauri::command]
pub fn get_old_installation() -> Option<OldInstallation> {
    migrate::machine_copy().map(|folder| OldInstallation { folder: folder.display().to_string() })
}

#[tauri::command]
pub async fn remove_old_installation() -> OldInstallationRemoval {
    let result = tauri::async_runtime::spawn_blocking(migrate::remove_machine_copy).await;
    let (outcome, message) = match result {
        Ok(Ok(true)) => (OldInstallationOutcome::Removed, None),
        Ok(Ok(false)) => (OldInstallationOutcome::Declined, None),
        Ok(Err(error)) => (OldInstallationOutcome::Failed, Some(error.to_string())),
        Err(error) => (OldInstallationOutcome::Failed, Some(error.to_string())),
    };
    OldInstallationRemoval { outcome, message }
}

#[tauri::command]
pub fn get_update_state(updates: State<'_, UpdateService>) -> UpdateState {
    updates.get()
}

#[tauri::command]
pub async fn check_for_updates(
    app: AppHandle,
    updates: State<'_, UpdateService>,
) -> Result<UpdateState, UpdateError> {
    updates.check(&app).await
}

#[tauri::command]
pub async fn download_update(
    app: AppHandle,
    updates: State<'_, UpdateService>,
) -> Result<(), UpdateError> {
    updates.download(&app).await
}

#[tauri::command]
pub async fn install_update(
    app: AppHandle,
    updates: State<'_, UpdateService>,
) -> Result<(), UpdateError> {
    updates.install(&app).await
}
