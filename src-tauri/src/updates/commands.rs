use tauri::{AppHandle, State};

use super::{
    model::UpdateState,
    service::{UpdateError, UpdateService},
};

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
