//! One JSON line per record. Every field is listed in ADR 0035; adding one
//! means changing the ADR, this file and the allowlist test below.
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::BTreeMap;

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Record {
    pub seq: u64,
    /// RFC 3339 UTC with milliseconds.
    pub t: String,
    pub run: String,
    pub session: Option<String>,
    #[serde(flatten)]
    pub body: Body,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum Body {
    RunStart(RunStart),
    RunEnd { reason: String },
    SessionStart(SessionStart),
    SessionEnd(SessionEnd),
    Minute(Minute),
    Event(Event),
    Incident(Incident),
    /// Records the client deleted before they could be sent (journal cap).
    Gap {
        #[serde(rename = "fromSeq")]
        from_seq: u64,
        #[serde(rename = "toSeq")]
        to_seq: u64,
        reason: String,
    },
}

#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct RunStart {
    pub version: String,
    /// Set when this run is the first after an update.
    pub previous_version: Option<String>,
    /// Whether the previous run ended with `run_end`; None on the first run.
    pub previous_clean: Option<bool>,
    pub windows_build: String,
    pub cpu: String,
    pub cpu_threads: u32,
    pub memory_mb: u64,
    pub gpu: String,
    pub gpu_driver: String,
    pub hardware_gpu_scheduling: String,
    pub displays: Vec<Display>,
    pub settings: RecordingSettings,
    /// Xbox Game Bar's "Record what happened": Windows recording games in
    /// the background, a second encoder user alongside Clipture. Since 1.9.3.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub game_bar_background_recording: Option<bool>,
}

#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct Display {
    pub width: u32,
    pub height: u32,
    pub hdr: bool,
    pub primary: bool,
}

#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct RecordingSettings {
    pub fps: u32,
    pub bitrate_mbps: u32,
    pub auto_bitrate: bool,
    pub nvenc_preset: u8,
    pub clip_length_seconds: u32,
    pub resolution_preset: String,
    pub monitor_mode: String,
    pub save_in_place: bool,
    pub gpu_priority_boost: bool,
    pub admin_game_hotkey: bool,
    pub audio_source_rules: u32,
    pub system_audio: bool,
    pub microphone_audio: bool,
    pub game_audio: bool,
    pub mixed_audio: bool,
    pub mic_isolation: bool,
    pub noise_gate: bool,
}

#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct SessionStart {
    pub backend: String,
    pub encoder: String,
    pub codec: String,
    pub source_resolution: String,
    pub output_resolution: String,
    pub target_fps: u32,
    pub display_refresh_hz: f64,
    pub hdr_tonemapping: bool,
}

#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct SessionEnd {
    pub reason: String,
    pub minutes: u32,
    pub dropped: u64,
    pub saves: u32,
    pub failed_saves: u32,
    pub warnings: u32,
    pub errors: u32,
}

#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct Minute {
    /// Start of the clock minute.
    pub minute: String,
    pub samples: u32,
    pub output_fps_mean: f64,
    pub output_fps_min: f64,
    pub capture_fps_mean: f64,
    /// The game's (or desktop's) present rate as capture sees it, measured
    /// per 2-second sample. Before 1.9.3 this was the average since capture
    /// started. Capture delivery is capped at twice the recording rate
    /// (ADR 0031), so a faster game reads as that cap.
    pub desktop_fps_mean: f64,
    pub dropped: u64,
    /// Non-zero counted reasons, by their `frame_definitions.json` names.
    pub reasons: BTreeMap<String, u64>,
    pub dominant_reason: String,
    pub worst_pressure: String,
    pub degraded_seconds: u32,
    pub nvenc_call_p95_max_us: f64,
    pub encoder_queue_p95_max_us: f64,
    pub capture_processing_p95_max_us: f64,
    pub input_map_p95_max_us: f64,
    pub vram_process_max_mb: f64,
    pub vram_adapter_max_mb: f64,
    pub replay_queued_max_bytes: u64,
    pub replay_ram_fallback_max_bytes: u64,
    pub gpu_scheduling_class: String,
    /// Seconds recording ran at realtime GPU priority, a known game was in
    /// front, and a raise was wanted but held at normal by a gate.
    pub realtime_seconds: u32,
    pub game_front_seconds: u32,
    pub held_seconds: u32,
    /// Dropped frames split by the priority they happened at.
    pub dropped_realtime: u64,
    pub dropped_normal: u64,
    /// The known game in front for most of the minute (its executable name,
    /// lowercase), empty at the desktop or in other apps. Since 1.9.3.
    #[serde(skip_serializing_if = "String::is_empty")]
    pub game: String,
}

/// The 2-second samples around a detected problem: up to 60 s before the
/// trigger and 30 s after, so the build-up and the recovery are visible.
#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct Incident {
    /// When the first trigger fired.
    pub at: String,
    /// What fired, in order (`drop_burst`, `fps_drop`, `output_stall`, ...).
    pub triggers: Vec<String>,
    pub backend: String,
    pub target_fps: u32,
    pub samples: Vec<IncidentSample>,
}

/// One engine sample. Latencies are the engine's recent-window values in
/// microseconds; counts are increases since the previous sample.
#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct IncidentSample {
    /// Milliseconds from the trigger (negative: before it).
    pub offset_ms: i64,
    pub output_fps: f64,
    pub capture_fps: f64,
    pub desktop_fps: f64,
    pub dropped: u64,
    pub reasons: BTreeMap<String, u64>,
    pub pressure: String,
    pub degraded: bool,
    pub nvenc_call_p95_us: f64,
    pub encoder_queue_p95_us: f64,
    pub encoder_queue_max_us: f64,
    pub capture_processing_p95_us: f64,
    pub input_map_p95_us: f64,
    /// Longest gap between source frames, encoder outputs, published frames.
    pub capture_interval_max_us: f64,
    pub output_interval_max_us: f64,
    pub published_interval_max_us: f64,
    pub scheduler_lateness_max_us: f64,
    pub queued_frames: f64,
    pub nvenc_in_flight: f64,
    pub gpu_packets_in_flight: f64,
    pub vram_process_mb: f64,
    pub vram_adapter_mb: f64,
    pub vram_budget_mb: f64,
    pub replay_queued_bytes: u64,
    pub gpu_scheduling_class: String,
    pub game_in_front: bool,
    /// The GPU policy wants realtime (it may be held at normal by a gate).
    pub gpu_raise_wanted: bool,
    #[serde(skip_serializing_if = "String::is_empty")]
    pub gpu_hold: String,
    /// Only measured after the trigger (it costs the engine a counter read).
    #[serde(skip_serializing_if = "Option::is_none")]
    pub gpu_busy_percent: Option<f64>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Level {
    Info,
    Warn,
    Error,
}

impl Level {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Info => "info",
            Self::Warn => "warn",
            Self::Error => "error",
        }
    }
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Event {
    pub level: Level,
    pub code: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub message: Option<String>,
    #[serde(default, skip_serializing_if = "BTreeMap::is_empty")]
    pub data: BTreeMap<String, Value>,
    /// Identical events merged within one minute.
    #[serde(default = "one", skip_serializing_if = "is_one")]
    pub count: u32,
}

fn one() -> u32 {
    1
}

fn is_one(count: &u32) -> bool {
    *count == 1
}

impl Event {
    pub fn new(level: Level, code: &str) -> Self {
        Self { level, code: code.into(), message: None, data: BTreeMap::new(), count: 1 }
    }

    pub fn message(mut self, message: impl Into<String>) -> Self {
        self.message = Some(message.into());
        self
    }

    pub fn with(mut self, key: &str, value: impl Into<Value>) -> Self {
        self.data.insert(key.into(), value.into());
        self
    }
}

/// Every event code the client may send (ADR 0035).
pub const EVENT_CODES: &[&str] = &[
    "save",
    "save_failed",
    "drop_burst",
    "capture_backend_changed",
    "capture_fallback",
    "capture_access_lost",
    "capture_recreated",
    "replay_write_failure",
    "pressure_changed",
    "degraded",
    "gpu_priority_changed",
    "encoder_hang",
    "recording_halted",
    "engine_lost",
    "engine_unavailable",
    "crash",
    "unclean_exit",
    "host_warning",
    "host_error",
    "enrolled",
];

#[cfg(test)]
mod tests {
    use super::*;

    fn keys(value: &Value) -> Vec<String> {
        value.as_object().unwrap().keys().cloned().collect()
    }

    #[test]
    fn records_round_trip_with_their_envelope() {
        let records = [
            Body::RunStart(RunStart { version: "1.9.0".into(), displays: vec![Display::default()], ..Default::default() }),
            Body::RunEnd { reason: "quit".into() },
            Body::SessionStart(SessionStart::default()),
            Body::SessionEnd(SessionEnd::default()),
            Body::Minute(Minute { reasons: [("captureQueueOverflow".to_string(), 3)].into(), ..Default::default() }),
            Body::Event(Event::new(Level::Warn, "drop_burst").with("dropped", 12)),
            Body::Gap { from_seq: 4, to_seq: 9, reason: "journal cap".into() },
            Body::Incident(Incident { triggers: vec!["fps_drop".into()], samples: vec![IncidentSample::default()], ..Default::default() }),
        ];
        for (seq, body) in records.into_iter().enumerate() {
            let record = Record { seq: seq as u64, t: "2026-11-14T00:00:00.000Z".into(), run: "r".into(), session: None, body };
            let line = serde_json::to_string(&record).unwrap();
            assert!(!line.contains('\n'));
            assert_eq!(serde_json::from_str::<Record>(&line).unwrap(), record);
        }
    }

    /// The allowlist: these are the only keys a record can carry. A new field
    /// fails here until ADR 0035 and this list say it may be sent.
    #[test]
    fn records_carry_only_the_documented_fields() {
        let run_start = serde_json::to_value(Record {
            seq: 1,
            t: String::new(),
            run: String::new(),
            session: None,
            body: Body::RunStart(RunStart { displays: vec![Display::default()], ..Default::default() }),
        })
        .unwrap();
        assert_eq!(
            keys(&run_start),
            ["cpu", "cpuThreads", "displays", "gpu", "gpuDriver", "hardwareGpuScheduling", "memoryMb", "previousClean", "previousVersion", "run", "seq", "session", "settings", "t", "type", "version", "windowsBuild"]
        );
        assert_eq!(keys(&run_start["displays"][0]), ["hdr", "height", "primary", "width"]);
        assert_eq!(
            keys(&run_start["settings"]),
            ["adminGameHotkey", "audioSourceRules", "autoBitrate", "bitrateMbps", "clipLengthSeconds", "fps", "gameAudio", "gpuPriorityBoost", "micIsolation", "microphoneAudio", "mixedAudio", "monitorMode", "noiseGate", "nvencPreset", "resolutionPreset", "saveInPlace", "systemAudio"]
        );
        assert_eq!(
            keys(&serde_json::to_value(SessionStart::default()).unwrap()),
            ["backend", "codec", "displayRefreshHz", "encoder", "hdrTonemapping", "outputResolution", "sourceResolution", "targetFps"]
        );
        let mut minute = vec![
            "captureFpsMean", "captureProcessingP95MaxUs", "degradedSeconds", "desktopFpsMean", "dominantReason", "dropped",
            "encoderQueueP95MaxUs", "gpuSchedulingClass", "inputMapP95MaxUs", "minute",
            "nvencCallP95MaxUs", "outputFpsMean", "outputFpsMin", "reasons", "replayQueuedMaxBytes", "replayRamFallbackMaxBytes",
            "samples", "vramAdapterMaxMb", "vramProcessMaxMb", "worstPressure", "realtimeSeconds", "gameFrontSeconds",
            "heldSeconds", "droppedRealtime", "droppedNormal",
        ];
        minute.sort_unstable();
        assert_eq!(keys(&serde_json::to_value(Minute::default()).unwrap()), minute);
        assert_eq!(keys(&serde_json::to_value(Incident::default()).unwrap()), ["at", "backend", "samples", "targetFps", "triggers"]);
        let mut sample = vec![
            "offsetMs", "outputFps", "captureFps", "desktopFps", "dropped", "reasons", "pressure", "degraded", "nvencCallP95Us",
            "encoderQueueP95Us", "encoderQueueMaxUs", "captureProcessingP95Us", "inputMapP95Us", "captureIntervalMaxUs",
            "outputIntervalMaxUs", "publishedIntervalMaxUs", "schedulerLatenessMaxUs", "queuedFrames", "nvencInFlight",
            "gpuPacketsInFlight", "vramProcessMb", "vramAdapterMb", "vramBudgetMb", "replayQueuedBytes", "gpuSchedulingClass",
            "gpuBusyPercent", "gameInFront", "gpuRaiseWanted", "gpuHold",
        ];
        sample.sort_unstable();
        let full = IncidentSample { gpu_busy_percent: Some(1.0), gpu_hold: "video memory moving".into(), ..Default::default() };
        assert_eq!(keys(&serde_json::to_value(full).unwrap()), sample);
    }

    #[test]
    fn single_events_omit_their_count() {
        let line = serde_json::to_string(&Event::new(Level::Error, "crash")).unwrap();
        assert_eq!(line, r#"{"level":"error","code":"crash"}"#);
        let parsed: Event = serde_json::from_str(&line).unwrap();
        assert_eq!(parsed.count, 1);
    }
}
