//! Capture smoke, encoder hang: freezes the engine so the host watchdog sees
//! a silent, live process. One hang must restart recording at normal GPU
//! priority with video and audio; a second must halt recording.
use std::time::Duration;

use serde_json::Value;
use tauri::{AppHandle, Manager};

use super::capture_smoke::check;
use crate::{
    contracts::{ClipSettings, SaveSource},
    engine::RecoveryState,
    state::AppState,
};

fn detail<'a>(diagnostics: &'a crate::contracts::EngineDiagnostics, key: &str) -> &'a Value {
    diagnostics.details.get(key).unwrap_or(&Value::Null)
}

async fn encoder_output(app: &AppHandle) -> Option<u64> {
    let diagnostics = app.state::<AppState>().engine.diagnostics().await.ok()?;
    detail(&diagnostics, "encoderOutputPackets").as_u64()
}

pub(super) async fn run(app: &AppHandle, settings: &ClipSettings, report: &mut Value) -> Result<(), String> {
    let state = app.state::<AppState>();
    // System audio on, so the recovered clip proves audio came back too.
    let mut settings = settings.clone();
    for source in settings.audio_sources.iter_mut().filter(|source| source.id == "system") {
        source.enabled = true;
        source.capture_all_system = Some(true);
    }
    let before = state
        .engine
        .configure_settings(&settings)
        .await
        .map_err(|error| error.to_string())?;
    check(
        report,
        detail(&before, "gpuSchedulingClass") == "realtime",
        "GPU priority boost applies the realtime scheduling class by default",
    )?;
    tokio::time::sleep(Duration::from_secs(3)).await;

    let hung = state.engine.hang_for_smoke().map_err(|error| error.to_string())?;
    let mut restored = false;
    for _ in 0..60 {
        tokio::time::sleep(Duration::from_millis(500)).await;
        if state.engine.restored_generation_for_smoke() > hung && state.engine.hotkey_diagnostics().armed {
            restored = true;
            break;
        }
    }
    check(report, restored, "watchdog restarted a hung engine and restored capture")?;
    let recovery = state.engine.encoder_recovery();
    report["hangRecovery"] = serde_json::to_value(&recovery).unwrap();
    check(
        report,
        recovery.state == RecoveryState::Recovered && recovery.hangs == 1,
        "first hang is recorded as one automatic recovery",
    )?;
    let after = state.engine.diagnostics().await.map_err(|error| error.to_string())?;
    check(
        report,
        detail(&after, "gpuSchedulingClass") == "normal" && detail(&after, "gpuPriorityBoost") == false,
        "recovered engine runs at normal GPU priority",
    )?;
    let first = encoder_output(app).await.unwrap_or(0);
    tokio::time::sleep(Duration::from_secs(6)).await;
    let later = encoder_output(app).await.unwrap_or(0);
    report["recoveredEncoderOutput"] = serde_json::json!([first, later]);
    check(report, later > first + 60, "recovered encoder keeps producing video packets")?;
    let saved = state.saves.save(app, &settings, Some(5), SaveSource::Tray).await;
    report["hangRecoveredSave"] = serde_json::to_value(&saved).unwrap();
    check(report, saved.ok, &format!("save after a hang recovery: {}", saved.message))?;
    check(
        report,
        saved.clip.as_ref().is_some_and(|clip| clip.duration_seconds >= 4 && !clip.audio_tracks.is_empty()),
        "clip after a hang recovery has the requested duration and an audio track",
    )?;

    let hung_again = state.engine.hang_for_smoke().map_err(|error| error.to_string())?;
    let mut halted = false;
    // Below realtime the watchdog waits 60 s before calling it a hang.
    for _ in 0..150 {
        tokio::time::sleep(Duration::from_millis(500)).await;
        if state.engine.encoder_recovery().state == RecoveryState::Halted {
            halted = true;
            break;
        }
    }
    check(report, halted, "second hang halts recording")?;
    tokio::time::sleep(Duration::from_secs(8)).await;
    check(
        report,
        state.engine.restored_generation_for_smoke() <= hung_again && state.engine.ensure_started().is_err(),
        "halted recording does not restart the engine",
    )?;
    let refused = state.saves.save(app, &settings, Some(5), SaveSource::Tray).await;
    check(report, !refused.ok, "saving is refused while recording is halted")?;
    Ok(())
}
