//! Explicit debug-only hardware integration test. Unlike the media smoke test,
//! this records the selected display; it requires an isolated profile and CLI opt-in.
use crate::{contracts::SaveSource, state::AppState};
use serde_json::{json, Value};
use std::{
    path::Path,
    time::{Duration, Instant},
};
use tauri::{AppHandle, Manager};

pub fn install(app: &AppHandle) {
    let paths = &app.state::<AppState>().paths;
    if !std::env::args().any(|arg| arg == "--capture-smoke") {
        return;
    }
    if paths.test_mode || !paths.isolated_profile {
        tracing::error!("capture smoke requires a non-test-mode isolated profile");
        super::request_exit(app);
        return;
    }
    let app = app.clone();
    tauri::async_runtime::spawn(async move {
        let mut report = json!({"ok": false, "checks": []});
        match tokio::time::timeout(Duration::from_secs(180), run(&app, &mut report)).await {
            Ok(Ok(())) => report["ok"] = json!(true),
            Ok(Err(error)) => report["error"] = json!(error),
            Err(_) => report["error"] = json!("native capture test timed out"),
        }
        let path = app
            .state::<AppState>()
            .paths
            .data_dir
            .join("capture-smoke-result.json");
        let _ = std::fs::write(path, serde_json::to_vec_pretty(&report).unwrap());
        super::request_exit(&app);
    });
}

fn check(report: &mut Value, condition: bool, message: &str) -> Result<(), String> {
    if !condition {
        return Err(message.into());
    }
    report["checks"]
        .as_array_mut()
        .unwrap()
        .push(json!(message));
    Ok(())
}

async fn run(app: &AppHandle, report: &mut Value) -> Result<(), String> {
    let state = app.state::<AppState>();
    let settings = state.settings.get();
    check(
        report,
        Path::new(&settings.save_folder).starts_with(&state.paths.data_dir),
        "clip folder is isolated",
    )?;
    check(
        report,
        settings.audio_sources.iter().all(|source| !source.enabled),
        "all audio capture disabled",
    )?;
    check(
        report,
        app.webview_windows().is_empty(),
        "starts with no WebView",
    )?;
    let configured = state
        .engine
        .configure_settings(&settings)
        .await
        .map_err(|error| error.to_string())?;
    report["configured"] = serde_json::to_value(configured).unwrap();
    let hotkey = state
        .engine
        .configure_hotkey(&settings.hotkey)
        .await
        .map_err(|error| error.to_string())?;
    check(
        report,
        hotkey.ready && hotkey.armed,
        "native hotkey registered without a WebView",
    )?;
    let mut ready = false;
    let recording_started = Instant::now();
    for _ in 0..30 {
        tokio::time::sleep(Duration::from_secs(1)).await;
        let diagnostics = state
            .engine
            .diagnostics()
            .await
            .map_err(|error| error.to_string())?;
        // bufferDurationSeconds is the configured capacity, not elapsed video.
        ready = recording_started.elapsed() >= Duration::from_secs(6)
            && diagnostics.engine_running
            && diagnostics.details.get("captureReady") == Some(&Value::Bool(true))
            && diagnostics
                .details
                .get("bufferedVideoPackets")
                .and_then(Value::as_u64)
                .unwrap_or(0)
                > 0;
        report["recording"] = serde_json::to_value(diagnostics).unwrap();
        if ready {
            break;
        }
    }
    check(
        report,
        ready,
        "native recording ran for six seconds with no UI",
    )?;
    report["trayResources"] = super::smoke::resources(app, 0, "closed").await;
    check(
        report,
        report["trayResources"]["ok"] == true,
        "tray has no WebView2 processes while recording",
    )?;
    let first = state
        .saves
        .save(app, &settings, Some(5), SaveSource::Tray)
        .await;
    report["traySave"] = serde_json::to_value(&first).unwrap();
    check(
        report,
        first.ok,
        &format!("save without a WebView: {}", first.message),
    )?;
    check(
        report,
        first
            .clip
            .as_ref()
            .is_some_and(|clip| clip.duration_seconds >= 4),
        "saved clip contains the requested replay duration",
    )?;
    tokio::time::sleep(Duration::from_millis(250)).await;
    check(
        report,
        app.get_window("native-save-feedback").is_some(),
        "native save notification exists",
    )?;
    crate::platform::windows::NativeNotificationSink::verify_window(app)
        .map_err(|error| error.to_string())?;
    check(
        report,
        true,
        "native notification preserves legacy size, opacity, square corners and click-through",
    )?;
    check(
        report,
        app.webview_windows().is_empty(),
        "notification did not create a WebView",
    )?;

    let opening = app.clone();
    app.run_on_main_thread(move || {
        let _ = super::windows::open_main(&opening);
    })
    .map_err(|error| error.to_string())?;
    tokio::time::sleep(Duration::from_secs(3)).await;
    check(
        report,
        app.get_webview_window("main").is_some() || super::ui_process::is_open(app),
        "UI opened during native recording",
    )?;
    report["uiResources"] = super::smoke::resources(app, 1, "open").await;
    if super::ui_process::enabled() {
        let generation = state.engine.restored_generation_for_smoke();
        check(
            report,
            super::ui_process::crash_for_smoke(app),
            "forced only the isolated UI worker to exit",
        )?;
        tokio::time::sleep(Duration::from_secs(1)).await;
        let after = state
            .engine
            .diagnostics()
            .await
            .map_err(|error| error.to_string())?;
        check(
            report,
            after.engine_running && state.engine.restored_generation_for_smoke() == generation,
            "UI worker crash did not restart or stop the recording engine",
        )?;
        report["workerCrashResources"] = super::smoke::resources(app, 3, "closed").await;
        check(
            report,
            report["workerCrashResources"]["ok"] == true,
            "crashed UI worker left no WebView processes",
        )?;
        super::windows::open_main(app).map_err(|error| error.to_string())?;
        tokio::time::sleep(Duration::from_secs(3)).await;
        check(
            report,
            super::ui_process::is_open(app),
            "fresh UI worker reopened against the same resident recorder",
        )?;
        let ui_generation = app
            .state::<super::ui_process::UiController>()
            .generation_for_smoke();
        super::windows::destroy_main(app).map_err(|error| error.to_string())?;
        for _ in 0..3 {
            super::windows::open_main(app).map_err(|error| error.to_string())?;
        }
        tokio::time::sleep(Duration::from_secs(4)).await;
        check(
            report,
            super::ui_process::is_open(app)
                && app
                    .state::<super::ui_process::UiController>()
                    .generation_for_smoke()
                    == ui_generation + 1,
            "duplicate Open requests during close create exactly one fresh UI worker",
        )?;
        check(
            report,
            state.engine.restored_generation_for_smoke() == generation,
            "open-during-close race preserves the resident recording generation",
        )?;
    }
    let saving_app = app.clone();
    let saves = state.saves.clone();
    let saving_settings = settings.clone();
    let saving = tauri::async_runtime::spawn(async move {
        saves
            .save(&saving_app, &saving_settings, Some(5), SaveSource::Ui)
            .await
    });
    for _ in 0..100 {
        if state.saves.is_busy() {
            break;
        }
        tokio::time::sleep(Duration::from_millis(1)).await;
    }
    let observed_busy = state.saves.is_busy();
    let closing = app.clone();
    app.run_on_main_thread(move || {
        let _ = super::windows::destroy_main(&closing);
    })
    .map_err(|error| error.to_string())?;
    let second = saving.await.map_err(|error| error.to_string())?;
    report["closingSave"] = serde_json::to_value(&second).unwrap();
    check(
        report,
        observed_busy,
        "UI close requested while a save was in progress",
    )?;
    check(
        report,
        second.ok,
        &format!("save survived UI close: {}", second.message),
    )?;
    tokio::time::sleep(Duration::from_secs(2)).await;
    report["closedResources"] = super::smoke::resources(app, 1, "closed").await;
    check(
        report,
        report["closedResources"]["ok"] == true,
        "WebView2 processes exited after closing during save",
    )?;
    // Exercise the actual production recovery path, not a new manual configure.
    let crashed = state
        .engine
        .crash_for_smoke()
        .map_err(|error| error.to_string())?;
    let mut restored = false;
    for _ in 0..40 {
        tokio::time::sleep(Duration::from_millis(500)).await;
        if state.engine.restored_generation_for_smoke() > crashed
            && state.engine.hotkey_diagnostics().armed
        {
            restored = true;
            break;
        }
    }
    check(
        report,
        restored,
        "engine crash automatically restarted and restored capture configuration",
    )?;
    let restored_hotkey = state.engine.hotkey_diagnostics();
    check(
        report,
        restored_hotkey.ready && restored_hotkey.armed,
        "engine crash restored the native hotkey registration",
    )?;
    // Simulate the updater's before-exit hook followed by an installer launch
    // error. No installer, registry mutation or update network request occurs.
    let installation = state
        .saves
        .reserve_installation()
        .ok_or("could not reserve install test")?;
    let blocked = state
        .saves
        .save(app, &settings, Some(5), SaveSource::Tray)
        .await;
    check(
        report,
        !blocked.ok,
        "install reservation atomically rejects a competing save",
    )?;
    let before_update = state.engine.restored_generation_for_smoke();
    state.engine.shutdown();
    state.engine.resume_after_failed_update();
    drop(installation);
    let mut resumed = false;
    for _ in 0..40 {
        tokio::time::sleep(Duration::from_millis(500)).await;
        if state.engine.restored_generation_for_smoke() > before_update
            && state.engine.hotkey_diagnostics().armed
        {
            resumed = true;
            break;
        }
    }
    check(
        report,
        resumed,
        "failed update launch resumed native capture configuration",
    )?;
    let resumed_hotkey = state.engine.hotkey_diagnostics();
    check(
        report,
        resumed_hotkey.ready && resumed_hotkey.armed,
        "failed update launch restored the native hotkey registration",
    )?;
    tokio::time::sleep(Duration::from_secs(6)).await;
    let recovered = state
        .saves
        .save(app, &settings, Some(5), SaveSource::Tray)
        .await;
    report["recoveredSave"] = serde_json::to_value(&recovered).unwrap();
    check(
        report,
        recovered.ok,
        "save succeeds after crash and failed-update recovery without a UI",
    )?;
    check(
        report,
        recovered
            .clip
            .as_ref()
            .is_some_and(|clip| clip.duration_seconds >= 4),
        "recovered engine buffered the requested replay duration",
    )?;
    report["recoveredResources"] = super::smoke::resources(app, 2, "closed").await;
    check(
        report,
        report["recoveredResources"]["ok"] == true,
        "recovery did not create a WebView",
    )?;
    let clips = state
        .clips
        .list_saved(Path::new(&settings.save_folder))
        .map_err(|error| error.to_string())?;
    check(
        report,
        clips.len() == 3
            && clips
                .iter()
                .all(|clip| Path::new(&clip.file_path).is_file()),
        "all three clips persisted to the isolated library",
    )?;
    report["clips"] = serde_json::to_value(clips).unwrap();
    Ok(())
}
