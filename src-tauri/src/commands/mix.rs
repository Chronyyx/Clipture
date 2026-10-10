use serde::Serialize;
use tauri::{AppHandle, Emitter, State};

use crate::{
    commands::{blocking, CommandResult},
    contracts::ClipRecord,
    error::AppError,
    media::MAXIMUM_GAIN,
    state::AppState,
};

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ClipMixResult {
    pub tracks_changed: usize,
    pub bytes_before: u64,
    pub bytes_after: u64,
}

/// Re-encodes each audio track of one of Clipture's own clips at its level
/// in `gains` (by track position; 0 silences it, 1 leaves it as is), for
/// good (see `media::mix`).
#[tauri::command]
pub async fn mix_clip_audio(
    app: AppHandle,
    state: State<'_, AppState>,
    id: String,
    gains: Vec<f64>,
) -> CommandResult<ClipMixResult> {
    if id.len() > 512 || gains.len() > 64 || gains.iter().any(|gain| !gain.is_finite() || !(0.0..=MAXIMUM_GAIN).contains(gain)) {
        return Err("Invalid audio mix request".into());
    }
    let library = state.library.clone();
    let media = state.media.clone();
    let sharing = state.sharing.clone();
    let settings = state.settings.get();
    let result = blocking(move || {
        let authority = library.path_authorizer(&settings)?;
        let clip = authority
            .clip(&id)
            .ok_or_else(|| AppError::Path("that clip is no longer in the library".into()))?;
        if clip.imported {
            return Err(AppError::Path("Only Clipture's own clips can be remixed; imported videos are left as they are.".into()));
        }
        if !clip.segments.is_empty() {
            return Err(AppError::Path("Clips saved in several parts cannot be remixed yet.".into()));
        }
        let path = clip.primary.clone();
        let mixed = media.mix(&path, &gains).map_err(|error| AppError::Path(error.to_string()))?;
        if mixed.tracks_changed > 0 {
            // Friends were offered the clip as it was; those offers are void.
            if let Err(error) = sharing.revoke_shares_of(&path) {
                tracing::warn!(%error, "could not end the shares of a remixed clip");
            }
        }
        Ok(ClipMixResult {
            tracks_changed: mixed.tracks_changed,
            bytes_before: mixed.bytes_before,
            bytes_after: mixed.bytes_after,
        })
    })
    .await?;
    if result.tracks_changed > 0 {
        let _ = app.emit("library://changed", Option::<ClipRecord>::None);
    }
    Ok(result)
}
