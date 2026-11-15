//! Telemetry commands: validate, call the service, serialize.
use super::TelemetryStatus;
use crate::state::AppState;
use tauri::State;

#[tauri::command]
pub fn telemetry_status(state: State<'_, AppState>) -> TelemetryStatus {
    state.telemetry.status()
}

#[tauri::command]
pub async fn telemetry_enroll(state: State<'_, AppState>, code: String) -> Result<TelemetryStatus, String> {
    if code.len() > 4096 {
        return Err("That is not a Clipture telemetry invite code.".into());
    }
    state.telemetry.enroll(&code).await
}

#[tauri::command]
pub fn telemetry_stop(state: State<'_, AppState>) -> TelemetryStatus {
    state.telemetry.stop()
}

#[tauri::command]
pub fn telemetry_last_batch(state: State<'_, AppState>) -> Vec<serde_json::Value> {
    state.telemetry.last_batch()
}
