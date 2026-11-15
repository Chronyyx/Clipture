//! The `run_start` record: what this PC and these settings are, built only
//! from allowlisted fields (ADR 0035, "What is recorded").
use crate::{
    contracts::{ClipSettings, DisplayDevice, EngineConfigure, EngineDiagnostics},
    diagnostics::OperatingSystemInfo,
};
use clipture_telemetry_protocol::{Display, RecordingSettings, RunStart};

fn detail(diagnostics: &EngineDiagnostics, key: &str) -> String {
    match diagnostics.details.get(key) {
        Some(serde_json::Value::String(text)) => text.clone(),
        _ => String::new(),
    }
}

fn enum_name<T: serde::Serialize>(value: &T) -> String {
    serde_json::to_value(value).ok().and_then(|value| value.as_str().map(str::to_string)).unwrap_or_default()
}

pub fn run_start(settings: &ClipSettings, displays: &[DisplayDevice], system: &OperatingSystemInfo, diagnostics: &EngineDiagnostics) -> RunStart {
    let configure = EngineConfigure::from_settings(settings, displays);
    RunStart {
        version: env!("CARGO_PKG_VERSION").into(),
        previous_version: None,
        previous_clean: None,
        windows_build: system.release.clone(),
        cpu: system.processor.clone(),
        cpu_threads: system.logical_processors as u32,
        memory_mb: system.total_memory_bytes / (1024 * 1024),
        gpu: detail(diagnostics, "gpu"),
        gpu_driver: detail(diagnostics, "gpuDriverVersion"),
        hardware_gpu_scheduling: detail(diagnostics, "hardwareGpuScheduling"),
        displays: displays.iter().map(|d| Display { width: d.width, height: d.height, hdr: d.hdr, primary: d.is_primary }).collect(),
        settings: RecordingSettings {
            fps: settings.fps,
            bitrate_mbps: settings.bitrate_mbps,
            auto_bitrate: settings.auto_bitrate,
            nvenc_preset: settings.nvenc_preset,
            clip_length_seconds: settings.clip_length_seconds,
            resolution_preset: enum_name(&settings.resolution_preset),
            monitor_mode: enum_name(&settings.monitor_mode),
            save_in_place: settings.save_in_place,
            gpu_priority_boost: settings.gpu_priority_boost,
            admin_game_hotkey: settings.admin_game_hotkey,
            audio_source_rules: settings.audio_sources.len() as u32,
            system_audio: configure.include_system_audio,
            microphone_audio: configure.include_microphone_audio,
            game_audio: configure.capture_game_audio,
            mixed_audio: configure.include_mixed_audio,
            mic_isolation: configure.mic_isolation,
            noise_gate: configure.noise_gate_enabled,
        },
    }
}
