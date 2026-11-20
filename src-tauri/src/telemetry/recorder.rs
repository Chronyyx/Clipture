//! Turns the host's 2-second engine samples into records (ADR 0035): one
//! `minute` per clock minute of a session, and events the moment something
//! happens. Pure: no I/O, no clock of its own, so every rule is testable.
use super::scrub::Scrubber;
use crate::contracts::{CapturePressure, EngineDiagnostics};
use clipture_telemetry_protocol::{Body, Event, Incident, IncidentSample, Level, Minute, SessionEnd, SessionStart};
use serde_json::Value;
use std::collections::{BTreeMap, VecDeque};

const MINUTE_MS: i64 = 60_000;
/// Frames dropped in one sample that count as a burst.
const BURST: f64 = 10.0;

/// Counted reasons (frame_definitions.json): record name, engine field.
const REASONS: [(&str, &str); 4] = [
    ("captureQueueOverflow", "captureOverflowDrops"),
    ("captureSlotExhaustion", "captureSlotDrops"),
    ("schedulerDeadlineMisses", "schedulerDroppedFrames"),
    ("encoderBackpressure", "encoderBackpressureDrops"),
];

/// Engine counters whose increase is an event: field, code, level.
const COUNTER_EVENTS: [(&str, &str, Level); 4] = [
    ("captureFallbacks", "capture_fallback", Level::Warn),
    ("captureAccessLosses", "capture_access_lost", Level::Warn),
    ("captureRecreationSuccesses", "capture_recreated", Level::Info),
    ("replayArchiveWriteFailures", "replay_write_failure", Level::Error),
];

/// Samples kept before a trigger (60 s) and taken after it (30 s).
const INCIDENT_BEFORE: usize = 30;
const INCIDENT_AFTER: usize = 15;
/// After an incident closes, the next one waits this long; at most
/// `INCIDENTS_PER_HOUR` per hour, so a bad evening is not hundreds of them.
const INCIDENT_COOLDOWN_MS: i64 = 2 * MINUTE_MS;
const INCIDENTS_PER_HOUR: usize = 10;
/// Event codes that open an incident when they happen.
const INCIDENT_EVENTS: [&str; 6] = ["drop_burst", "encoder_hang", "recording_halted", "replay_write_failure", "capture_access_lost", "degraded"];

struct OpenIncident {
    at_ms: i64,
    triggers: Vec<String>,
    samples: Vec<IncidentSample>,
    after: usize,
}

/// Codes merged to one per minute: a bad stretch repeats them every sample.
const ONCE_PER_MINUTE: [&str; 3] = ["drop_burst", "pressure_changed", "capture_backend_changed"];

/// A record and the session it belongs to.
#[derive(Debug, PartialEq)]
pub struct Out {
    pub session: Option<String>,
    pub body: Body,
}

#[derive(Default)]
struct Bucket {
    start_ms: i64,
    samples: u32,
    output_fps: Vec<f64>,
    capture_fps: Vec<f64>,
    desktop_fps: Vec<f64>,
    dropped: f64,
    reasons: BTreeMap<&'static str, f64>,
    worst_pressure: u8,
    degraded_ms: i64,
    nvenc_call_us: f64,
    encoder_queue_us: f64,
    capture_processing_us: f64,
    input_map_us: f64,
    vram_process_mb: f64,
    vram_adapter_mb: f64,
    replay_queued: f64,
    replay_ram_fallback: f64,
    gpu_scheduling_class: String,
    realtime_ms: i64,
    game_front_ms: i64,
    held_ms: i64,
    dropped_realtime: f64,
    dropped_normal: f64,
    /// Time each known game was in front, to name the minute's game.
    games: BTreeMap<String, i64>,
}

struct Session {
    id: String,
    minutes: u32,
    dropped: u64,
    saves: u32,
    failed_saves: u32,
    warnings: u32,
    errors: u32,
}

#[derive(Default)]
pub struct Recorder {
    session: Option<Session>,
    bucket: Option<Bucket>,
    counters: BTreeMap<&'static str, f64>,
    backend: String,
    pressure: Option<CapturePressure>,
    degraded: bool,
    scheduling_class: String,
    last_sample_ms: Option<i64>,
    limited: BTreeMap<&'static str, i64>,
    sessions_started: u64,
    recent: VecDeque<(i64, IncidentSample)>,
    incident: Option<OpenIncident>,
    incident_times: VecDeque<i64>,
    last_incident_end_ms: Option<i64>,
    session_started_ms: i64,
    target_fps: u32,
    /// The engine's cumulative present count at the last sample.
    presents: Option<f64>,
}

fn number(diagnostics: &EngineDiagnostics, key: &str) -> f64 {
    diagnostics.details.get(key).and_then(Value::as_f64).unwrap_or(0.0)
}

fn flag(diagnostics: &EngineDiagnostics, key: &str) -> bool {
    diagnostics.details.get(key).and_then(Value::as_bool).unwrap_or(false)
}

fn text(diagnostics: &EngineDiagnostics, key: &str) -> String {
    diagnostics.details.get(key).and_then(Value::as_str).unwrap_or_default().to_string()
}

fn pressure_name(pressure: CapturePressure) -> &'static str {
    match pressure {
        CapturePressure::Healthy => "healthy",
        CapturePressure::Elevated => "elevated",
        CapturePressure::Critical => "critical",
        CapturePressure::Unknown => "unknown",
    }
}

fn pressure_rank(pressure: CapturePressure) -> u8 {
    match pressure {
        CapturePressure::Healthy | CapturePressure::Unknown => 0,
        CapturePressure::Elevated => 1,
        CapturePressure::Critical => 2,
    }
}

fn mean(values: &[f64]) -> f64 {
    if values.is_empty() {
        0.0
    } else {
        values.iter().sum::<f64>() / values.len() as f64
    }
}

fn round1(value: f64) -> f64 {
    (value * 10.0).round() / 10.0
}

fn sample_of(diagnostics: &EngineDiagnostics, game_fps: Option<f64>, dropped: f64, reasons: &BTreeMap<&'static str, f64>, detailed: bool) -> IncidentSample {
    let us = |key: &str| round1(number(diagnostics, key) / 10.0);
    IncidentSample {
        offset_ms: 0,
        output_fps: round1(number(diagnostics, "recentEncoderOutputFps")),
        capture_fps: round1(number(diagnostics, "recentPublishedFreshFps")),
        desktop_fps: round1(game_fps.unwrap_or(0.0)),
        dropped: dropped as u64,
        reasons: reasons.iter().map(|(name, value)| (name.to_string(), *value as u64)).collect(),
        pressure: pressure_name(diagnostics.capture_pressure).into(),
        degraded: diagnostics.degraded,
        nvenc_call_p95_us: us("recentNvencCallP95_100ns"),
        encoder_queue_p95_us: us("recentEncoderQueueResidenceP95_100ns"),
        encoder_queue_max_us: us("recentEncoderQueueResidenceMaximum100ns"),
        capture_processing_p95_us: us("recentCaptureProcessingP95_100ns"),
        input_map_p95_us: us("recentInputMapP95_100ns"),
        capture_interval_max_us: us("recentCaptureSourceIntervalMaximum100ns"),
        output_interval_max_us: us("recentEncoderOutputIntervalMaximum100ns"),
        published_interval_max_us: us("recentPublishedWallIntervalMaximum100ns"),
        scheduler_lateness_max_us: us("recentSchedulerWakeLatenessMaximum100ns"),
        queued_frames: number(diagnostics, "queuedFrames"),
        nvenc_in_flight: number(diagnostics, "nvencInFlightFrames"),
        gpu_packets_in_flight: number(diagnostics, "gpuPacketsInFlight"),
        vram_process_mb: number(diagnostics, "vramProcessUsageMb"),
        vram_adapter_mb: number(diagnostics, "vramAdapterUsageMb"),
        vram_budget_mb: number(diagnostics, "vramBudgetMb"),
        replay_queued_bytes: number(diagnostics, "replayArchiveQueuedBytes") as u64,
        gpu_scheduling_class: text(diagnostics, "gpuSchedulingClass"),
        game_in_front: !text(diagnostics, "gpuPriorityForeground").is_empty(),
        gpu_raise_wanted: flag(diagnostics, "gpuPriorityRaised"),
        gpu_hold: text(diagnostics, "gpuPriorityHold"),
        gpu_busy_percent: detailed.then(|| round1(number(diagnostics, "gpuBusyPercent"))),
    }
}

impl Bucket {
    fn gpu_scheduling_class_is_realtime(&self, class: &str) -> bool {
        class == "realtime"
    }
}

impl Recorder {
    /// While an incident collects its after-samples, the host asks the engine
    /// for the costlier sample that includes GPU load.
    pub fn wants_detail(&self) -> bool {
        self.incident.is_some()
    }

    /// Opens an incident (or adds to the open one) unless rate-limited.
    fn trigger(&mut self, now_ms: i64, code: &str) {
        if self.session.is_none() {
            return;
        }
        if let Some(incident) = &mut self.incident {
            if !incident.triggers.iter().any(|t| t == code) {
                incident.triggers.push(code.to_string());
            }
            return;
        }
        if self.last_incident_end_ms.is_some_and(|end| now_ms - end < INCIDENT_COOLDOWN_MS) {
            return;
        }
        while self.incident_times.front().is_some_and(|t| now_ms - t > 60 * MINUTE_MS) {
            self.incident_times.pop_front();
        }
        if self.incident_times.len() >= INCIDENTS_PER_HOUR {
            return;
        }
        self.incident_times.push_back(now_ms);
        let samples = self.recent.iter().map(|(t, sample)| IncidentSample { offset_ms: t - now_ms, ..sample.clone() }).collect();
        self.incident = Some(OpenIncident { at_ms: now_ms, triggers: vec![code.to_string()], samples, after: 0 });
    }

    fn close_incident(&mut self, now_ms: i64) -> Vec<Out> {
        let Some(incident) = self.incident.take() else { return Vec::new() };
        self.last_incident_end_ms = Some(now_ms);
        vec![Out {
            session: self.session_id().map(str::to_string),
            body: Body::Incident(Incident {
                at: super::text_at(incident.at_ms),
                triggers: incident.triggers,
                backend: self.backend.clone(),
                target_fps: self.target_fps,
                samples: incident.samples,
            }),
        }]
    }

    pub fn session_id(&self) -> Option<&str> {
        self.session.as_ref().map(|s| s.id.as_str())
    }

    /// Every counter the recorder diffs, so the first sample sets a baseline.
    fn counter_keys() -> impl Iterator<Item = &'static str> {
        ["droppedFrames"].into_iter().chain(REASONS.iter().map(|(_, field)| *field)).chain(COUNTER_EVENTS.iter().map(|(field, _, _)| *field))
    }

    /// The increase since the last sample. A smaller value means a new
    /// engine started counting from zero, so all of it is new.
    fn delta(&mut self, diagnostics: &EngineDiagnostics, key: &'static str) -> f64 {
        let value = number(diagnostics, key);
        let previous = self.counters.insert(key, value);
        match previous {
            Some(previous) if value >= previous => value - previous,
            Some(_) => value,
            None => 0.0,
        }
    }

    fn event(&mut self, out: &mut Vec<Out>, now_ms: i64, event: Event) {
        if let Some(code) = ONCE_PER_MINUTE.iter().find(|code| **code == event.code) {
            let minute = now_ms.div_euclid(MINUTE_MS);
            if self.limited.get(code) == Some(&minute) {
                return;
            }
            self.limited.insert(code, minute);
        }
        if let Some(session) = &mut self.session {
            match event.level {
                Level::Warn => session.warnings += event.count,
                Level::Error => session.errors += event.count,
                Level::Info => {}
            }
        }
        if INCIDENT_EVENTS.contains(&event.code.as_str()) && event.level != Level::Info {
            self.trigger(now_ms, &event.code.clone());
        }
        out.push(Out { session: self.session_id().map(str::to_string), body: Body::Event(event) });
    }

    /// Any event from outside the sampler (saves, recovery, host logs) goes
    /// through here so it is counted in the session.
    pub fn external(&mut self, now_ms: i64, event: Event) -> Vec<Out> {
        if let Some(session) = &mut self.session {
            match event.code.as_str() {
                "save" => session.saves += 1,
                "save_failed" => session.failed_saves += 1,
                _ => {}
            }
        }
        let mut out = Vec::new();
        self.event(&mut out, now_ms, event);
        out
    }

    pub fn observe(&mut self, diagnostics: &EngineDiagnostics, now_ms: i64, scrubber: &Scrubber, new_id: impl FnOnce() -> String) -> Vec<Out> {
        let mut out = Vec::new();
        let interval = self.last_sample_ms.map(|last| (now_ms - last).clamp(0, 10_000)).unwrap_or(2_000);
        self.last_sample_ms = Some(now_ms);

        if !diagnostics.engine_running {
            if self.session.is_some() {
                self.event(&mut out, now_ms, Event::new(Level::Error, "engine_lost").message("The engine stopped recording."));
                out.extend(self.end_session("engine stopped"));
            }
            self.counters.clear();
            self.presents = None;
            return out;
        }
        if self.session.is_none() {
            self.sessions_started += 1;
            self.session_started_ms = now_ms;
            self.recent.clear();
            self.target_fps = number(diagnostics, "fps") as u32;
            self.session = Some(Session { id: new_id(), minutes: 0, dropped: 0, saves: 0, failed_saves: 0, warnings: 0, errors: 0 });
            out.push(Out {
                session: self.session_id().map(str::to_string),
                body: Body::SessionStart(SessionStart {
                    backend: text(diagnostics, "activeCaptureBackend"),
                    encoder: text(diagnostics, "activeEncoder"),
                    codec: text(diagnostics, "codec"),
                    source_resolution: text(diagnostics, "videoSourceResolution"),
                    output_resolution: text(diagnostics, "videoOutputResolution"),
                    target_fps: number(diagnostics, "fps") as u32,
                    display_refresh_hz: round1(number(diagnostics, "displayRefreshHz")),
                    hdr_tonemapping: number(diagnostics, "hdrTonemapping") > 0.0 || diagnostics.details.get("hdrTonemapping") == Some(&Value::Bool(true)),
                }),
            });
        }

        // The minute this sample falls in; a new minute flushes the last one.
        let minute_start = now_ms.div_euclid(MINUTE_MS) * MINUTE_MS;
        if self.bucket.as_ref().is_some_and(|bucket| bucket.start_ms != minute_start) {
            out.extend(self.flush_minute());
        }
        let bucket_is_new = self.bucket.is_none();
        if bucket_is_new {
            self.bucket = Some(Bucket { start_ms: minute_start, ..Default::default() });
        }

        let baseline = self.counters.is_empty();
        // The game's present rate over this sample. The engine's own
        // `desktopPresentFps` averages since capture started, so a minute's
        // mean of it barely moves; the counter's step does. A smaller count
        // means capture restarted: no rate for that sample.
        let presents_now = number(diagnostics, "captureDesktopPresents");
        let game_fps = self
            .presents
            .replace(presents_now)
            .filter(|previous| presents_now >= *previous && interval > 0)
            .map(|previous| (presents_now - previous) * 1000.0 / interval as f64);
        let dropped = self.delta(diagnostics, "droppedFrames");
        let mut reasons = BTreeMap::new();
        for (name, field) in REASONS {
            let value = self.delta(diagnostics, field);
            if value > 0.0 {
                reasons.insert(name, value);
            }
        }
        for (field, code, level) in COUNTER_EVENTS {
            let increase = self.delta(diagnostics, field);
            if increase > 0.0 && !baseline {
                let mut event = Event::new(level, code);
                event.count = increase as u32;
                if code == "capture_fallback" {
                    let reason = text(diagnostics, "captureFallbackReason");
                    if !reason.is_empty() {
                        event = event.message(scrubber.scrub(&reason));
                    }
                }
                self.event(&mut out, now_ms, event);
            }
        }
        let gpu_starved = self.delta(diagnostics, "gpuStarvationLossyWindows");
        // A gauge, not a counter: what matters is the replay buffer starting
        // to spill to RAM.
        let ram_now = number(diagnostics, "replayArchiveRamFallbackBytes");
        let ram_fallback = if self.counters.insert("replayArchiveRamFallbackBytes", ram_now) == Some(0.0) && ram_now > 0.0 { 1.0 } else { 0.0 };
        if baseline {
            for key in Self::counter_keys() {
                self.counters.entry(key).or_insert_with(|| number(diagnostics, key));
            }
        }

        if dropped >= BURST {
            let mut event = Event::new(Level::Warn, "drop_burst").with("dropped", dropped as u64);
            for (name, value) in &reasons {
                event = event.with(name, *value as u64);
            }
            self.event(&mut out, now_ms, event);
        }
        let backend = text(diagnostics, "activeCaptureBackend");
        if !self.backend.is_empty() && !backend.is_empty() && backend != self.backend {
            let event = Event::new(Level::Info, "capture_backend_changed").with("from", self.backend.clone()).with("to", backend.clone());
            self.event(&mut out, now_ms, event);
        }
        if !backend.is_empty() {
            self.backend = backend;
        }
        let pressure = diagnostics.capture_pressure;
        if self.pressure.is_some_and(|previous| previous != pressure) {
            // Elevated is a frame or two queued for one pass; the recording
            // absorbs it, and when it does not, drop_burst warns. Only
            // critical is worth a warning on its own.
            let level = if pressure == CapturePressure::Critical { Level::Warn } else { Level::Info };
            self.event(&mut out, now_ms, Event::new(level, "pressure_changed").with("to", pressure_name(pressure)));
        }
        self.pressure = Some(pressure);
        if diagnostics.degraded != self.degraded {
            let event = if diagnostics.degraded { Event::new(Level::Warn, "degraded").with("state", "started") } else { Event::new(Level::Info, "degraded").with("state", "ended") };
            self.event(&mut out, now_ms, event);
            self.degraded = diagnostics.degraded;
        }
        let class = text(diagnostics, "gpuSchedulingClass");
        if !self.scheduling_class.is_empty() && !class.is_empty() && class != self.scheduling_class {
            // Why it changed (a policy decision or a gate) and the known game
            // the engine saw in front: what tells an alt-tab from a policy bug.
            let mut event = Event::new(Level::Info, "gpu_priority_changed").with("from", self.scheduling_class.clone()).with("to", class.clone());
            for (key, field) in [("reason", "gpuSchedulingChangeReason"), ("game", "gpuPriorityForeground"), ("hold", "gpuPriorityHold")] {
                let value = text(diagnostics, field);
                if !value.is_empty() {
                    event = event.with(key, value);
                }
            }
            // What "raise ineffective" was decided on: shares of frames lost
            // before and during the raise.
            if text(diagnostics, "gpuSchedulingChangeReason") == "raise ineffective" {
                for (key, field) in [("lossBefore", "gpuPolicyIneffectiveBefore"), ("lossDuring", "gpuPolicyIneffectiveDuring")] {
                    let value = number(diagnostics, field);
                    if value >= 0.0 {
                        event = event.with(key, (value * 10_000.0).round() / 10_000.0);
                    }
                }
            }
            self.event(&mut out, now_ms, event);
        }
        if !class.is_empty() {
            self.scheduling_class = class.clone();
        }

        // Lag the minute average would hide: the game is presenting frames
        // but the recording falls behind, or the output stalls.
        let sample = sample_of(diagnostics, game_fps, dropped, &reasons, self.incident.as_ref().is_some_and(|i| i.after > 0) || self.wants_detail());
        let target = f64::from(self.target_fps.max(1));
        let source_has_frames = sample.desktop_fps >= target * 0.9;
        let settled = now_ms - self.session_started_ms >= 10_000;
        if settled && source_has_frames && sample.output_fps > 0.0 && sample.output_fps < target * 0.8 {
            self.trigger(now_ms, "fps_drop");
        }
        if settled && source_has_frames && sample.output_interval_max_us > 200_000.0 {
            self.trigger(now_ms, "output_stall");
        }
        if pressure == CapturePressure::Critical {
            self.trigger(now_ms, "pressure_critical");
        }
        // The stress situations the simulator reproduces, seen in the field.
        if settled && reasons.get("captureSlotExhaustion").copied().unwrap_or(0.0) >= 3.0 {
            self.trigger(now_ms, "capture_slots");
        }
        if settled && reasons.get("encoderBackpressure").copied().unwrap_or(0.0) >= 3.0 {
            self.trigger(now_ms, "encoder_backpressure");
        }
        if gpu_starved > 0.0 && !baseline {
            self.trigger(now_ms, "gpu_starvation");
        }
        if ram_fallback > 0.0 && !baseline {
            self.trigger(now_ms, "replay_ram_fallback");
        }
        let budget = number(diagnostics, "vramBudgetMb");
        // A full budget alone is normal for a game that streams textures
        // (20 of 20 such incidents on a 3080 lost no frame): it opens one
        // only with drops, and otherwise just tags an incident already open.
        let vram_full = budget > 0.0 && number(diagnostics, "vramAdapterUsageMb") >= budget * 0.95;
        if vram_full && (dropped > 0.0 || self.incident.is_some()) {
            self.trigger(now_ms, "vram_pressure");
        }
        let incident_done = match &mut self.incident {
            Some(incident) => {
                incident.samples.push(IncidentSample { offset_ms: now_ms - incident.at_ms, ..sample.clone() });
                if now_ms > incident.at_ms {
                    incident.after += 1;
                }
                incident.after >= INCIDENT_AFTER
            }
            None => false,
        };
        self.recent.push_back((now_ms, sample));
        while self.recent.len() > INCIDENT_BEFORE {
            self.recent.pop_front();
        }
        if incident_done {
            let closed = self.close_incident(now_ms);
            out.extend(closed);
        }

        let bucket = self.bucket.as_mut().expect("created above");
        bucket.samples += 1;
        bucket.output_fps.push(number(diagnostics, "recentEncoderOutputFps"));
        bucket.capture_fps.push(number(diagnostics, "recentPublishedFreshFps"));
        if let Some(fps) = game_fps {
            bucket.desktop_fps.push(fps);
        }
        bucket.dropped += dropped;
        for (name, value) in reasons {
            *bucket.reasons.entry(name).or_default() += value;
        }
        bucket.worst_pressure = bucket.worst_pressure.max(pressure_rank(pressure));
        if diagnostics.degraded {
            bucket.degraded_ms += interval;
        }
        let max = |slot: &mut f64, value: f64| *slot = slot.max(value);
        max(&mut bucket.nvenc_call_us, number(diagnostics, "recentNvencCallP95_100ns") / 10.0);
        max(&mut bucket.encoder_queue_us, number(diagnostics, "recentEncoderQueueResidenceP95_100ns") / 10.0);
        max(&mut bucket.capture_processing_us, number(diagnostics, "recentCaptureProcessingP95_100ns") / 10.0);
        max(&mut bucket.input_map_us, number(diagnostics, "recentInputMapP95_100ns") / 10.0);
        max(&mut bucket.vram_process_mb, number(diagnostics, "vramProcessUsageMb"));
        max(&mut bucket.vram_adapter_mb, number(diagnostics, "vramAdapterUsageMb"));
        max(&mut bucket.replay_queued, number(diagnostics, "replayArchiveQueuedBytes"));
        max(&mut bucket.replay_ram_fallback, number(diagnostics, "replayArchiveRamFallbackBytes"));
        // Where the minute's time and drops went, by GPU priority.
        let realtime = bucket.gpu_scheduling_class_is_realtime(&class);
        if realtime {
            bucket.realtime_ms += interval;
            bucket.dropped_realtime += dropped;
        } else {
            bucket.dropped_normal += dropped;
        }
        let game = text(diagnostics, "gpuPriorityForeground");
        if !game.is_empty() {
            bucket.game_front_ms += interval;
            *bucket.games.entry(game.to_lowercase()).or_default() += interval;
        }
        if flag(diagnostics, "gpuPriorityRaised") && !realtime {
            bucket.held_ms += interval;
        }
        bucket.gpu_scheduling_class = class;
        out
    }

    fn flush_minute(&mut self) -> Vec<Out> {
        let Some(bucket) = self.bucket.take() else { return Vec::new() };
        if bucket.samples == 0 {
            return Vec::new();
        }
        let dominant = bucket
            .reasons
            .iter()
            .max_by(|a, b| a.1.total_cmp(b.1))
            .map(|(name, _)| name.to_string())
            .unwrap_or_else(|| "none".into());
        let minute = Minute {
            minute: super::text_at(bucket.start_ms),
            samples: bucket.samples,
            output_fps_mean: round1(mean(&bucket.output_fps)),
            output_fps_min: round1(bucket.output_fps.iter().cloned().fold(f64::INFINITY, f64::min)),
            capture_fps_mean: round1(mean(&bucket.capture_fps)),
            desktop_fps_mean: round1(mean(&bucket.desktop_fps)),
            dropped: bucket.dropped as u64,
            reasons: bucket.reasons.iter().map(|(name, value)| (name.to_string(), *value as u64)).collect(),
            dominant_reason: dominant,
            worst_pressure: ["healthy", "elevated", "critical"][bucket.worst_pressure as usize].into(),
            degraded_seconds: (bucket.degraded_ms / 1000) as u32,
            nvenc_call_p95_max_us: round1(bucket.nvenc_call_us),
            encoder_queue_p95_max_us: round1(bucket.encoder_queue_us),
            capture_processing_p95_max_us: round1(bucket.capture_processing_us),
            input_map_p95_max_us: round1(bucket.input_map_us),
            vram_process_max_mb: round1(bucket.vram_process_mb),
            vram_adapter_max_mb: round1(bucket.vram_adapter_mb),
            replay_queued_max_bytes: bucket.replay_queued as u64,
            replay_ram_fallback_max_bytes: bucket.replay_ram_fallback as u64,
            gpu_scheduling_class: bucket.gpu_scheduling_class,
            realtime_seconds: (bucket.realtime_ms / 1000) as u32,
            game_front_seconds: (bucket.game_front_ms / 1000) as u32,
            held_seconds: (bucket.held_ms / 1000) as u32,
            dropped_realtime: bucket.dropped_realtime as u64,
            dropped_normal: bucket.dropped_normal as u64,
            game: bucket.games.iter().max_by_key(|(_, ms)| **ms).map(|(game, _)| game.clone()).unwrap_or_default(),
        };
        if let Some(session) = &mut self.session {
            session.minutes += 1;
            session.dropped += minute.dropped;
        }
        vec![Out { session: self.session_id().map(str::to_string), body: Body::Minute(minute) }]
    }

    /// Flushes the partial minute and closes the session.
    pub fn end_session(&mut self, reason: &str) -> Vec<Out> {
        let now = self.last_sample_ms.unwrap_or_default();
        let mut out = self.close_incident(now);
        out.extend(self.flush_minute());
        if let Some(session) = self.session.take() {
            out.push(Out {
                session: Some(session.id),
                body: Body::SessionEnd(SessionEnd {
                    reason: reason.into(),
                    minutes: session.minutes,
                    dropped: session.dropped,
                    saves: session.saves,
                    failed_saves: session.failed_saves,
                    warnings: session.warnings,
                    errors: session.errors,
                }),
            });
        }
        self.counters.clear();
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn sample(running: bool, details: Value) -> EngineDiagnostics {
        let mut value = json!({ "engineRunning": running, "capturePressure": "healthy", "degraded": false, "status": "" });
        for (key, field) in details.as_object().unwrap() {
            value[key] = field.clone();
        }
        serde_json::from_value(value).unwrap()
    }

    fn scrubber() -> Scrubber {
        Scrubber::with_names(vec![])
    }

    fn codes(out: &[Out]) -> Vec<String> {
        out.iter()
            .map(|o| match &o.body {
                Body::Event(event) => event.code.clone(),
                Body::Minute(_) => "minute".into(),
                Body::SessionStart(_) => "session_start".into(),
                Body::SessionEnd(_) => "session_end".into(),
                other => format!("{other:?}"),
            })
            .collect()
    }

    const T0: i64 = 1_800_000_000_000 - 1_800_000_000_000 % 60_000;

    #[test]
    fn a_session_produces_minutes_from_counter_deltas() {
        let mut recorder = Recorder::default();
        let base = json!({ "droppedFrames": 100, "captureOverflowDrops": 100, "recentEncoderOutputFps": 60, "activeCaptureBackend": "WGC", "fps": 60 });
        let out = recorder.observe(&sample(true, base), T0, &scrubber(), || "s-1".into());
        assert_eq!(codes(&out), ["session_start"], "the first sample is a baseline, not 100 drops");
        let out = recorder.observe(&sample(true, json!({ "droppedFrames": 103, "captureOverflowDrops": 103, "recentEncoderOutputFps": 58 })), T0 + 2_000, &scrubber(), || unreachable!());
        assert!(out.is_empty());
        // The next minute flushes the first one.
        let out = recorder.observe(&sample(true, json!({ "droppedFrames": 103, "captureOverflowDrops": 103, "recentEncoderOutputFps": 60 })), T0 + 60_000, &scrubber(), || unreachable!());
        let Body::Minute(minute) = &out[0].body else { panic!("expected a minute: {out:?}") };
        assert_eq!(out[0].session.as_deref(), Some("s-1"));
        assert_eq!((minute.samples, minute.dropped, minute.output_fps_mean, minute.output_fps_min), (2, 3, 59.0, 58.0));
        assert_eq!(minute.reasons.get("captureQueueOverflow"), Some(&3));
        assert_eq!(minute.dominant_reason, "captureQueueOverflow");
        // The engine stopping ends the session as an error, with its totals.
        let out = recorder.observe(&sample(false, json!({})), T0 + 62_000, &scrubber(), || unreachable!());
        assert_eq!(codes(&out), ["engine_lost", "minute", "session_end"]);
        let Body::SessionEnd(end) = &out[2].body else { panic!() };
        assert_eq!((end.minutes, end.dropped, end.errors), (2, 3, 1));
    }

    #[test]
    fn errors_are_events_at_once_and_bursts_are_merged_per_minute() {
        let mut recorder = Recorder::default();
        recorder.observe(&sample(true, json!({ "droppedFrames": 0, "replayArchiveWriteFailures": 0, "captureFallbacks": 0 })), T0, &scrubber(), || "s".into());
        let out = recorder.observe(
            &sample(true, json!({ "droppedFrames": 40, "captureOverflowDrops": 40, "replayArchiveWriteFailures": 2, "captureFallbacks": 1, "captureFallbackReason": r"DDA failed for C:\Users\x\a" })),
            T0 + 2_000,
            &scrubber(),
            || unreachable!(),
        );
        assert_eq!(codes(&out), ["capture_fallback", "replay_write_failure", "drop_burst"]);
        let Body::Event(fallback) = &out[0].body else { panic!() };
        assert_eq!(fallback.message.as_deref(), Some("DDA failed for <path>"));
        let Body::Event(write) = &out[1].body else { panic!() };
        assert_eq!((write.level, write.count), (Level::Error, 2));
        // A second burst in the same minute is folded into the minute record.
        let out = recorder.observe(&sample(true, json!({ "droppedFrames": 80, "captureOverflowDrops": 80 })), T0 + 4_000, &scrubber(), || unreachable!());
        assert!(codes(&out).is_empty());
        let out = recorder.observe(&sample(true, json!({ "droppedFrames": 120, "captureOverflowDrops": 120 })), T0 + 62_000, &scrubber(), || unreachable!());
        assert_eq!(codes(&out), ["minute", "drop_burst"]);
    }

    #[test]
    fn state_changes_are_events() {
        let mut recorder = Recorder::default();
        recorder.observe(&sample(true, json!({ "activeCaptureBackend": "WGC", "gpuSchedulingClass": "normal" })), T0, &scrubber(), || "s".into());
        let mut changed = sample(true, json!({ "activeCaptureBackend": "DDA", "gpuSchedulingClass": "realtime", "gpuSchedulingChangeReason": "queued drops", "gpuPriorityForeground": "fortniteclient-win64-shipping.exe" }));
        changed.capture_pressure = CapturePressure::Critical;
        changed.degraded = true;
        let out = recorder.observe(&changed, T0 + 2_000, &scrubber(), || unreachable!());
        assert_eq!(codes(&out), ["capture_backend_changed", "pressure_changed", "degraded", "gpu_priority_changed"]);
        let Body::Event(priority) = &out[3].body else { panic!() };
        assert_eq!(priority.data.get("reason"), Some(&json!("queued drops")));
        assert_eq!(priority.data.get("game"), Some(&json!("fortniteclient-win64-shipping.exe")));
        let out = recorder.observe(&sample(true, json!({ "activeCaptureBackend": "DDA", "gpuSchedulingClass": "realtime" })), T0 + 64_000, &scrubber(), || unreachable!());
        let Body::Minute(minute) = &out[0].body else { panic!() };
        assert_eq!((minute.worst_pressure.as_str(), minute.degraded_seconds), ("critical", 2));
        assert_eq!(codes(&out)[1..], ["pressure_changed", "degraded"]);
    }

    #[test]
    fn only_critical_pressure_warns() {
        let mut recorder = Recorder::default();
        recorder.observe(&sample(true, json!({})), T0, &scrubber(), || "s".into());
        let level_at = |recorder: &mut Recorder, pressure: CapturePressure, at: i64| {
            let mut changed = sample(true, json!({}));
            changed.capture_pressure = pressure;
            let out = recorder.observe(&changed, at, &scrubber(), || unreachable!());
            out.iter()
                .find_map(|record| match &record.body {
                    Body::Event(event) if event.code == "pressure_changed" => Some(event.level),
                    _ => None,
                })
                .expect("pressure_changed")
        };
        // Once per minute each: a minute apart.
        assert_eq!(level_at(&mut recorder, CapturePressure::Elevated, T0 + 2_000), Level::Info);
        assert_eq!(level_at(&mut recorder, CapturePressure::Critical, T0 + 70_000), Level::Warn);
    }

    #[test]
    fn minutes_split_time_and_drops_by_gpu_priority() {
        let mut recorder = Recorder::default();
        let at = |class: &str, dropped: u64, front: &str, wanted: bool| {
            json!({ "droppedFrames": dropped, "gpuSchedulingClass": class, "gpuPriorityForeground": front, "gpuPriorityRaised": wanted })
        };
        recorder.observe(&sample(true, at("normal", 0, "game.exe", false)), T0, &scrubber(), || "s".into());
        recorder.observe(&sample(true, at("realtime", 2, "game.exe", true)), T0 + 2_000, &scrubber(), || unreachable!());
        recorder.observe(&sample(true, at("normal", 10, "game.exe", true)), T0 + 4_000, &scrubber(), || unreachable!());
        recorder.observe(&sample(true, at("normal", 10, "", false)), T0 + 6_000, &scrubber(), || unreachable!());
        let out = recorder.end_session("quit");
        let Body::Minute(minute) = &out[0].body else { panic!() };
        assert_eq!((minute.realtime_seconds, minute.game_front_seconds, minute.held_seconds), (2, 6, 2));
        assert_eq!((minute.dropped_realtime, minute.dropped_normal), (2, 8));
    }

    #[test]
    fn a_restarted_engine_counts_from_zero() {
        let mut recorder = Recorder::default();
        recorder.observe(&sample(true, json!({ "droppedFrames": 500 })), T0, &scrubber(), || "s".into());
        // Counters reset (new engine): the 4 frames are new, not -496.
        recorder.observe(&sample(true, json!({ "droppedFrames": 4 })), T0 + 2_000, &scrubber(), || unreachable!());
        let out = recorder.end_session("quit");
        let Body::Minute(minute) = &out[0].body else { panic!() };
        assert_eq!(minute.dropped, 4);
    }

    #[test]
    fn external_events_count_toward_the_session() {
        let mut recorder = Recorder::default();
        recorder.observe(&sample(true, json!({})), T0, &scrubber(), || "s".into());
        recorder.external(T0, Event::new(Level::Info, "save"));
        recorder.external(T0, Event::new(Level::Error, "save_failed"));
        let out = recorder.end_session("quit");
        let Body::SessionEnd(end) = &out.last().unwrap().body else { panic!() };
        assert_eq!((end.saves, end.failed_saves, end.errors), (1, 1, 1));
    }

    fn healthy(output: f64, desktop: f64) -> Value {
        json!({ "fps": 60, "recentEncoderOutputFps": output, "desktopPresentFps": desktop, "vramBudgetMb": 8000, "vramAdapterUsageMb": 5000, "gpuBusyPercent": 71 })
    }

    fn incidents(out: &[Out]) -> Vec<&Incident> {
        out.iter().filter_map(|o| match &o.body { Body::Incident(i) => Some(i), _ => None }).collect()
    }

    /// Feeds `count` samples 2 s apart from `start`; returns everything recorded.
    /// Feeds `count` samples 2 s apart. A sample's `desktopPresentFps` is
    /// turned into the engine's cumulative present count, which is what the
    /// recorder reads.
    fn feed(recorder: &mut Recorder, start: i64, count: usize, details: impl Fn(usize) -> Value) -> Vec<Out> {
        let mut out = Vec::new();
        let mut presents = 0.0;
        for i in 0..count {
            let mut value = details(i);
            presents += value["desktopPresentFps"].as_f64().unwrap_or(0.0) * 2.0;
            value["captureDesktopPresents"] = json!(presents);
            out.extend(recorder.observe(&sample(true, value), start + i as i64 * 2_000, &scrubber(), || "s".into()));
        }
        out
    }

    #[test]
    fn game_fps_is_per_sample_and_minutes_name_their_game() {
        let mut recorder = Recorder::default();
        // A minute at 100 fps in Deadlock, then one at 50 fps on the desktop.
        let start = T0 - T0 % 60_000;
        let mut out = feed(&mut recorder, start, 60, |i| {
            if i < 30 {
                json!({ "fps": 60, "recentEncoderOutputFps": 60.0, "desktopPresentFps": 100.0, "gpuPriorityForeground": "Deadlock.exe" })
            } else {
                json!({ "fps": 60, "recentEncoderOutputFps": 60.0, "desktopPresentFps": 50.0 })
            }
        });
        out.extend(recorder.end_session("quit"));
        let minutes: Vec<&Minute> = out.iter().filter_map(|o| match &o.body { Body::Minute(m) => Some(m), _ => None }).collect();
        assert_eq!(minutes.len(), 2);
        assert_eq!((minutes[0].game.as_str(), minutes[0].desktop_fps_mean), ("deadlock.exe", 100.0));
        assert_eq!((minutes[1].game.as_str(), minutes[1].desktop_fps_mean), ("", 50.0));
    }

    #[test]
    fn lag_while_the_game_presents_records_the_minute_around_it() {
        let mut recorder = Recorder::default();
        // 40 s healthy, then the recording falls to 40 fps while the game runs at 140.
        let out = feed(&mut recorder, T0, 40, |i| if i == 20 { healthy(40.0, 140.0) } else { healthy(60.0, 140.0) });
        let found = incidents(&out);
        assert_eq!(found.len(), 1);
        let incident = found[0];
        assert_eq!(incident.triggers, ["fps_drop"]);
        assert_eq!(incident.target_fps, 60);
        // 20 samples before (all there were), the trigger, then 15 after.
        assert_eq!(incident.samples.first().unwrap().offset_ms, -40_000);
        assert_eq!(incident.samples.iter().filter(|s| s.offset_ms == 0).count(), 1);
        assert_eq!(incident.samples.last().unwrap().offset_ms, 30_000);
        assert_eq!(incident.samples.iter().find(|s| s.offset_ms == 0).unwrap().output_fps, 40.0);
        // GPU load is only measured once the trigger asked for it.
        assert!(incident.samples.iter().filter(|s| s.offset_ms < 0).all(|s| s.gpu_busy_percent.is_none()));
        assert!(incident.samples.iter().filter(|s| s.offset_ms > 0).all(|s| s.gpu_busy_percent == Some(71.0)));
        assert!(!recorder.wants_detail(), "detail sampling stops when the incident closes");
    }

    #[test]
    fn a_game_that_runs_slower_than_the_target_is_not_lag() {
        let mut recorder = Recorder::default();
        // A menu or a heavy scene at 45 fps: the recording follows it, nothing is wrong.
        let out = feed(&mut recorder, T0, 40, |_| healthy(45.0, 45.0));
        assert!(incidents(&out).is_empty());
        assert!(!recorder.wants_detail());
    }

    #[test]
    fn incidents_are_rate_limited() {
        let mut recorder = Recorder::default();
        // Lag every sample for four minutes: one incident, then the cooldown.
        let out = feed(&mut recorder, T0, 120, |i| if i >= 10 { healthy(30.0, 140.0) } else { healthy(60.0, 140.0) });
        let count = incidents(&out).len();
        assert!((2..=3).contains(&count), "{count} incidents in 4 minutes of constant lag");
    }

    #[test]
    fn vram_near_its_budget_opens_an_incident_only_with_drops() {
        let mut recorder = Recorder::default();
        // Full for a minute without a drop: nothing to look at.
        let out = feed(&mut recorder, T0, 40, |i| {
            let mut value = healthy(60.0, 140.0);
            if i >= 12 {
                value["vramAdapterUsageMb"] = json!(7900);
            }
            value
        });
        assert!(incidents(&out).is_empty());
        assert!(!recorder.wants_detail());
        // Full and dropping: the incident says so.
        let mut recorder = Recorder::default();
        let out = feed(&mut recorder, T0, 30, |i| {
            let mut value = healthy(60.0, 140.0);
            if i == 12 {
                value["vramAdapterUsageMb"] = json!(7900);
                value["droppedFrames"] = json!(2);
            } else if i > 12 {
                value["droppedFrames"] = json!(2);
            }
            value["gpuStarvationLossyWindows"] = json!(if i >= 13 { 1 } else { 0 });
            value
        });
        let found = incidents(&out);
        assert_eq!(found.len(), 1);
        assert_eq!(found[0].triggers, ["vram_pressure", "gpu_starvation"]);
    }

    #[test]
    fn an_open_incident_is_written_when_the_session_ends() {
        let mut recorder = Recorder::default();
        feed(&mut recorder, T0, 12, |i| if i == 11 { healthy(20.0, 140.0) } else { healthy(60.0, 140.0) });
        assert!(recorder.wants_detail());
        let out = recorder.end_session("quit");
        assert_eq!(incidents(&out).len(), 1);
    }
}
