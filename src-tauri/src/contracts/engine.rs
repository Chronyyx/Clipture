use std::collections::BTreeMap;

use serde::{Deserialize, Serialize};
use serde_json::Value;

use super::{AudioSourceKind, ClipSettings};

#[derive(Clone, Copy, Debug, Default, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum CapturePressure {
    #[default]
    Healthy,
    Elevated,
    Critical,
    #[serde(other)]
    Unknown,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct EngineDiagnostics {
    #[serde(default)]
    pub capture_pressure: CapturePressure,
    #[serde(default)]
    pub engine_running: bool,
    #[serde(default)]
    pub degraded: bool,
    #[serde(default)]
    pub status: String,
    #[serde(flatten)]
    pub details: BTreeMap<String, Value>,
}

impl EngineDiagnostics {
    pub fn unavailable(status: impl Into<String>) -> Self {
        Self {
            capture_pressure: CapturePressure::Healthy,
            engine_running: false,
            degraded: true,
            status: status.into(),
            details: BTreeMap::new(),
        }
    }
}

#[derive(Clone, Debug, Default, Deserialize, Serialize)]
#[serde(rename_all = "camelCase", default)]
pub struct ClipRecord {
    pub id: String,
    pub title: String,
    pub game_or_app: String,
    pub library_source: Option<String>,
    pub folder_name: Option<String>,
    pub imported_root: Option<String>,
    pub is_game: Option<bool>,
    pub created_at: String,
    pub duration_seconds: u32,
    pub file_path: String,
    pub resolution: String,
    pub recommended_resolution: Option<String>,
    pub segment_files: Option<Vec<String>>,
    pub segment_resolutions: Option<Vec<String>>,
    pub segment_audio_tracks: Option<Vec<Vec<String>>>,
    pub fps: u32,
    pub encoder: String,
    pub audio_tracks: Vec<String>,
    pub focused_apps: Option<Vec<String>>,
}

#[derive(Clone, Debug, Default, Deserialize, Serialize)]
#[serde(rename_all = "camelCase", default)]
pub struct SaveClipResult {
    pub ok: bool,
    pub message: String,
    pub clip: Option<ClipRecord>,
    pub save_io_analysis: Option<Vec<Value>>,
}

impl SaveClipResult {
    pub fn rejected(message: impl Into<String>) -> Self {
        Self {
            ok: false,
            message: message.into(),
            ..Self::default()
        }
    }
}

#[derive(Clone, Debug, Default, Deserialize, Serialize)]
#[serde(rename_all = "camelCase", default)]
pub struct DisplayDevice {
    pub id: String,
    pub name: String,
    pub is_primary: bool,
    pub width: u32,
    pub height: u32,
    pub x: i32,
    pub y: i32,
    pub hdr: bool,
}

#[derive(Clone, Debug, Default, Deserialize, Serialize)]
#[serde(rename_all = "camelCase", default)]
pub struct AudioInputDevice {
    pub id: String,
    pub name: String,
    pub is_default: bool,
    pub state: Option<String>,
    pub match_key: Option<String>,
}

#[derive(Clone, Debug, Default, Deserialize, Serialize)]
#[serde(rename_all = "camelCase", default)]
pub struct HotkeyStatus {
    pub ready: bool,
    pub armed: bool,
    pub status: String,
}

#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct EngineConfigure {
    pub fps: u32,
    pub bitrate_mbps: u32,
    pub nvenc_preset: u8,
    pub clip_length_seconds: u32,
    #[serde(default = "default_save_in_place")]
    pub save_in_place: bool,
    #[serde(default)]
    pub save_folder: String,
    pub monitor_id: String,
    pub target_width: u32,
    pub target_height: u32,
    pub include_mixed_audio: bool,
    pub include_system_audio: bool,
    pub include_microphone_audio: bool,
    pub capture_game_audio: bool,
    pub capture_foreground_system_audio: bool,
    pub app_audio_processes: String,
    pub system_audio_processes: String,
    pub mic_volume: f64,
    pub mic_isolation: bool,
    pub mic_isolation_weight: f64,
    pub noise_gate_enabled: bool,
    pub auto_noise_gate: bool,
    pub noise_gate_threshold: f64,
    pub noise_gate_debounce_ms: u32,
    pub mic_device_id: String,
    pub mic_device_match_key: String,
    pub mic_device_name: String,
}

fn default_save_in_place() -> bool {
    true
}

impl EngineConfigure {
    pub fn from_settings(settings: &ClipSettings, displays: &[DisplayDevice]) -> Self {
        let system = settings
            .audio_sources
            .iter()
            .find(|source| source.id == "system");
        let microphone = settings
            .audio_sources
            .iter()
            .find(|source| source.kind == AudioSourceKind::Microphone);
        let (target_width, target_height) = settings.resolution_preset.size();
        let bitrate_resolution = if target_width > 0 && target_height > 0 {
            (target_width, target_height)
        } else {
            displays
                .iter()
                .find(|display| {
                    (settings.monitor_id != "primary" && display.id == settings.monitor_id)
                        || (settings.monitor_id == "primary" && display.is_primary)
                })
                .map(|display| (display.width, display.height))
                .unwrap_or((1920, 1080))
        };
        let bitrate_mbps = if settings.auto_bitrate {
            automatic_bitrate(
                bitrate_resolution,
                settings.fps,
                settings.max_auto_bitrate_mbps,
            )
        } else {
            settings.bitrate_mbps
        };

        Self {
            fps: settings.fps,
            bitrate_mbps,
            nvenc_preset: settings.nvenc_preset,
            clip_length_seconds: settings.clip_length_seconds,
            save_in_place: settings.save_in_place,
            save_folder: settings.save_folder.clone(),
            monitor_id: settings.monitor_id.clone(),
            target_width,
            target_height,
            include_mixed_audio: false,
            include_system_audio: system
                .is_some_and(|source| source.enabled && source.capture_all_system.unwrap_or(true)),
            include_microphone_audio: microphone.is_some_and(|source| source.enabled),
            capture_game_audio: settings
                .audio_sources
                .iter()
                .any(|source| source.id == "game" && source.enabled),
            capture_foreground_system_audio: system
                .is_some_and(|source| source.enabled && !source.capture_all_system.unwrap_or(true)),
            app_audio_processes: joined_processes(
                settings
                    .audio_sources
                    .iter()
                    .filter(|source| source.kind == AudioSourceKind::App && source.enabled)
                    .filter_map(|source| source.process_name.as_deref()),
            ),
            system_audio_processes: joined_processes(
                system
                    .filter(|source| source.enabled && !source.capture_all_system.unwrap_or(true))
                    .and_then(|source| source.process_names.as_deref())
                    .unwrap_or_default()
                    .iter()
                    .map(String::as_str),
            ),
            mic_volume: microphone.and_then(|source| source.volume).unwrap_or(1.0),
            mic_isolation: microphone
                .and_then(|source| source.voice_isolation)
                .unwrap_or(false),
            mic_isolation_weight: microphone
                .and_then(|source| source.voice_isolation_weight)
                .unwrap_or(1.0),
            noise_gate_enabled: microphone
                .and_then(|source| source.noise_gate_enabled)
                .unwrap_or(true),
            auto_noise_gate: microphone
                .and_then(|source| source.auto_noise_gate)
                .unwrap_or(true),
            noise_gate_threshold: microphone
                .and_then(|source| source.noise_gate_threshold)
                .unwrap_or(0.05),
            noise_gate_debounce_ms: microphone
                .and_then(|source| source.noise_gate_debounce_ms)
                .unwrap_or(180),
            mic_device_id: microphone
                .and_then(|source| source.mic_device_id.clone())
                .unwrap_or_default(),
            mic_device_match_key: microphone
                .and_then(|source| source.mic_device_match_key.clone())
                .unwrap_or_default(),
            mic_device_name: microphone
                .and_then(|source| source.mic_device_name.clone())
                .unwrap_or_default(),
        }
    }
}

fn automatic_bitrate((width, height): (u32, u32), fps: u32, maximum: u32) -> u32 {
    let pixels = f64::from(width.max(1)) * f64::from(height.max(1));
    let suggested = (24.0 * pixels / (1920.0 * 1080.0) * f64::from(fps.max(1)) / 30.0).round();
    (suggested as u32).min(maximum).clamp(4, 120)
}

fn joined_processes<'a>(values: impl Iterator<Item = &'a str>) -> String {
    let mut processes: Vec<_> = values
        .map(str::trim)
        .filter(|value| !value.is_empty())
        .map(str::to_owned)
        .collect();
    processes.sort_unstable();
    processes.dedup_by(|left, right| left.eq_ignore_ascii_case(right));
    processes.join("|")
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct HostInfo {
    pub host: &'static str,
    pub version: String,
    pub test_mode: bool,
    pub capabilities: Vec<&'static str>,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct HotkeyEvent {
    pub source: String,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct EngineStatusEvent {
    pub running: bool,
    pub message: String,
}

#[derive(Clone, Copy, Debug, Serialize)]
#[serde(rename_all = "kebab-case")]
pub enum SaveSource {
    Ui,
    Tray,
    Hotkey,
}

#[derive(Clone, Copy, Debug, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum SavePhase {
    Started,
    Completed,
    Failed,
    Rejected,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SaveLifecycleEvent {
    pub phase: SavePhase,
    pub source: SaveSource,
    pub duration_seconds: u32,
    pub message: Option<String>,
    pub clip: Option<ClipRecord>,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn configure_contract_matches_engine_field_names() {
        let value = serde_json::to_value(EngineConfigure::from_settings(
            &ClipSettings::default(),
            &[],
        ))
        .unwrap();
        assert_eq!(value["bitrateMbps"], 40);
        assert_eq!(value["includeMixedAudio"], false);
        assert_eq!(value["saveInPlace"], true);
        assert_eq!(value["saveFolder"], "");
        let mut settings = ClipSettings::default();
        settings.save_in_place = false;
        settings.save_folder = r"C:\fixture\clips".into();
        let configured = EngineConfigure::from_settings(&settings, &[]);
        assert!(!configured.save_in_place);
        assert_eq!(configured.save_folder, settings.save_folder);
        assert!(value.get("noiseGateDebounceMs").is_some());
    }

    #[test]
    fn sparse_segment_audio_contract_round_trips() {
        let fixture: serde_json::Value = serde_json::from_str(include_str!(
            "../../../scripts/migration/fixtures/engine-protocol.v1.json"
        ))
        .unwrap();
        let result: SaveClipResult =
            serde_json::from_value(fixture["examples"]["segmentedSave"]["payload"].clone())
                .unwrap();
        let clip = result.clip.unwrap();
        assert_eq!(
            clip.segment_audio_tracks.as_ref().unwrap(),
            &vec![
                vec!["microphone-pcm"],
                vec!["app:chat.exe", "microphone-pcm"]
            ]
        );
        let serialized = serde_json::to_value(clip).unwrap();
        assert_eq!(
            serialized["segmentAudioTracks"],
            fixture["examples"]["segmentedSave"]["payload"]["clip"]["segmentAudioTracks"]
        );
    }

    #[test]
    fn diagnostics_preserve_fields_not_yet_modeled_by_rust() {
        let diagnostics: EngineDiagnostics = serde_json::from_value(serde_json::json!({
            "engineRunning": true,
            "capturePressure": "elevated",
            "status": "ready",
            "futureCounter": 42
        }))
        .unwrap();
        let round_trip = serde_json::to_value(diagnostics).unwrap();
        assert_eq!(round_trip["futureCounter"], 42);
    }
}
