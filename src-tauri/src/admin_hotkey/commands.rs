use std::{path::PathBuf, sync::Arc};

use tauri::{AppHandle, Emitter, State};

use super::{AdminGameHotkey, AdminGameHotkeyChange, AdminGameHotkeyOutcome};
use crate::{contracts::ClipSettings, settings::SettingsStore, state::AppState};

#[tauri::command]
pub fn get_admin_game_hotkey(state: State<'_, AppState>) -> AdminGameHotkey {
    state.admin_hotkey.snapshot(&state.settings.get(), &state.engine.cached_diagnostics())
}

/// "Not now" on the in-app card: no card or in-game notice for the rest of
/// this session. Settings still offer it.
#[tauri::command]
pub fn dismiss_admin_game_hotkey(state: State<'_, AppState>) {
    state.admin_hotkey.dismiss();
}

/// Sets the relay up behind Windows' administrator prompt, then turns the
/// setting on; when it is already set up, only turns it on. Never throws: the
/// outcome says what happened.
#[tauri::command]
pub async fn allow_admin_game_hotkey(app: AppHandle, state: State<'_, AppState>) -> Result<AdminGameHotkeyChange, ()> {
    let current = state.engine.cached_diagnostics();
    let relay = super::detail(&current, "adminHotkeyRelay");
    // Set up and working last time it ran: turning it on again needs no prompt.
    let reusable = super::relay_installed()
        && !matches!(relay.as_deref(), Some("outdated" | "failed" | "not-installed"));
    let installed = if reusable { Ok(Some(0)) } else { elevated(&state, "--install-hotkey-relay").await };
    let outcome = match installed {
        Ok(None) => declined(),
        Ok(Some(0)) => match set_enabled(&app, &state, true).await {
            Ok(()) => change(AdminGameHotkeyOutcome::Allowed, None),
            Err(message) => failed(message),
        },
        Ok(Some(3)) => failed(
            "Windows asked for a different account. Allow it while signed in to an administrator account.".into(),
        ),
        Ok(Some(code)) => failed(format!("The hotkey helper could not be set up (code {code}).")),
        Err(message) => failed(message),
    };
    Ok(outcome)
}

/// Off from the setting, without a prompt: the engine stops the relay and
/// Clipture no longer starts it. It stays set up, so turning it on again is instant.
#[tauri::command]
pub async fn disable_admin_game_hotkey(app: AppHandle, state: State<'_, AppState>) -> Result<AdminGameHotkeyChange, ()> {
    Ok(match set_enabled(&app, &state, false).await {
        Ok(()) => change(AdminGameHotkeyOutcome::Disabled, None),
        Err(message) => failed(message),
    })
}

/// Turns the setting off first, so the engine stops the relay, then removes
/// the relay behind Windows' prompt. Declining leaves it off but installed.
#[tauri::command]
pub async fn remove_admin_game_hotkey(app: AppHandle, state: State<'_, AppState>) -> Result<AdminGameHotkeyChange, ()> {
    if let Err(message) = set_enabled(&app, &state, false).await {
        return Ok(failed(message));
    }
    let outcome = match elevated(&state, "--uninstall-hotkey-relay").await {
        Ok(None) => declined(),
        Ok(Some(0)) => change(AdminGameHotkeyOutcome::Removed, None),
        Ok(Some(code)) => failed(format!("The hotkey helper could not be removed (code {code}).")),
        Err(message) => failed(message),
    };
    Ok(outcome)
}

fn change(outcome: AdminGameHotkeyOutcome, message: Option<String>) -> AdminGameHotkeyChange {
    AdminGameHotkeyChange { outcome, message }
}

fn declined() -> AdminGameHotkeyChange {
    change(AdminGameHotkeyOutcome::Declined, None)
}

fn failed(message: String) -> AdminGameHotkeyChange {
    change(AdminGameHotkeyOutcome::Failed, Some(message))
}

/// The engine executable the app runs, started elevated for setup or removal.
fn engine_executable(state: &AppState) -> Option<PathBuf> {
    state.paths.development_engine().or_else(|| {
        let executable = std::env::current_exe().ok()?;
        Some(executable.parent()?.join("clipture_engine.exe")).filter(|path| path.is_file())
    })
}

async fn elevated(state: &AppState, mode: &'static str) -> Result<Option<u32>, String> {
    let engine = engine_executable(state).ok_or_else(|| "Clipture's engine was not found.".to_owned())?;
    let sid = crate::platform::current_user_sid().ok_or_else(|| "Windows did not name the signed-in user.".to_owned())?;
    tauri::async_runtime::spawn_blocking(move || crate::platform::run_elevated(&engine, &format!("{mode} {sid}")))
        .await
        .map_err(|error| error.to_string())?
}

async fn set_enabled(app: &AppHandle, state: &AppState, enabled: bool) -> Result<(), String> {
    let store: Arc<SettingsStore> = state.settings.clone();
    let saved: ClipSettings = tauri::async_runtime::spawn_blocking(move || {
        let mut settings = store.get();
        settings.admin_game_hotkey = enabled;
        store.save(settings)
    })
    .await
    .map_err(|error| error.to_string())?
    .map_err(|error| error.to_string())?;
    let _ = app.emit("settings://changed", saved.clone());
    state.engine.configure_settings(&saved).await.map_err(|error| error.to_string())?;
    Ok(())
}
