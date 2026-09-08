use std::{path::PathBuf, process::Command};

use tauri::{AppHandle, State};
use tauri_plugin_shell::ShellExt;

use crate::{
    commands::{blocking, media::authorize_path, CommandResult},
    error::{AppError, AppResult},
    sounds::ClipSoundOption,
    state::AppState,
};

#[tauri::command]
pub async fn list_clip_sounds(state: State<'_, AppState>) -> CommandResult<Vec<ClipSoundOption>> {
    let sounds = state.sounds.clone();
    blocking(move || sounds.list()).await
}

#[tauri::command]
pub async fn import_clip_sound(
    app: AppHandle,
    state: State<'_, AppState>,
) -> CommandResult<Option<ClipSoundOption>> {
    if state.paths.test_mode {
        return Ok(None);
    }
    let picker = super::picker::desktop_picker(&app, "Import clip sound")
        .add_filter("Audio", &["mp3", "wav", "ogg"]);
    let selected = super::picker::select(|done| picker.pick_file(done))
        .await?
        .map(|path| path.into_path().map_err(|error| error.to_string()))
        .transpose()?;
    let Some(selected) = selected else {
        return Ok(None);
    };
    let sounds = state.sounds.clone();
    blocking(move || sounds.import(&selected).map(Some)).await
}

#[tauri::command]
pub fn reveal_sounds_folder(state: State<'_, AppState>) -> CommandResult<()> {
    if state.paths.test_mode {
        return Ok(());
    }
    std::fs::create_dir_all(state.sounds.sounds_dir()).map_err(|error| error.to_string())?;
    reveal(state.sounds.sounds_dir(), false).map_err(|error| error.to_string())
}

#[tauri::command]
pub async fn reveal_clip(state: State<'_, AppState>, file_path: String) -> CommandResult<()> {
    if state.paths.test_mode {
        return Ok(());
    }
    let library = state.library.clone();
    let settings = state.settings.get();
    let path = blocking(move || {
        let (authority, id) = authorize_path(&library, &settings, &file_path)?;
        authority
            .primary(&id)
            .map(PathBuf::from)
            .ok_or_else(|| AppError::Path("authorized clip disappeared".into()))
    })
    .await?;
    reveal(&path, true).map_err(|error| error.to_string())
}

#[tauri::command]
pub fn hide_notification(state: State<'_, AppState>) -> CommandResult<()> {
    state
        .notifications
        .hide()
        .map_err(|error| error.to_string())
}

#[tauri::command]
#[allow(deprecated)]
pub fn open_theme_font_download(
    app: AppHandle,
    state: State<'_, AppState>,
    theme: String,
) -> CommandResult<()> {
    let url = match theme.as_str() {
        "glitten" => "https://www.dafont.com/glitten.font",
        "milate" => "https://www.dafont.com/dh-milate.font",
        _ => return Err(format!("Unknown theme font: {theme}")),
    };
    if state.paths.test_mode {
        return Ok(());
    }
    app.shell()
        .open(url, None)
        .map_err(|error| error.to_string())
}

#[tauri::command]
pub async fn select_folder(
    app: AppHandle,
    state: State<'_, AppState>,
    current_path: String,
) -> CommandResult<Option<String>> {
    if state.paths.test_mode {
        return Ok(Some(current_path));
    }
    let mut picker = super::picker::desktop_picker(&app, "Select Save Folder");
    if !current_path.trim().is_empty() {
        picker = picker.set_directory(&current_path);
    }
    let selected = super::picker::select(|done| picker.pick_folder(done))
        .await?
        .map(|path| path.into_path().map_err(|error| error.to_string()))
        .transpose()?;
    Ok(Some(
        selected
            .map(|path| path.to_string_lossy().into_owned())
            .unwrap_or(current_path),
    ))
}

fn reveal(path: &std::path::Path, select_file: bool) -> AppResult<()> {
    if !path.exists() {
        return Err(AppError::Path(format!(
            "cannot reveal a missing path: {}",
            path.display()
        )));
    }
    let mut command = Command::new("explorer.exe");
    if select_file {
        command.arg(format!("/select,{}", path.display()));
    } else {
        command.arg(path);
    }
    configure_hidden(&mut command);
    command.spawn().map(|_| ()).map_err(|source| AppError::Io {
        action: "open Windows Explorer",
        path: path.to_owned(),
        source,
    })
}

#[cfg(windows)]
fn configure_hidden(command: &mut Command) {
    use std::os::windows::process::CommandExt;
    command.creation_flags(0x0800_0000);
}

#[cfg(not(windows))]
fn configure_hidden(_: &mut Command) {}
