mod app;
mod clips;
mod commands;
mod contracts;
mod diagnostics;
mod engine;
mod error;
mod library;
mod media;
pub mod migration;
mod notifications;
mod paths;
mod platform;
mod processes;
mod save;
mod settings;
mod sharing;
mod sounds;
mod state;
mod updates;

use tauri::{Manager, RunEvent};
use tauri_plugin_autostart::MacosLauncher;

pub fn installer_startup_command() -> Option<i32> {
    #[cfg(windows)]
    return app::installer_startup::command();
    #[cfg(not(windows))]
    None
}

pub fn run() {
    let _ = tracing_subscriber::fmt()
        .with_writer(std::io::stderr)
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| "clipture=info,warn".into()),
        )
        .try_init();
    if let Ok(paths) = paths::AppPaths::discover() {
        diagnostics::install_crash_log(&paths.data_dir);
    }
    #[cfg(windows)]
    match updates::activation::run_supervisor() {
        Ok(true) => return,
        Ok(false) => {}
        Err(error) => {
            tracing::error!(%error, "Runtime supervisor refused");
            std::process::exit(1);
        }
    }
    match updates::activation::startup() {
        Ok(false) => {}
        Ok(true) => return,
        Err(error) => {
            tracing::error!(%error, "Runtime activation refused");
            std::process::exit(1);
        }
    }
    if app::ui_process::run_if_worker() {
        return;
    }
    #[cfg(debug_assertions)]
    if updates::smoke::run_if_requested() {
        return;
    }

    let builder = media::register_protocol(tauri::Builder::default(), |app| {
        app.try_state::<state::AppState>()
            .map(|state| state.media.clone())
    });
    let application = builder
        // This plugin must remain first so duplicate processes cannot initialize
        // the engine or touch persistent state.
        .plugin(tauri_plugin_single_instance::init(
            |app, arguments, _working_directory| {
                app::handle_second_instance(app, &arguments);
            },
        ))
        .plugin(tauri_plugin_updater::Builder::new().build())
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_shell::init())
        .plugin(tauri_plugin_autostart::init(
            MacosLauncher::LaunchAgent,
            Some(vec!["--hidden"]),
        ))
        .invoke_handler(tauri::generate_handler![
            commands::host_info,
            commands::get_settings,
            commands::save_settings,
            commands::configure_engine,
            commands::get_diagnostics,
            commands::export_diagnostics,
            commands::get_save_io_analyzer_state,
            commands::set_save_io_analyzer_armed,
            commands::list_audio_input_devices,
            commands::list_display_devices,
            commands::save_clip,
            commands::list_clips,
            commands::delete_clips,
            commands::import_video_folders,
            commands::rename_clip,
            commands::clip_url,
            commands::clip_icon_url,
            commands::process_icon_url,
            commands::clip_thumbnail_url,
            commands::clip_playback_url,
            commands::release_playback_cache,
            commands::get_clip_repair_status,
            commands::check_clip_layouts,
            commands::fix_clip_layouts,
            commands::list_active_processes,
            commands::list_clip_sounds,
            commands::import_clip_sound,
            commands::reveal_sounds_folder,
            commands::reveal_clip,
            commands::sharing_get_state,
            commands::sharing_set_enabled,
            commands::sharing_set_appear_offline,
            commands::sharing_set_display_name,
            commands::sharing_add_friend,
            commands::sharing_accept_friend,
            commands::sharing_accept_invite,
            commands::sharing_dismiss_invite,
            commands::sharing_remove_friend,
            commands::sharing_share_clip,
            commands::sharing_revoke_share,
            commands::sharing_dismiss_clip,
            commands::sharing_save_clip,
            commands::sharing_cancel_download,
            commands::sharing_stream_url,
            updates::commands::get_update_state,
            updates::commands::check_for_updates,
            updates::commands::download_update,
            updates::commands::install_update,
            commands::open_theme_font_download,
            commands::hide_notification,
            commands::select_folder,
            commands::open_main_window,
            commands::close_main_window,
            commands::exit_app,
        ])
        .setup(|application| {
            if updates::activation::activate_at_launch()? {
                std::process::exit(0);
            }
            app::setup(application)?;
            Ok(())
        })
        .on_window_event(app::windows::handle_window_event)
        .build(tauri::generate_context!())
        .expect("failed to build the Clipture Tauri host");

    application.run(|app_handle, event| match event {
        RunEvent::Ready => {
            if let Err(error) = updates::startup_health::host_ready() {
                tracing::error!(%error, "Runtime health acknowledgement failed");
                app::request_exit(app_handle);
            }
        }
        RunEvent::ExitRequested { api, .. } => {
            let exiting = app_handle
                .try_state::<state::AppState>()
                .is_some_and(|state| state.is_exiting());
            if !exiting {
                api.prevent_exit();
            }
        }
        RunEvent::Exit => {
            app::ui_process::shutdown(app_handle);
            if let Some(state) = app_handle.try_state::<state::AppState>() {
                state.sharing.shutdown();
                state.engine.shutdown();
            }
        }
        _ => {}
    });
}

#[cfg(test)]
mod tests {
    #[test]
    fn startup_configuration_has_no_eager_webviews() {
        let config: serde_json::Value =
            serde_json::from_str(include_str!("../tauri.conf.json")).unwrap();
        assert_eq!(config["app"]["windows"], serde_json::json!([]));
    }
}
