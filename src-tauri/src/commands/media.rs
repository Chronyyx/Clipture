use std::path::{Path, PathBuf};

use tauri::{State, WebviewWindow};

use crate::{
    clips::PathAuthorizer,
    commands::{blocking, CommandResult},
    contracts::ClipRecord,
    error::{AppError, AppResult},
    media::PlaybackDescriptor,
    processes::ActiveProcess,
    state::AppState,
};

#[tauri::command]
pub async fn clip_url(
    window: WebviewWindow,
    state: State<'_, AppState>,
    file_path: String,
) -> CommandResult<String> {
    clip_url_for_owner(window.label().to_owned(), state, file_path).await
}

pub(crate) async fn clip_url_for_owner(
    owner: String,
    state: State<'_, AppState>,
    file_path: String,
) -> CommandResult<String> {
    let library = state.library.clone();
    let media = state.media.clone();
    let settings = state.settings.get();
    blocking(move || {
        let (authority, id) = authorize_path(&library, &settings, &file_path)?;
        media
            .open_playback(&authority, &id, &[], &owner)
            .map(|descriptor| descriptor.url)
    })
    .await
}

#[tauri::command]
pub async fn clip_thumbnail_url(
    state: State<'_, AppState>,
    file_path: String,
) -> CommandResult<String> {
    let library = state.library.clone();
    let media = state.media.clone();
    let settings = state.settings.get();
    blocking(move || {
        let (authority, id) = authorize_path(&library, &settings, &file_path)?;
        media.thumbnail(&authority, &id)
    })
    .await
}

#[tauri::command]
pub async fn clip_playback_url(
    window: WebviewWindow,
    state: State<'_, AppState>,
    file_path: String,
    audio_tracks: Vec<String>,
) -> CommandResult<PlaybackDescriptor> {
    clip_playback_for_owner(window.label().to_owned(), state, file_path, audio_tracks).await
}

pub(crate) async fn clip_playback_for_owner(
    owner: String,
    state: State<'_, AppState>,
    file_path: String,
    audio_tracks: Vec<String>,
) -> CommandResult<PlaybackDescriptor> {
    let library = state.library.clone();
    let media = state.media.clone();
    let settings = state.settings.get();
    blocking(move || {
        let (authority, id) = authorize_path(&library, &settings, &file_path)?;
        media.open_playback(&authority, &id, &audio_tracks, &owner)
    })
    .await
}

#[tauri::command]
pub fn release_playback_cache(window: WebviewWindow, state: State<'_, AppState>) -> bool {
    state.media.release_owner(window.label()) > 0
}

#[tauri::command]
pub async fn clip_icon_url(
    state: State<'_, AppState>,
    clip: ClipRecord,
    preferred_labels: Option<Vec<String>>,
) -> CommandResult<String> {
    refresh_process_paths(&state).await;
    let library = state.library.clone();
    let icons = state.process_icons.clone();
    let settings = state.settings.get();
    blocking(move || {
        let records = library.list_refreshed(&settings)?;
        let authority = PathAuthorizer::from_records(records.clone());
        let id = authority
            .clip_id_for_path(Path::new(&clip.file_path))
            .ok_or_else(|| {
                AppError::Path("clip is not in the authorized library snapshot".into())
            })?;
        let record = records
            .iter()
            .find(|record| record.id == id)
            .ok_or_else(|| AppError::Path("authorized clip record is unavailable".into()))?;
        icons.clip_icon(record, preferred_labels.as_deref().unwrap_or_default())
    })
    .await
}

#[tauri::command]
pub async fn process_icon_url(
    state: State<'_, AppState>,
    process_name: String,
    executable_path: Option<String>,
) -> CommandResult<String> {
    refresh_process_paths(&state).await;
    let icons = state.process_icons.clone();
    blocking(move || icons.process_icon(&process_name, executable_path.as_deref())).await
}

#[tauri::command]
pub async fn list_active_processes(
    state: State<'_, AppState>,
) -> CommandResult<Vec<ActiveProcess>> {
    match state.engine.list_running_processes(true).await {
        Ok(processes) if !processes.is_empty() => Ok(state.processes.remember(processes)),
        Ok(_) | Err(_) => {
            let processes = state.processes.clone();
            blocking(move || processes.list()).await
        }
    }
}

async fn refresh_process_paths(state: &AppState) {
    if let Ok(processes) = state.engine.list_running_processes(true).await {
        if !processes.is_empty() {
            state.processes.remember(processes);
        }
    }
}

pub(crate) fn authorize_path(
    library: &crate::library::LibraryService,
    settings: &crate::contracts::ClipSettings,
    requested_path: &str,
) -> AppResult<(PathAuthorizer, String)> {
    let authority = library.path_authorizer(settings)?;
    let id = authority
        .clip_id_for_path(&PathBuf::from(requested_path))
        .ok_or_else(|| {
            AppError::Path("media path is not in the authorized library snapshot".into())
        })?
        .to_owned();
    Ok((authority, id))
}
