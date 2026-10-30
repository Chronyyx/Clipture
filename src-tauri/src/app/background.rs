use tauri::{AppHandle, Manager};

use crate::{contracts::SaveSource, state::AppState};

pub fn start(app: AppHandle) {
    let state = app.state::<AppState>();
    let engine = state.engine.clone();
    let settings_store = state.settings.clone();
    let save_coordinator = state.saves.clone();
    let test_mode = state.paths.test_mode;
    let mut hotkeys = engine.subscribe_hotkeys();

    if !test_mode && !state.paths.isolated_profile {
        if let Err(error) = super::autostart::apply(&app, settings_store.get().start_on_login) {
            tracing::warn!(%error, "could not synchronize autostart registration");
        }
    }

    let configure_engine = engine.clone();
    let startup_settings = settings_store.get();
    if !test_mode {
        let diagnostics_engine = engine.clone();
        let notice_settings = settings_store.clone();
        let admin_hotkey = state.admin_hotkey.clone();
        let notifications = state.notifications.clone();
        tauri::async_runtime::spawn(async move {
            loop {
                tokio::time::sleep(std::time::Duration::from_secs(2)).await;
                let Ok(diagnostics) = diagnostics_engine.diagnostics().await else { continue };
                // An admin game the hotkey cannot reach: tell the player once, in game.
                let settings = notice_settings.get();
                if let Some(game) = admin_hotkey.notice_due(&settings, &diagnostics) {
                    if settings.show_notification {
                        let placement = crate::save::notification_placement(settings.notification_position);
                        if let Err(error) = notifications.admin_game(&game, placement) {
                            tracing::warn!(%error, "could not show the admin game notice");
                        }
                    }
                }
            }
        });
    }
    let health_app = app.clone();
    tauri::async_runtime::spawn(async move {
        let diagnostics = match configure_engine.configure_settings(&startup_settings).await {
            Ok(diagnostics) => diagnostics,
            Err(error) => {
                tracing::error!(%error, "could not configure native engine at startup");
                return;
            }
        };
        if let Err(error) = configure_engine
            .configure_hotkey(&startup_settings.hotkey)
            .await
        {
            tracing::error!(%error, "could not configure native hotkey at startup");
            return;
        }
        if diagnostics.engine_running {
            if let Err(error) = crate::updates::startup_health::engine_ready() {
                tracing::error!(%error, "Runtime engine health acknowledgement failed");
                super::request_exit(&health_app);
            }
        }
    });

    tauri::async_runtime::spawn(async move {
        while super::hotkeys::next_trigger(&mut hotkeys).await {
            let settings = settings_store.get();
            let result = save_coordinator
                .save(&app, &settings, None, SaveSource::Hotkey)
                .await;
            if !result.ok {
                tracing::warn!(message = %result.message, "native hotkey save failed");
            }
        }
    });
}
