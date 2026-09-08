use std::collections::{BTreeMap, HashMap, HashSet};

use serde::{Deserialize, Serialize};
use serde_json::Value;

#[derive(Clone, Copy, Debug, Default, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "kebab-case")]
pub enum UiTheme {
    #[default]
    Graphite,
    Light,
    Glitten,
    Milate,
    Custom,
    #[serde(other)]
    Unknown,
}

#[derive(Clone, Copy, Debug, Default, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "kebab-case")]
pub enum ResolutionPreset {
    #[default]
    System,
    #[serde(rename = "144p")]
    P144,
    #[serde(rename = "360p")]
    P360,
    #[serde(rename = "720p")]
    P720,
    #[serde(rename = "1080p")]
    P1080,
    #[serde(rename = "1440p")]
    P1440,
    #[serde(rename = "4k")]
    P4k,
    #[serde(other)]
    Unknown,
}

impl ResolutionPreset {
    pub fn size(self) -> (u32, u32) {
        match self {
            Self::P144 => (256, 144),
            Self::P360 => (640, 360),
            Self::P720 => (1280, 720),
            Self::P1080 => (1920, 1080),
            Self::P1440 => (2560, 1440),
            Self::P4k => (3840, 2160),
            Self::System | Self::Unknown => (0, 0),
        }
    }
}

#[derive(Clone, Copy, Debug, Default, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "kebab-case")]
pub enum MonitorMode {
    #[default]
    Primary,
    #[serde(other)]
    Unknown,
}

#[derive(Clone, Copy, Debug, Default, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "kebab-case")]
pub enum NotificationPosition {
    #[default]
    TopRight,
    TopLeft,
    BottomRight,
    BottomLeft,
    TopCenter,
    #[serde(other)]
    Unknown,
}

#[derive(Clone, Copy, Debug, Default, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum AudioSourceKind {
    Microphone,
    Game,
    App,
    Rest,
    Mix,
    #[default]
    System,
    #[serde(other)]
    Unknown,
}

#[derive(Clone, Debug, Default, Deserialize, PartialEq, Serialize)]
#[serde(rename_all = "camelCase", default)]
pub struct AudioSourceRule {
    pub id: String,
    pub label: String,
    pub kind: AudioSourceKind,
    pub process_name: Option<String>,
    pub process_names: Option<Vec<String>>,
    pub executable_path: Option<String>,
    pub capture_all_system: Option<bool>,
    pub enabled: bool,
    pub omit_if_silent: bool,
    pub volume: Option<f64>,
    pub voice_isolation: Option<bool>,
    pub voice_isolation_weight: Option<f64>,
    pub noise_gate_enabled: Option<bool>,
    pub auto_noise_gate: Option<bool>,
    pub noise_gate_threshold: Option<f64>,
    pub noise_gate_debounce_ms: Option<u32>,
    pub mic_device_id: Option<String>,
    pub mic_device_match_key: Option<String>,
    pub mic_device_name: Option<String>,
}

#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
#[serde(rename_all = "camelCase", default)]
pub struct ClipSettings {
    pub ui_theme: UiTheme,
    pub custom_main_color: String,
    pub custom_accent_color: String,
    pub clip_length_seconds: u32,
    pub fps: u32,
    pub bitrate_mbps: u32,
    pub auto_bitrate: bool,
    pub max_auto_bitrate_mbps: u32,
    pub nvenc_preset: u8,
    pub resolution_preset: ResolutionPreset,
    pub monitor_mode: MonitorMode,
    pub monitor_id: String,
    pub start_on_login: bool,
    pub hotkey: String,
    pub clip_sound: String,
    pub show_notification: bool,
    pub notification_position: NotificationPosition,
    pub save_folder: String,
    pub imported_video_directories: Vec<String>,
    pub imported_video_titles: BTreeMap<String, String>,
    pub audio_sources: Vec<AudioSourceRule>,
    #[serde(flatten)]
    pub compatibility_fields: BTreeMap<String, Value>,
}

impl Default for ClipSettings {
    fn default() -> Self {
        Self::defaults_with_save_folder(String::new())
    }
}

impl ClipSettings {
    pub fn defaults_with_save_folder(save_folder: String) -> Self {
        Self {
            ui_theme: UiTheme::Graphite,
            custom_main_color: "#101114".into(),
            custom_accent_color: "#c8a6ff".into(),
            clip_length_seconds: 30,
            fps: 30,
            bitrate_mbps: 40,
            auto_bitrate: false,
            max_auto_bitrate_mbps: 80,
            nvenc_preset: 3,
            resolution_preset: ResolutionPreset::System,
            monitor_mode: MonitorMode::Primary,
            monitor_id: "primary".into(),
            start_on_login: true,
            hotkey: "Ctrl+Shift+S".into(),
            clip_sound: "default.mp3".into(),
            show_notification: true,
            notification_position: NotificationPosition::TopRight,
            save_folder,
            imported_video_directories: Vec::new(),
            imported_video_titles: BTreeMap::new(),
            audio_sources: default_audio_sources(),
            compatibility_fields: BTreeMap::new(),
        }
    }

    pub fn normalize(mut self, default_save_folder: &str) -> Self {
        if self.ui_theme == UiTheme::Unknown {
            self.ui_theme = UiTheme::Graphite;
        }
        if self.resolution_preset == ResolutionPreset::Unknown {
            self.resolution_preset = ResolutionPreset::System;
        }
        if self.monitor_mode == MonitorMode::Unknown {
            self.monitor_mode = MonitorMode::Primary;
        }
        if self.notification_position == NotificationPosition::Unknown {
            self.notification_position = NotificationPosition::TopRight;
        }
        self.custom_main_color = valid_hex(&self.custom_main_color).unwrap_or("#101114".into());
        self.custom_accent_color = valid_hex(&self.custom_accent_color).unwrap_or("#c8a6ff".into());
        self.clip_length_seconds = self.clip_length_seconds.clamp(5, 600);
        if !matches!(self.fps, 24 | 30 | 60) {
            self.fps = 30;
        }
        self.bitrate_mbps = self.bitrate_mbps.clamp(4, 120);
        self.max_auto_bitrate_mbps = self.max_auto_bitrate_mbps.clamp(4, 120);
        if !(1..=5).contains(&self.nvenc_preset) {
            self.nvenc_preset = 3;
        }
        self.monitor_id = if self.monitor_id.trim().is_empty() {
            "primary".into()
        } else {
            self.monitor_id.trim().into()
        };
        self.save_folder = if self.save_folder.trim().is_empty() {
            default_save_folder.into()
        } else {
            normalize_path_text(&self.save_folder)
        };
        self.imported_video_directories = unique_paths(self.imported_video_directories);
        self.imported_video_titles = self
            .imported_video_titles
            .into_iter()
            .filter_map(|(path, title)| {
                let title = title.trim();
                (!path.trim().is_empty() && !title.is_empty())
                    .then(|| (normalize_path_text(&path), title.to_owned()))
            })
            .collect();
        self.audio_sources = normalize_audio_sources(self.audio_sources);
        self
    }
}

fn default_audio_sources() -> Vec<AudioSourceRule> {
    vec![
        AudioSourceRule {
            id: "system".into(),
            label: "System audio".into(),
            kind: AudioSourceKind::System,
            enabled: true,
            omit_if_silent: true,
            ..AudioSourceRule::default()
        },
        AudioSourceRule {
            id: "mic".into(),
            label: "Microphone".into(),
            kind: AudioSourceKind::Microphone,
            enabled: true,
            omit_if_silent: true,
            volume: Some(1.0),
            auto_noise_gate: Some(true),
            noise_gate_enabled: Some(true),
            noise_gate_threshold: Some(0.05),
            noise_gate_debounce_ms: Some(180),
            mic_device_id: Some(String::new()),
            mic_device_match_key: Some(String::new()),
            mic_device_name: Some(String::new()),
            ..AudioSourceRule::default()
        },
        AudioSourceRule {
            id: "game".into(),
            label: "Detected game/app".into(),
            kind: AudioSourceKind::Game,
            omit_if_silent: true,
            ..AudioSourceRule::default()
        },
    ]
}

fn normalize_audio_sources(sources: Vec<AudioSourceRule>) -> Vec<AudioSourceRule> {
    let incoming: HashMap<String, AudioSourceRule> = sources
        .iter()
        .cloned()
        .filter(|source| !source.id.trim().is_empty())
        .map(|source| (source.id.clone(), source))
        .collect();
    let mut normalized: Vec<_> = default_audio_sources()
        .into_iter()
        .map(|fallback| incoming.get(&fallback.id).cloned().unwrap_or(fallback))
        .map(normalize_audio_source)
        .collect();
    normalized.extend(
        sources
            .into_iter()
            .filter(|source| {
                !matches!(source.id.as_str(), "system" | "mic" | "game" | "mix")
                    && source.kind == AudioSourceKind::App
            })
            .map(normalize_audio_source),
    );
    normalized
}

fn normalize_audio_source(mut source: AudioSourceRule) -> AudioSourceRule {
    source.id = source.id.trim().into();
    source.label = source.label.trim().into();
    source.volume = source.volume.map(|value| value.clamp(0.0, 2.0));
    source.voice_isolation_weight = source
        .voice_isolation_weight
        .map(|value| value.clamp(0.0, 1.0));
    source.noise_gate_threshold = source
        .noise_gate_threshold
        .map(|value| value.clamp(0.0, 1.0));
    source.noise_gate_debounce_ms = source
        .noise_gate_debounce_ms
        .map(|value| value.clamp(0, 5_000));
    source
}

fn valid_hex(value: &str) -> Option<String> {
    let value = value.trim();
    (value.len() == 7
        && value.starts_with('#')
        && value[1..].bytes().all(|byte| byte.is_ascii_hexdigit()))
    .then(|| value.to_ascii_lowercase())
}

fn normalize_path_text(value: &str) -> String {
    std::path::PathBuf::from(value.trim())
        .to_string_lossy()
        .into_owned()
}

fn unique_paths(paths: Vec<String>) -> Vec<String> {
    let mut seen = HashSet::new();
    paths
        .into_iter()
        .filter_map(|path| {
            let normalized = normalize_path_text(&path);
            (!normalized.is_empty() && seen.insert(normalized.to_ascii_lowercase()))
                .then_some(normalized)
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn invalid_values_are_bounded_and_default_sources_are_restored() {
        let settings = ClipSettings {
            clip_length_seconds: 1,
            fps: 144,
            bitrate_mbps: 900,
            nvenc_preset: 9,
            save_folder: String::new(),
            audio_sources: Vec::new(),
            ..ClipSettings::default()
        }
        .normalize(r"C:\safe\clips");

        assert_eq!(settings.clip_length_seconds, 5);
        assert_eq!(settings.fps, 30);
        assert_eq!(settings.bitrate_mbps, 120);
        assert_eq!(settings.nvenc_preset, 3);
        assert_eq!(settings.save_folder, r"C:\safe\clips");
        assert_eq!(settings.audio_sources.len(), 3);
    }

    #[test]
    fn serialized_contract_uses_frontend_camel_case() {
        let value = serde_json::to_value(ClipSettings::default()).unwrap();
        assert!(value.get("clipLengthSeconds").is_some());
        assert!(value.get("audioSources").is_some());
        assert_eq!(value["resolutionPreset"], "system");
    }

    #[test]
    fn unknown_legacy_fields_survive_a_settings_round_trip() {
        let settings: ClipSettings = serde_json::from_value(serde_json::json!({
            "clipLengthSeconds": 45,
            "futureCompatibilityField": { "enabled": true }
        }))
        .unwrap();
        let value = serde_json::to_value(settings).unwrap();
        assert_eq!(value["futureCompatibilityField"]["enabled"], true);
    }
}
