pub mod autostart;
#[cfg(windows)]
pub(crate) mod installer_startup;
mod background;
#[cfg(all(debug_assertions, windows))]
mod capture_smoke;
mod hotkeys;
mod sharing_events;
#[cfg(debug_assertions)]
mod smoke;
mod tray;
pub(crate) mod ui_process;
pub(crate) mod window_appearance;
pub mod windows;

use std::sync::Arc;

use tauri::{App, AppHandle, Manager};

use crate::{
    engine::EngineClient, error::AppResult, notifications::SilentNotificationSink, paths::AppPaths,
    platform::NativeIconSource, settings::SettingsStore, state::AppState, updates,
};

pub fn setup(application: &mut App) -> AppResult<()> {
    let paths = AppPaths::discover()?;
    application
        .asset_protocol_scope()
        .allow_directory(&paths.sounds_dir, false)
        .map_err(|error| crate::error::AppError::Integration(error.to_string()))?;
    let test_mode = paths.test_mode;
    let restricted_profile = test_mode || paths.isolated_profile;
    let settings = Arc::new(SettingsStore::load(paths.clone())?);
    let engine = EngineClient::new(application.handle().clone(), paths.clone());
    let notification_sink: Arc<dyn crate::notifications::NotificationSink> = {
        #[cfg(windows)]
        if !test_mode {
            Arc::new(crate::platform::windows::NativeNotificationSink::new(
                application.handle().clone(),
            ))
        } else {
            Arc::new(SilentNotificationSink)
        }
        #[cfg(not(windows))]
        {
            Arc::new(SilentNotificationSink)
        }
    };
    let state = AppState::new(
        paths,
        settings,
        engine,
        Arc::new(NativeIconSource),
        notification_sink,
        Box::new(sharing_events::TauriSharingEvents {
            app: application.handle().clone(),
            cues: share_cues(test_mode),
        }),
        tauri::async_runtime::handle().inner().clone(),
    );
    let update_service = updates::runtime_service(
        state.engine.clone(),
        state.saves.clone(),
        restricted_profile,
    );
    let sharing = state.sharing.clone();
    application.manage(state);
    application.manage(ui_process::UiController::default());
    application.manage(update_service);
    #[cfg(debug_assertions)]
    smoke::install(application.handle());
    #[cfg(all(debug_assertions, windows))]
    capture_smoke::install(application.handle());

    tray::create(application.handle())?;
    background::start(application.handle().clone());
    sharing.resume();
    updates::start_background_checks(
        application.handle().clone(),
        updates::automatic_checks_enabled(restricted_profile),
    );
    // An explicit launch opens the UI; startup registration passes --hidden.
    handle_second_instance(application.handle(), &std::env::args().collect::<Vec<_>>());
    Ok(())
}

/// Sharing sounds play natively; test mode stays silent.
fn share_cues(test_mode: bool) -> Option<Arc<crate::sounds::CuePlayer>> {
    #[cfg(windows)]
    if !test_mode {
        return Some(Arc::new(crate::sounds::CuePlayer::new(Arc::new(
            crate::platform::windows::WindowsWaveSoundSink,
        ))));
    }
    let _ = test_mode;
    None
}

pub fn handle_second_instance(app: &AppHandle, arguments: &[String]) {
    // An invite link (clipture://add/...) always opens the UI to confirm it.
    let invite = crate::sharing::invite_argument(arguments);
    if let (Some(invite), Some(state)) = (invite.clone(), app.try_state::<AppState>()) {
        state.sharing.receive_invite(invite);
    }
    let background_launch = arguments
        .iter()
        .any(|argument| matches!(argument.as_str(), "--hidden" | "--background"));
    if !background_launch || invite.is_some() {
        if let Err(error) = windows::open_main(app) {
            tracing::error!(%error, "could not open the main window for second instance");
        }
    }
}

pub fn request_exit(app: &AppHandle) {
    if let Some(state) = app.try_state::<AppState>() {
        state.begin_exit();
        ui_process::shutdown(app);
        state.engine.shutdown();
    }
    app.exit(0);
}
