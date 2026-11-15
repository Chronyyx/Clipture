use tauri::{AppHandle, Manager};

use crate::{contracts::SaveSource, state::AppState};

pub fn start(app: AppHandle) {
    let state = app.state::<AppState>();
    let engine = state.engine.clone();
    let settings_store = state.settings.clone();
    let save_coordinator = state.saves.clone();
    let test_mode = state.paths.test_mode;
    let mut hotkeys = engine.subscribe_hotkeys();
    let mut recoveries = engine.subscribe_encoder_recovery();
    let telemetry = state.telemetry.clone();
    telemetry.start_uploads();
    enroll_from_arguments(&app, &std::env::args().collect::<Vec<_>>());

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
        let sample_telemetry = telemetry.clone();
        tauri::async_runtime::spawn(async move {
            let mut run_described = false;
            let mut failures = 0u32;
            loop {
                tokio::time::sleep(std::time::Duration::from_secs(2)).await;
                let sampled = if sample_telemetry.wants_detail() {
                    diagnostics_engine.diagnostics().await
                } else {
                    diagnostics_engine.sample_diagnostics().await
                };
                failures = if sampled.is_ok() { 0 } else { failures + 1 };
                // The install summary wants the engine's GPU and displays, but
                // an engine that never answers is itself worth reporting.
                if !run_described && (sampled.is_ok() || failures >= 15) {
                    run_described = true;
                    let displays = diagnostics_engine.list_display_devices().await.unwrap_or_default();
                    let system = crate::diagnostics::SystemInfoProvider::snapshot(&crate::platform::HostSystemInfo);
                    let described = sampled.as_ref().cloned().unwrap_or_else(|_| crate::contracts::EngineDiagnostics::unavailable("The engine did not answer."));
                    sample_telemetry.begin_run(crate::telemetry::run_start(&notice_settings.get(), &displays, &system, &described));
                    if sampled.is_err() {
                        sample_telemetry.engine_unavailable();
                    }
                }
                let Ok(diagnostics) = sampled else {
                    // Three missed samples in a row: the engine is not recording.
                    if failures == 3 {
                        sample_telemetry.observe_diagnostics(&crate::contracts::EngineDiagnostics::unavailable("The engine did not answer."));
                    }
                    continue;
                };
                sample_telemetry.observe_diagnostics(&diagnostics);
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
    let recovery_settings = settings_store.clone();
    let recovery_notices = state.notifications.clone();
    let recovery_engine = engine.clone();
    let recovery_telemetry = telemetry.clone();
    tauri::async_runtime::spawn(async move {
        use tokio::sync::broadcast::error::RecvError;
        loop {
            let recovery = match recoveries.recv().await {
                Ok(recovery) => recovery,
                Err(RecvError::Lagged(_)) => continue,
                Err(RecvError::Closed) => return,
            };
            recovery_telemetry.recovery(recovery, recovery_engine.encoder_recovery().last_hang);
            let settings = recovery_settings.get();
            let placement = crate::save::notification_placement(settings.notification_position);
            // A halt always reaches the player: recording is off until restart.
            let shown = match recovery {
                crate::engine::RecoveryState::Recovered if settings.show_notification => {
                    recovery_notices.encoder_recovered(placement)
                }
                crate::engine::RecoveryState::Halted => recovery_notices.recording_stopped(placement),
                _ => Ok(()),
            };
            if let Err(error) = shown {
                tracing::warn!(%error, "could not show the encoder recovery notice");
            }
        }
    });
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

/// `clipture.exe --telemetry-enroll <invite code>`: the same as pasting the
/// code in Settings (ADR 0035). Works for the first and a second instance.
pub fn enroll_from_arguments(app: &AppHandle, arguments: &[String]) {
    let Some(code) = arguments.iter().position(|a| a == "--telemetry-enroll").and_then(|i| arguments.get(i + 1)).cloned() else { return };
    let Some(state) = app.try_state::<AppState>() else { return };
    let telemetry = state.telemetry.clone();
    tauri::async_runtime::spawn(async move {
        if let Err(error) = telemetry.enroll(&code).await {
            tracing::warn!(target: "clipture::telemetry", %error, "telemetry enrollment from the command line failed");
        }
    });
}
