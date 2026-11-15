//! Invite-only telemetry (ADR 0035). Off unless the user redeemed an invite;
//! then the host records what the ADR lists into a local journal and uploads
//! it, signed, to the friend's server. Runs without any WebView.
pub mod commands;
mod enrollment;
mod journal;
mod log_layer;
mod recorder;
mod run_info;
mod scrub;
mod uploader;

pub use log_layer::layer as log_layer;
pub use run_info::run_start;

use clipture_telemetry_protocol::{Body, EnrollRequest, EnrollResponse, Event, Invite, Level, RunStart};
use enrollment::{Enrollment, EnrollmentState};
use journal::Journal;
use recorder::{Out, Recorder};
use scrub::Scrubber;
use serde::Serialize;
use std::{
    collections::BTreeMap,
    path::PathBuf,
    sync::{Arc, Mutex, OnceLock, Weak},
    time::{SystemTime, UNIX_EPOCH},
};

use crate::{contracts::{EngineDiagnostics, SaveClipResult}, engine::RecoveryState};

/// Host log events allowed into the journal per minute.
const HOST_LOGS_PER_MINUTE: u32 = 30;

pub fn now_ms() -> i64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|elapsed| elapsed.as_millis() as i64).unwrap_or_default()
}

pub fn text_at(ms: i64) -> String {
    crate::library::iso_utc(UNIX_EPOCH + std::time::Duration::from_millis(ms.max(0) as u64))
}

pub fn now_text() -> String {
    text_at(now_ms())
}

fn random_id(prefix: &str) -> String {
    let mut bytes = [0u8; 8];
    let _ = getrandom::fill(&mut bytes);
    format!("{prefix}-{}", bytes.iter().map(|b| format!("{b:02x}")).collect::<String>())
}

#[derive(Clone, Debug, Serialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct TelemetryStatus {
    pub supported: bool,
    /// `off`, `active` or `revoked`.
    pub state: &'static str,
    pub server: Option<String>,
    pub friend_name: Option<String>,
    pub device_id: Option<String>,
    pub enrolled_at: Option<String>,
    pub pending_bytes: u64,
    pub last_upload_at: Option<String>,
    pub last_upload_records: u64,
    pub last_error: Option<String>,
}

#[derive(Default)]
struct HostLogs {
    minute: i64,
    written: u32,
    repeats: BTreeMap<(Level, String), u32>,
}

struct Inner {
    enrollment: Option<Enrollment>,
    journal: Option<Journal>,
    recorder: Recorder,
    /// Built once per run; written when telemetry is (or becomes) active.
    run_start: Option<RunStart>,
    run_written: bool,
    host_logs: HostLogs,
    last_upload_at: Option<String>,
    last_upload_records: u64,
    last_error: Option<String>,
    finished: bool,
}

pub struct TelemetryService {
    root: PathBuf,
    crash_log: PathBuf,
    run_id: String,
    network: bool,
    scrubber: Scrubber,
    inner: Mutex<Inner>,
    urgent: tokio::sync::Notify,
    client: OnceLock<reqwest::Client>,
}

/// The log layer reaches the service through this once it exists.
static SINK: OnceLock<Weak<TelemetryService>> = OnceLock::new();

impl TelemetryService {
    pub fn new(data_dir: &std::path::Path, network: bool) -> Arc<Self> {
        let root = data_dir.join("telemetry");
        let enrollment = Enrollment::load(&root);
        let journal = enrollment
            .as_ref()
            .filter(|e| e.state == EnrollmentState::Active)
            .and_then(|_| Journal::open(&root).map_err(|error| tracing::warn!(target: "clipture::telemetry", %error, "telemetry journal unavailable")).ok());
        let service = Arc::new(Self {
            root,
            crash_log: data_dir.join("crash.log"),
            run_id: random_id("r"),
            network,
            scrubber: Scrubber::from_environment(),
            inner: Mutex::new(Inner {
                enrollment,
                journal,
                recorder: Recorder::default(),
                run_start: None,
                run_written: false,
                host_logs: HostLogs::default(),
                last_upload_at: None,
                last_upload_records: 0,
                last_error: None,
                finished: false,
            }),
            urgent: tokio::sync::Notify::new(),
            client: OnceLock::new(),
        });
        let _ = SINK.set(Arc::downgrade(&service));
        service
    }

    fn lock(&self) -> std::sync::MutexGuard<'_, Inner> {
        self.inner.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
    }

    fn active(inner: &Inner) -> bool {
        inner.journal.is_some() && inner.enrollment.as_ref().is_some_and(|e| e.state == EnrollmentState::Active)
    }

    fn append(&self, inner: &mut Inner, session: Option<&str>, t: String, body: Body) {
        let urgent = matches!(&body, Body::Event(event) if event.level == Level::Error);
        if let Some(journal) = &mut inner.journal {
            if let Err(error) = journal.append(&self.run_id, session, t, body) {
                inner.last_error = Some(format!("Could not write the telemetry journal: {error}"));
            }
        }
        if urgent {
            self.urgent.notify_one();
        }
    }

    fn append_out(&self, inner: &mut Inner, out: Vec<Out>) {
        for record in out {
            self.append(inner, record.session.as_deref(), now_text(), record.body);
        }
    }

    /// The run's install summary, known once the engine answered. Writes it
    /// (with crash and unclean-exit events from the last run) when active.
    pub fn begin_run(&self, run_start: RunStart) {
        let mut inner = self.lock();
        inner.run_start = Some(run_start);
        self.write_run_start(&mut inner);
    }

    fn write_run_start(&self, inner: &mut Inner) {
        if inner.run_written || !Self::active(inner) {
            return;
        }
        let Some(mut start) = inner.run_start.clone() else { return };
        let (previous_run, previous_ended, previous_version, crash_offset) = {
            let state = &inner.journal.as_ref().expect("active").state;
            (state.last_run.clone(), state.last_run_ended, state.last_version.clone(), state.crash_log_offset)
        };
        start.previous_clean = previous_run.as_ref().map(|_| previous_ended);
        start.previous_version = previous_version.filter(|version| *version != start.version);
        let version = start.version.clone();
        self.append(inner, None, now_text(), Body::RunStart(start));
        let (crashes, offset) = read_crashes(&self.crash_log, crash_offset);
        let crashed = !crashes.is_empty();
        for crash in crashes {
            self.append(inner, None, now_text(), Body::Event(Event::new(Level::Error, "crash").message(self.scrubber.scrub(&crash))));
        }
        if previous_run.is_some() && !previous_ended && !crashed {
            let event = Event::new(Level::Error, "unclean_exit").message("The previous run ended without closing (killed, power loss or a crash that left no log).");
            self.append(inner, None, now_text(), Body::Event(event));
        }
        inner.run_written = true;
        if let Some(journal) = &mut inner.journal {
            journal.state.last_run = Some(self.run_id.clone());
            journal.state.last_run_ended = false;
            journal.state.last_version = Some(version);
            journal.state.crash_log_offset = offset;
            let _ = journal.save_state();
        }
    }

    pub fn observe_diagnostics(&self, diagnostics: &EngineDiagnostics) {
        let mut inner = self.lock();
        if !Self::active(&inner) || !inner.run_written || inner.finished {
            return;
        }
        let out = inner.recorder.observe(diagnostics, now_ms(), &self.scrubber, || random_id("s"));
        self.append_out(&mut inner, out);
    }

    /// True while an incident collects samples: the host then asks the
    /// engine for the sample that includes GPU load.
    pub fn wants_detail(&self) -> bool {
        self.lock().recorder.wants_detail()
    }

    fn external(&self, event: Event) {
        let mut inner = self.lock();
        if !Self::active(&inner) || !inner.run_written || inner.finished {
            return;
        }
        let out = inner.recorder.external(now_ms(), event);
        self.append_out(&mut inner, out);
    }

    pub fn saved(&self, result: &SaveClipResult, elapsed_ms: u128, requested_seconds: u32) {
        if result.ok {
            let mut event = Event::new(Level::Info, "save").with("durationMs", elapsed_ms as u64).with("clipSeconds", requested_seconds);
            if let Some(clip) = &result.clip {
                event = event.with("fps", clip.fps).with("resolution", clip.resolution.clone()).with("encoder", clip.encoder.clone());
                if clip.is_game == Some(true) && !clip.game_or_app.is_empty() {
                    event = event.with("game", clip.game_or_app.clone());
                }
            }
            self.external(event);
        } else {
            let event = Event::new(Level::Error, "save_failed").message(self.scrubber.scrub(&result.message)).with("durationMs", elapsed_ms as u64);
            self.external(event);
        }
    }

    pub fn recovery(&self, state: RecoveryState, description: Option<String>) {
        let event = match state {
            RecoveryState::Recovered => Event::new(Level::Warn, "encoder_hang"),
            RecoveryState::Halted => Event::new(Level::Error, "recording_halted"),
            _ => return,
        };
        let event = match description {
            Some(description) => event.message(self.scrubber.scrub(&description)),
            None => event,
        };
        self.external(event);
    }

    /// The engine never answered after start: no session will begin.
    pub fn engine_unavailable(&self) {
        self.external(Event::new(Level::Error, "engine_unavailable").message("The engine has not answered for 30 seconds after start."));
    }

    /// From the tracing layer: the host's own warnings and errors.
    fn host_log(&self, level: Level, target: &str, message: &str) {
        let mut inner = self.lock();
        if !Self::active(&inner) || !inner.run_written || inner.finished {
            return;
        }
        let minute = now_ms() / 60_000;
        self.flush_host_logs(&mut inner, minute);
        let message = self.scrubber.scrub(message);
        let key = (level, format!("{target}: {message}"));
        if let Some(repeats) = inner.host_logs.repeats.get_mut(&key) {
            *repeats += 1;
            return;
        }
        if inner.host_logs.written >= HOST_LOGS_PER_MINUTE {
            return;
        }
        inner.host_logs.written += 1;
        inner.host_logs.repeats.insert(key, 0);
        let code = if level == Level::Error { "host_error" } else { "host_warning" };
        let event = Event::new(level, code).message(message).with("target", target.to_string());
        let out = inner.recorder.external(now_ms(), event);
        self.append_out(&mut inner, out);
    }

    /// Repeats of a host log within a minute become one event with a count.
    fn flush_host_logs(&self, inner: &mut Inner, minute: i64) {
        if inner.host_logs.minute == minute {
            return;
        }
        let repeats = std::mem::take(&mut inner.host_logs.repeats);
        inner.host_logs = HostLogs { minute, ..Default::default() };
        for ((level, message), count) in repeats.into_iter().filter(|(_, count)| *count > 0) {
            let code = if level == Level::Error { "host_error" } else { "host_warning" };
            let (target, text) = message.split_once(": ").unwrap_or(("", message.as_str()));
            let mut event = Event::new(level, code).message(text.to_string()).with("target", target.to_string()).with("repeated", true);
            event.count = count;
            let out = inner.recorder.external(now_ms(), event);
            self.append_out(inner, out);
        }
    }

    /// Called by the upload loop: merges a finished minute's repeated host logs.
    fn tick(&self) {
        let mut inner = self.lock();
        if Self::active(&inner) && inner.run_written && !inner.finished {
            self.flush_host_logs(&mut inner, now_ms() / 60_000);
        }
    }

    /// Closes the session and the run. Safe to call more than once.
    pub fn finish(&self, reason: &str) {
        let mut inner = self.lock();
        if inner.finished {
            return;
        }
        inner.finished = true;
        if !Self::active(&inner) || !inner.run_written {
            return;
        }
        self.flush_host_logs(&mut inner, i64::MAX);
        let out = inner.recorder.end_session("app closed");
        self.append_out(&mut inner, out);
        self.append(&mut inner, None, now_text(), Body::RunEnd { reason: reason.into() });
        if let Some(journal) = &mut inner.journal {
            journal.state.last_run_ended = true;
            let _ = journal.seal();
        }
    }

    pub fn status(&self) -> TelemetryStatus {
        let inner = self.lock();
        let enrollment = inner.enrollment.as_ref();
        TelemetryStatus {
            supported: true,
            state: match enrollment.map(|e| e.state) {
                Some(EnrollmentState::Active) => "active",
                Some(EnrollmentState::Revoked) => "revoked",
                None => "off",
            },
            server: enrollment.map(|e| e.server.clone()),
            friend_name: enrollment.map(|e| e.friend_name.clone()),
            device_id: enrollment.map(|e| e.device_id.clone()),
            enrolled_at: enrollment.map(|e| e.enrolled_at.clone()),
            pending_bytes: inner.journal.as_ref().map(Journal::pending_bytes).unwrap_or_default(),
            last_upload_at: inner.last_upload_at.clone(),
            last_upload_records: inner.last_upload_records,
            last_error: inner.last_error.clone(),
        }
    }

    /// The records of the last batch the server accepted, as sent.
    pub fn last_batch(&self) -> Vec<serde_json::Value> {
        std::fs::read_to_string(self.root.join(uploader::LAST_BATCH_FILE))
            .unwrap_or_default()
            .lines()
            .filter_map(|line| serde_json::from_str(line).ok())
            .collect()
    }

    pub async fn enroll(self: &Arc<Self>, code: &str) -> Result<TelemetryStatus, String> {
        if !self.network {
            return Err("Telemetry is disabled in test mode.".into());
        }
        let invite = Invite::parse(code).map_err(|error| error.to_string())?;
        let key = enrollment::new_key().map_err(|error| format!("Could not create a device key: {error}"))?;
        let token = invite.token_text();
        let request = EnrollRequest {
            signature: key.sign(&clipture_telemetry_protocol::enroll_message(&token, &key.public_key())),
            token,
            public_key: key.public_key(),
            version: env!("CARGO_PKG_VERSION").into(),
        };
        let response: EnrollResponse = uploader::enroll(self.client()?, &invite.server, &request).await?;
        let enrolled = Enrollment {
            server: invite.server,
            device_id: response.device_id,
            friend_name: response.friend_name,
            public_key: key.public_key(),
            protected_key: enrollment::protect_key(&key).map_err(|error| format!("Could not protect the device key: {error}"))?,
            enrolled_at: now_text(),
            state: EnrollmentState::Active,
        };
        {
            let mut inner = self.lock();
            // A fresh enrollment never sends an old device's records.
            let _ = std::fs::remove_dir_all(&self.root);
            enrolled.save(&self.root).map_err(|error| format!("Could not save the enrollment: {error}"))?;
            let mut journal = Journal::open(&self.root).map_err(|error| error.to_string())?;
            // Crashes from before enrolling are not this device's telemetry.
            journal.state.crash_log_offset = std::fs::metadata(&self.crash_log).map(|m| m.len()).unwrap_or(0);
            inner.journal = Some(journal);
            inner.enrollment = Some(enrolled);
            inner.recorder = Recorder::default();
            inner.run_written = false;
            inner.last_error = None;
            self.write_run_start(&mut inner);
            let event = Event::new(Level::Info, "enrolled");
            self.append(&mut inner, None, now_text(), Body::Event(event));
        }
        self.urgent.notify_one();
        Ok(self.status())
    }

    /// Stops telemetry and deletes the key and everything unsent.
    pub fn stop(&self) -> TelemetryStatus {
        {
            let mut inner = self.lock();
            inner.journal = None;
            inner.enrollment = None;
            inner.recorder = Recorder::default();
            inner.run_written = false;
            inner.last_upload_at = None;
            inner.last_upload_records = 0;
            inner.last_error = None;
            if let Err(error) = std::fs::remove_dir_all(&self.root) {
                if error.kind() != std::io::ErrorKind::NotFound {
                    inner.last_error = Some(format!("Could not delete the telemetry folder: {error}"));
                }
            }
        }
        self.status()
    }

    fn client(&self) -> Result<&reqwest::Client, String> {
        if let Some(client) = self.client.get() {
            return Ok(client);
        }
        let client = uploader::client().map_err(|error| error.to_string())?;
        Ok(self.client.get_or_init(|| client))
    }

    /// Starts the upload loop. Returns at once; the loop runs for the app's life.
    pub fn start_uploads(self: &Arc<Self>) {
        if !self.network {
            return;
        }
        let service = self.clone();
        tauri::async_runtime::spawn(async move { uploader::run(service).await });
    }
}

impl crate::save::SaveObserver for TelemetryService {
    fn saved(&self, result: &SaveClipResult, elapsed_ms: u128, requested_seconds: u32) {
        TelemetryService::saved(self, result, elapsed_ms, requested_seconds);
    }
}

/// `crash.log` entries written after `offset`. The log restarts past 256 KB;
/// a file shorter than the offset was restarted, so read it all.
fn read_crashes(path: &std::path::Path, offset: u64) -> (Vec<String>, u64) {
    let Ok(bytes) = std::fs::read(path) else { return (Vec::new(), 0) };
    let start = if (bytes.len() as u64) < offset { 0 } else { offset as usize };
    let text = String::from_utf8_lossy(&bytes[start..]);
    let mut entries: Vec<String> = Vec::new();
    for line in text.lines() {
        if line.starts_with('[') || entries.is_empty() {
            entries.push(line.to_string());
        } else if let Some(last) = entries.last_mut() {
            last.push(' ');
            last.push_str(line);
        }
    }
    entries.retain(|entry| !entry.trim().is_empty());
    (entries, bytes.len() as u64)
}

#[cfg(test)]
mod tests;
