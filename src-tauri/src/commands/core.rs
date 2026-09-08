use tauri::{AppHandle, Emitter, State};

use crate::{
    app,
    commands::{blocking, CommandResult},
    contracts::{
        AudioInputDevice, ClipSettings, DisplayDevice, EngineDiagnostics, HostInfo, SaveClipResult,
        SaveSource,
    },
    save::SaveIoAnalyzerState,
    state::AppState,
};

#[tauri::command]
pub fn host_info(app: AppHandle, state: State<'_, AppState>) -> HostInfo {
    let info = state.host_info(app.package_info().version.to_string());
    let _ = app.emit_to("main", "host://ready", info.clone());
    info
}

#[tauri::command]
pub fn get_settings(state: State<'_, AppState>) -> ClipSettings {
    state.settings.get()
}

#[tauri::command]
pub async fn save_settings(
    app: AppHandle,
    state: State<'_, AppState>,
    settings: ClipSettings,
) -> CommandResult<ClipSettings> {
    let store = state.settings.clone();
    let saved = blocking(move || store.save(settings)).await?;
    if !state.paths.test_mode && !state.paths.isolated_profile {
        if let Err(error) = app::autostart::apply(&app, saved.start_on_login) {
            tracing::warn!(%error, "could not update autostart registration");
        }
    }
    let _ = app.emit("settings://changed", saved.clone());
    configure_engine_in_background(state.engine.clone(), saved.clone());
    Ok(saved)
}

#[tauri::command]
pub async fn configure_engine(state: State<'_, AppState>) -> CommandResult<EngineDiagnostics> {
    let settings = state.settings.get();
    let diagnostics = state
        .engine
        .configure_settings(&settings)
        .await
        .map_err(|error| error.to_string())?;
    state
        .engine
        .configure_hotkey(&settings.hotkey)
        .await
        .map_err(|error| error.to_string())?;
    Ok(diagnostics)
}

#[tauri::command]
pub async fn get_diagnostics(state: State<'_, AppState>) -> CommandResult<EngineDiagnostics> {
    state
        .engine
        .diagnostics()
        .await
        .map_err(|error| error.to_string())
}

#[tauri::command]
pub fn get_save_io_analyzer_state(state: State<'_, AppState>) -> SaveIoAnalyzerState {
    state.save_io.state()
}

#[tauri::command]
pub fn set_save_io_analyzer_armed(state: State<'_, AppState>, armed: bool) -> SaveIoAnalyzerState {
    state.save_io.set_armed(armed)
}

#[tauri::command]
pub async fn list_audio_input_devices(
    state: State<'_, AppState>,
) -> CommandResult<Vec<AudioInputDevice>> {
    state
        .engine
        .list_audio_input_devices()
        .await
        .map_err(|error| error.to_string())
}

#[tauri::command]
pub async fn list_display_devices(state: State<'_, AppState>) -> CommandResult<Vec<DisplayDevice>> {
    state
        .engine
        .list_display_devices()
        .await
        .map_err(|error| error.to_string())
}

#[tauri::command]
pub async fn save_clip(
    app: AppHandle,
    state: State<'_, AppState>,
    duration_seconds: u32,
) -> CommandResult<SaveClipResult> {
    let settings = state.settings.get();
    Ok(state
        .saves
        .save(&app, &settings, Some(duration_seconds), SaveSource::Ui)
        .await)
}

#[tauri::command]
pub fn open_main_window(app: AppHandle) -> CommandResult<()> {
    app::windows::open_main(&app).map_err(|error| error.to_string())
}

#[tauri::command]
pub fn close_main_window(app: AppHandle) -> CommandResult<()> {
    app::windows::destroy_main(&app).map_err(|error| error.to_string())
}

#[tauri::command]
pub fn exit_app(app: AppHandle, _state: State<'_, AppState>) {
    app::request_exit(&app);
}

fn configure_engine_in_background(
    engine: std::sync::Arc<crate::engine::EngineClient>,
    settings: ClipSettings,
) {
    tauri::async_runtime::spawn(async move {
        if let Err(error) = engine.configure_settings(&settings).await {
            tracing::warn!(%error, "could not apply recording settings");
            return;
        }
        if let Err(error) = engine.configure_hotkey(&settings.hotkey).await {
            tracing::warn!(%error, "could not apply native hotkey");
        }
    });
}
