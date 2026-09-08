use std::path::PathBuf;

use tauri::{AppHandle, Emitter, State};

use crate::{
    commands::{blocking, CommandResult},
    contracts::{ClipRecord, ClipSettings},
    state::AppState,
};

#[tauri::command]
pub async fn list_clips(state: State<'_, AppState>) -> CommandResult<Vec<ClipRecord>> {
    let library = state.library.clone();
    let settings = state.settings.get();
    blocking(move || library.list_refreshed(&settings)).await
}

#[tauri::command]
pub async fn delete_clips(
    app: AppHandle,
    state: State<'_, AppState>,
    ids: Vec<String>,
) -> CommandResult<bool> {
    if ids.is_empty() {
        return Ok(false);
    }
    let clips = state.clips.clone();
    let library = state.library.clone();
    let settings_store = state.settings.clone();
    let settings = state.settings.get();
    let (changed, saved_settings) = blocking(move || {
        let saved = clips.delete_saved(&ids)?;
        let imported = library.delete_imported(&settings, &ids)?;
        let persisted = imported
            .settings
            .map(|settings| settings_store.save(settings))
            .transpose()?;
        Ok((saved.changed || imported.changed, persisted))
    })
    .await?;
    emit_mutation(&app, changed, saved_settings);
    Ok(changed)
}

#[tauri::command]
pub async fn import_video_folders(
    app: AppHandle,
    state: State<'_, AppState>,
) -> CommandResult<bool> {
    if state.paths.test_mode {
        return Ok(false);
    }
    let settings = state.settings.get();
    let picker = super::picker::desktop_picker(&app, "Import video folders")
        .set_directory(&settings.save_folder);
    let selected = super::picker::select(|done| picker.pick_folders(done))
        .await?
        .unwrap_or_default()
        .into_iter()
        .map(|path| path.into_path().map_err(|error| error.to_string()))
        .collect::<Result<Vec<PathBuf>, _>>()?;
    if selected.is_empty() {
        return Ok(false);
    }
    let library = state.library.clone();
    let settings_store = state.settings.clone();
    let (changed, saved_settings) = blocking(move || {
        let mutation = library.add_import_roots(&settings, &selected)?;
        let persisted = mutation
            .settings
            .map(|settings| settings_store.save(settings))
            .transpose()?;
        Ok((mutation.changed, persisted))
    })
    .await?;
    emit_mutation(&app, changed, saved_settings);
    Ok(changed)
}

#[tauri::command]
pub async fn rename_clip(
    app: AppHandle,
    state: State<'_, AppState>,
    id: String,
    new_title: String,
) -> CommandResult<bool> {
    let clips = state.clips.clone();
    let library = state.library.clone();
    let settings_store = state.settings.clone();
    let settings = state.settings.get();
    let imported = id.starts_with("imported:");
    let (changed, saved_settings) = blocking(move || {
        if imported {
            let mutation = library.rename_imported(&settings, &id, &new_title)?;
            let persisted = mutation
                .settings
                .map(|settings| settings_store.save(settings))
                .transpose()?;
            Ok((mutation.changed, persisted))
        } else {
            Ok((clips.rename_saved(&id, &new_title)?, None))
        }
    })
    .await?;
    emit_mutation(&app, changed, saved_settings);
    Ok(changed)
}

fn emit_mutation(app: &AppHandle, changed: bool, settings: Option<ClipSettings>) {
    if let Some(settings) = settings {
        let _ = app.emit("settings://changed", settings);
    }
    if changed {
        let _ = app.emit("library://changed", Option::<ClipRecord>::None);
    }
}
