//! Thin private transport adapter. Existing commands and domain services remain
//! the only owners of business rules and persistent effects.
use crate::{commands, state::AppState, updates};
use serde::{de::DeserializeOwned, Serialize};
use serde_json::Value;
use tauri::{AppHandle, Manager};

pub async fn invoke(
    app: AppHandle,
    owner: String,
    command: &str,
    args: Value,
) -> Result<Value, String> {
    if !args.is_object() {
        return Err("UI command arguments must be an object".into());
    }
    let state = app.state::<AppState>();
    match command {
        "host_info" => encode(commands::host_info(app.clone(), state)),
        "get_settings" => encode(commands::get_settings(state)),
        "save_settings" => {
            encode(commands::save_settings(app.clone(), state, argument(&args, "settings")?).await?)
        }
        "configure_engine" => encode(commands::configure_engine(state).await?),
        "get_diagnostics" => encode(commands::get_diagnostics(state).await?),
        "export_diagnostics" => encode(commands::export_diagnostics(app.clone(), state).await?),
        "get_save_io_analyzer_state" => encode(commands::get_save_io_analyzer_state(state)),
        "set_save_io_analyzer_armed" => encode(commands::set_save_io_analyzer_armed(
            state,
            argument(&args, "armed")?,
        )),
        "list_audio_input_devices" => encode(commands::list_audio_input_devices(state).await?),
        "list_display_devices" => encode(commands::list_display_devices(state).await?),
        "save_clip" => {
            let duration = argument::<Option<u32>>(&args, "durationSeconds")?
                .unwrap_or(state.settings.get().clip_length_seconds);
            encode(commands::save_clip(app.clone(), state, duration).await?)
        }
        "list_clips" => encode(commands::list_clips(state).await?),
        "delete_clips" => {
            encode(commands::delete_clips(app.clone(), state, argument(&args, "ids")?).await?)
        }
        "import_video_folders" => encode(commands::import_video_folders(app.clone(), state).await?),
        "rename_clip" => encode(
            commands::rename_clip(
                app.clone(),
                state,
                argument(&args, "id")?,
                argument(&args, "newTitle")?,
            )
            .await?,
        ),
        "clip_url" => {
            encode(commands::clip_url_for_owner(owner, state, argument(&args, "filePath")?).await?)
        }
        "clip_playback_url" => encode(
            commands::clip_playback_for_owner(
                owner,
                state,
                argument(&args, "filePath")?,
                argument(&args, "audioTracks")?,
            )
            .await?,
        ),
        "release_playback_cache" => encode(state.media.release_owner(&owner) > 0),
        "get_clip_repair_status" => encode(commands::get_clip_repair_status(state)),
        "check_clip_layouts" => encode(commands::check_clip_layouts(state).await?),
        "fix_clip_layouts" => encode(commands::fix_clip_layouts(state)),
        "clip_thumbnail_url" => {
            encode(commands::clip_thumbnail_url(state, argument(&args, "filePath")?).await?)
        }
        "clip_icon_url" => encode(
            commands::clip_icon_url(
                state,
                argument(&args, "clip")?,
                argument(&args, "preferredLabels")?,
            )
            .await?,
        ),
        "process_icon_url" => encode(
            commands::process_icon_url(
                state,
                argument(&args, "processName")?,
                argument(&args, "executablePath")?,
            )
            .await?,
        ),
        "list_active_processes" => encode(commands::list_active_processes(state).await?),
        "list_clip_sounds" => encode(commands::list_clip_sounds(state).await?),
        "import_clip_sound" => encode(commands::import_clip_sound(app.clone(), state).await?),
        "reveal_sounds_folder" => encode(commands::reveal_sounds_folder(state)?),
        "reveal_clip" => encode(commands::reveal_clip(state, argument(&args, "filePath")?).await?),
        "sharing_get_state" => encode(commands::sharing_get_state(state)),
        "sharing_set_enabled" => {
            encode(commands::sharing_set_enabled(state, argument(&args, "enabled")?)?)
        }
        "sharing_set_appear_offline" => encode(commands::sharing_set_appear_offline(
            state,
            argument(&args, "appearOffline")?,
        )?),
        "sharing_set_display_name" => {
            encode(commands::sharing_set_display_name(state, argument(&args, "name")?)?)
        }
        "sharing_add_friend" => encode(commands::sharing_add_friend(
            state,
            argument(&args, "code")?,
            argument(&args, "name")?,
        )?),
        "sharing_accept_invite" => encode(commands::sharing_accept_invite(state)?),
        "sharing_dismiss_invite" => encode(commands::sharing_dismiss_invite(state)),
        "sharing_set_nickname" => encode(commands::sharing_set_nickname(
            state,
            argument(&args, "friendId")?,
            argument(&args, "nickname")?,
        )?),
        "sharing_accept_friend" => {
            encode(commands::sharing_accept_friend(state, argument(&args, "friendId")?)?)
        }
        "sharing_remove_friend" => {
            encode(commands::sharing_remove_friend(state, argument(&args, "friendId")?)?)
        }
        "sharing_share_clip" => encode(
            commands::sharing_share_clip(
                state,
                argument(&args, "friendId")?,
                argument(&args, "filePath")?,
            )
            .await?,
        ),
        "sharing_revoke_share" => {
            encode(commands::sharing_revoke_share(state, argument(&args, "shareId")?)?)
        }
        "sharing_dismiss_clip" => {
            encode(commands::sharing_dismiss_clip(state, argument(&args, "shareId")?)?)
        }
        "sharing_answer_clip" => encode(commands::sharing_answer_clip(
            state,
            argument(&args, "shareId")?,
            argument(&args, "accept")?,
        )?),
        "sharing_cancel_download" => {
            encode(commands::sharing_cancel_download(state, argument(&args, "shareId")?)?)
        }
        "sharing_save_clip" => {
            encode(commands::sharing_save_clip(state, argument(&args, "shareId")?)?)
        }
        "sharing_stream_url" => encode(commands::sharing_stream_url_for_owner(
            &owner,
            state,
            argument(&args, "shareId")?,
        )?),
        "hide_notification" => encode(commands::hide_notification(state)?),
        "open_theme_font_download" => encode(commands::open_theme_font_download(
            app.clone(),
            state,
            argument(&args, "theme")?,
        )?),
        "select_folder" => encode(
            commands::select_folder(app.clone(), state, argument(&args, "currentPath")?).await?,
        ),
        "get_update_state" => encode(updates::commands::get_update_state(app.state())),
        "check_for_updates" => encode(
            updates::commands::check_for_updates(app.clone(), app.state())
                .await
                .map_err(|error| error.to_string())?,
        ),
        "download_update" => encode(
            updates::commands::download_update(app.clone(), app.state())
                .await
                .map_err(|error| error.to_string())?,
        ),
        "install_update" => encode(
            updates::commands::install_update(app.clone(), app.state())
                .await
                .map_err(|error| error.to_string())?,
        ),
        "open_main_window" => encode(commands::open_main_window(app.clone())?),
        "close_main_window" => encode(commands::close_main_window(app.clone())?),
        "exit_app" => {
            super::super::request_exit(&app);
            Ok(Value::Null)
        }
        _ => Err(format!("Unknown UI command: {command}")),
    }
}

fn argument<T: DeserializeOwned>(args: &Value, name: &str) -> Result<T, String> {
    serde_json::from_value(args.get(name).cloned().unwrap_or(Value::Null))
        .map_err(|error| format!("Invalid {name}: {error}"))
}

fn encode(value: impl Serialize) -> Result<Value, String> {
    serde_json::to_value(value).map_err(|error| error.to_string())
}
