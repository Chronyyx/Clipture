use serde::Serialize;
use tauri::{AppHandle, Emitter, State};

use crate::{
    commands::{blocking, CommandResult},
    contracts::ClipRecord,
    error::AppError,
    state::AppState,
};

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ClipTrimResult {
    pub duration_seconds: f64,
    pub bytes_before: u64,
    pub bytes_after: u64,
}

/// Keeps `[startSeconds, endSeconds)` of one of Clipture's own clips and
/// permanently deletes the rest from its file (see `media::trim`).
#[tauri::command]
pub async fn trim_clip(
    app: AppHandle,
    state: State<'_, AppState>,
    id: String,
    start_seconds: f64,
    end_seconds: f64,
) -> CommandResult<ClipTrimResult> {
    if !start_seconds.is_finite() || !end_seconds.is_finite() || id.len() > 512 {
        return Err("Invalid trim request".into());
    }
    let library = state.library.clone();
    let media = state.media.clone();
    let clips = state.clips.clone();
    let sharing = state.sharing.clone();
    let settings = state.settings.get();
    let result = blocking(move || {
        let authority = library.path_authorizer(&settings)?;
        let clip = authority
            .clip(&id)
            .ok_or_else(|| AppError::Path("that clip is no longer in the library".into()))?;
        if clip.imported {
            return Err(AppError::Path("Only Clipture's own clips can be trimmed; imported videos are left as they are.".into()));
        }
        if !clip.segments.is_empty() {
            return Err(AppError::Path("Clips saved in several parts cannot be trimmed yet.".into()));
        }
        let path = clip.primary.clone();
        let trimmed = media
            .trim(&path, start_seconds, end_seconds)
            .map_err(|error| AppError::Path(error.to_string()))?;
        clips.set_trimmed_duration(&path, trimmed.duration_seconds)?;
        // Friends were offered the clip as it was; those offers are void.
        if let Err(error) = sharing.revoke_shares_of(&path) {
            tracing::warn!(%error, "could not end the shares of a trimmed clip");
        }
        Ok(ClipTrimResult {
            duration_seconds: trimmed.duration_seconds,
            bytes_before: trimmed.bytes_before,
            bytes_after: trimmed.bytes_after,
        })
    })
    .await?;
    let _ = app.emit("library://changed", Option::<ClipRecord>::None);
    Ok(result)
}
