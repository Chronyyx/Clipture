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
        tauri::async_runtime::spawn(async move {
            loop {
                tokio::time::sleep(std::time::Duration::from_secs(2)).await;
                let _ = diagnostics_engine.diagnostics().await;
            }
        });
    }
    tauri::async_runtime::spawn(async move {
        if let Err(error) = configure_engine.configure_settings(&startup_settings).await {
            tracing::error!(%error, "could not configure native engine at startup");
            return;
        }
        if let Err(error) = configure_engine
            .configure_hotkey(&startup_settings.hotkey)
            .await
        {
            tracing::error!(%error, "could not configure native hotkey at startup");
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
