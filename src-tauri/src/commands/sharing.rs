use std::path::PathBuf;

use serde::Serialize;
use tauri::{State, WebviewWindow};

use crate::{
    commands::{blocking, media::authorize_path, CommandResult},
    error::AppError,
    sharing::{is_friend_id, valid_share_id, FriendStatus, ShareSource, SharingSnapshot},
    state::AppState,
};

const MAX_INPUT_CHARS: usize = 120;
const MAX_LINK_BYTES: usize = 512;

/// A clip offered to a friend: its share, to follow its progress.
#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SharedClipResult {
    share_id: String,
    snapshot: SharingSnapshot,
}

#[tauri::command]
pub fn sharing_get_state(state: State<'_, AppState>) -> SharingSnapshot {
    state.sharing.snapshot()
}

#[tauri::command]
pub fn sharing_set_enabled(
    state: State<'_, AppState>,
    enabled: bool,
) -> CommandResult<SharingSnapshot> {
    state
        .sharing
        .set_enabled(enabled)
        .map_err(|error| error.to_string())?;
    Ok(state.sharing.snapshot())
}

#[tauri::command]
pub fn sharing_set_appear_offline(
    state: State<'_, AppState>,
    appear_offline: bool,
) -> CommandResult<SharingSnapshot> {
    state
        .sharing
        .set_appear_offline(appear_offline)
        .map_err(|error| error.to_string())?;
    Ok(state.sharing.snapshot())
}

#[tauri::command]
pub fn sharing_set_display_name(
    state: State<'_, AppState>,
    name: String,
) -> CommandResult<SharingSnapshot> {
    bounded(&name)?;
    state
        .sharing
        .set_display_name(&name)
        .map_err(|error| error.to_string())?;
    Ok(state.sharing.snapshot())
}

#[tauri::command]
pub fn sharing_add_friend(
    state: State<'_, AppState>,
    code: String,
    name: String,
) -> CommandResult<FriendStatus> {
    // Room for a pasted invite link, not just a bare code.
    if code.len() > MAX_LINK_BYTES {
        return Err("input is too long".into());
    }
    bounded(&name)?;
    state
        .sharing
        .add_friend(&code, &name)
        .map_err(|error| error.to_string())
}

/// Confirms the invite link that opened Clipture (turning sharing on if needed).
#[tauri::command]
pub fn sharing_accept_invite(state: State<'_, AppState>) -> CommandResult<SharingSnapshot> {
    state
        .sharing
        .accept_invite()
        .map_err(|error| error.to_string())?;
    Ok(state.sharing.snapshot())
}

#[tauri::command]
pub fn sharing_dismiss_invite(state: State<'_, AppState>) -> SharingSnapshot {
    state.sharing.dismiss_invite();
    state.sharing.snapshot()
}

#[tauri::command]
pub fn sharing_accept_friend(
    state: State<'_, AppState>,
    friend_id: String,
) -> CommandResult<SharingSnapshot> {
    friend(&friend_id)?;
    state
        .sharing
        .accept_friend(&friend_id)
        .map_err(|error| error.to_string())?;
    Ok(state.sharing.snapshot())
}

#[tauri::command]
pub fn sharing_remove_friend(
    state: State<'_, AppState>,
    friend_id: String,
) -> CommandResult<SharingSnapshot> {
    friend(&friend_id)?;
    state
        .sharing
        .remove_friend(&friend_id)
        .map_err(|error| error.to_string())?;
    Ok(state.sharing.snapshot())
}

/// Offers a library clip to a friend. The renderer's path is only a lookup
/// key into the authorized library snapshot, as for playback.
#[tauri::command]
pub async fn sharing_share_clip(
    state: State<'_, AppState>,
    friend_id: String,
    file_path: String,
) -> CommandResult<SharedClipResult> {
    friend(&friend_id)?;
    let library = state.library.clone();
    let sharing = state.sharing.clone();
    let settings = state.settings.get();
    blocking(move || {
        let (authority, id) = authorize_path(&library, &settings, &file_path)?;
        let record = library
            .list_cached(&settings)?
            .into_iter()
            .find(|clip| clip.id == id)
            .ok_or_else(|| AppError::Path("that clip is no longer in the library".into()))?;
        if record
            .segment_files
            .as_ref()
            .is_some_and(|files| !files.is_empty())
        {
            return Err(AppError::Path("this clip is still being processed".into()));
        }
        let path = authority
            .primary(&id)
            .map(PathBuf::from)
            .ok_or_else(|| AppError::Path("that clip is no longer in the library".into()))?;
        let share_id = sharing.share_clip(
            &friend_id,
            ShareSource {
                path,
                title: record.title,
                duration_seconds: record.duration_seconds,
                resolution: record.resolution,
                game_or_app: record.game_or_app,
                fps: record.fps,
                audio_tracks: record.audio_tracks,
            },
        )?;
        Ok(SharedClipResult {
            share_id,
            snapshot: sharing.snapshot(),
        })
    })
    .await
}

/// Accepts or declines a clip a friend wants to send.
#[tauri::command]
pub fn sharing_answer_clip(
    state: State<'_, AppState>,
    share_id: String,
    accept: bool,
) -> CommandResult<SharingSnapshot> {
    share(&share_id)?;
    state
        .sharing
        .answer_shared_clip(&share_id, accept)
        .map_err(|error| error.to_string())?;
    Ok(state.sharing.snapshot())
}

#[tauri::command]
pub fn sharing_revoke_share(
    state: State<'_, AppState>,
    share_id: String,
) -> CommandResult<SharingSnapshot> {
    share(&share_id)?;
    state
        .sharing
        .revoke_share(&share_id)
        .map_err(|error| error.to_string())?;
    Ok(state.sharing.snapshot())
}

#[tauri::command]
pub fn sharing_dismiss_clip(
    state: State<'_, AppState>,
    share_id: String,
) -> CommandResult<SharingSnapshot> {
    share(&share_id)?;
    state
        .sharing
        .dismiss_shared_clip(&share_id)
        .map_err(|error| error.to_string())?;
    Ok(state.sharing.snapshot())
}

#[tauri::command]
pub fn sharing_cancel_download(
    state: State<'_, AppState>,
    share_id: String,
) -> CommandResult<SharingSnapshot> {
    share(&share_id)?;
    state.sharing.cancel_download(&share_id);
    Ok(state.sharing.snapshot())
}

#[tauri::command]
pub fn sharing_save_clip(
    state: State<'_, AppState>,
    share_id: String,
) -> CommandResult<SharingSnapshot> {
    share(&share_id)?;
    state
        .sharing
        .save_shared_clip(&share_id)
        .map_err(|error| error.to_string())?;
    Ok(state.sharing.snapshot())
}

#[tauri::command]
pub fn sharing_stream_url(
    window: WebviewWindow,
    state: State<'_, AppState>,
    share_id: String,
) -> CommandResult<String> {
    sharing_stream_url_for_owner(window.label(), state, share_id)
}

/// Stream sessions belong to the requesting UI, like playback sessions.
pub(crate) fn sharing_stream_url_for_owner(
    owner: &str,
    state: State<'_, AppState>,
    share_id: String,
) -> CommandResult<String> {
    share(&share_id)?;
    let stream_id = state
        .sharing
        .open_stream(owner, &share_id)
        .map_err(|error| error.to_string())?;
    Ok(state.media.remote_url(&stream_id))
}

fn bounded(value: &str) -> CommandResult<()> {
    if value.chars().count() > MAX_INPUT_CHARS {
        return Err("input is too long".into());
    }
    Ok(())
}

fn friend(id: &str) -> CommandResult<()> {
    if !is_friend_id(id) {
        return Err("unknown friend".into());
    }
    Ok(())
}

fn share(id: &str) -> CommandResult<()> {
    if !valid_share_id(id) {
        return Err("unknown shared clip".into());
    }
    Ok(())
}
