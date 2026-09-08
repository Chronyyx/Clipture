mod control;
mod supervision;

use std::{
    sync::{
        atomic::{AtomicBool, AtomicU64, Ordering},
        Arc, Mutex, RwLock,
    },
    time::Duration,
};

use serde::{de::DeserializeOwned, Serialize};
use serde_json::Value;
use tauri::{AppHandle, Emitter};
use tauri_plugin_shell::{
    process::{CommandChild, CommandEvent},
    ShellExt,
};
use tokio::sync::broadcast;

use super::{
    pending::PendingResponses,
    protocol::{encode_request, JsonLineDecoder, ResponseEnvelope},
};
use crate::{
    contracts::{
        AudioInputDevice, DisplayDevice, EngineConfigure, EngineDiagnostics, EngineStatusEvent,
        HotkeyEvent, HotkeyStatus, SaveClipResult,
    },
    error::{AppError, AppResult},
    paths::AppPaths,
    processes::ActiveProcess,
};

const DEFAULT_REQUEST_TIMEOUT: Duration = Duration::from_secs(5);
const CONFIGURE_TIMEOUT: Duration = Duration::from_secs(15);
const RESTART_DELAY: Duration = Duration::from_millis(1_500);

struct RunningEngine {
    generation: u64,
    pid: u32,
    child: CommandChild,
}

pub struct EngineClient {
    app: AppHandle,
    paths: AppPaths,
    process: Mutex<Option<RunningEngine>>,
    start_guard: Mutex<()>,
    pending: PendingResponses,
    frame_diagnostics: Mutex<crate::diagnostics::FrameDropRecorder>,
    next_id: AtomicU64,
    next_generation: AtomicU64,
    configured_generation: AtomicU64,
    shutting_down: AtomicBool,
    last_diagnostics: RwLock<EngineDiagnostics>,
    last_configure: Mutex<Option<EngineConfigure>>,
    desired_hotkey: Mutex<String>,
    hotkey_state: super::hotkey_state::HotkeyState,
    hotkey_events: broadcast::Sender<HotkeyEvent>,
}

impl EngineClient {
    pub fn new(app: AppHandle, paths: AppPaths) -> Arc<Self> {
        let (hotkey_events, _) = broadcast::channel(8);
        Arc::new(Self {
            app,
            paths,
            process: Mutex::new(None),
            start_guard: Mutex::new(()),
            pending: PendingResponses::default(),
            frame_diagnostics: Mutex::new(crate::diagnostics::FrameDropRecorder::default()),
            next_id: AtomicU64::new(1),
            next_generation: AtomicU64::new(1),
            configured_generation: AtomicU64::new(0),
            shutting_down: AtomicBool::new(false),
            last_diagnostics: RwLock::new(EngineDiagnostics::unavailable(
                "Native engine process has not started.",
            )),
            last_configure: Mutex::new(None),
            desired_hotkey: Mutex::new(String::new()),
            hotkey_state: super::hotkey_state::HotkeyState::default(),
            hotkey_events,
        })
    }

    pub fn subscribe_hotkeys(&self) -> broadcast::Receiver<HotkeyEvent> {
        self.hotkey_events.subscribe()
    }

    #[cfg(all(debug_assertions, windows))]
    pub fn crash_for_smoke(&self) -> AppResult<u64> {
        use windows::Win32::{
            Foundation::CloseHandle,
            System::Threading::{OpenProcess, TerminateProcess, PROCESS_TERMINATE},
        };
        if !self.paths.isolated_profile || self.paths.test_mode {
            return Err(AppError::Engine(
                "crash test requires an isolated hardware profile".into(),
            ));
        }
        let process = self.process.lock().unwrap_or_else(|p| p.into_inner());
        let running = process
            .as_ref()
            .ok_or_else(|| AppError::Engine("no owned engine to crash".into()))?;
        // Keep ownership locked; this is the exact live child, never a process
        // found by executable name or an arbitrary renderer-supplied PID.
        unsafe {
            let handle = OpenProcess(PROCESS_TERMINATE, false, running.pid)
                .map_err(|error| AppError::Engine(error.to_string()))?;
            let result = TerminateProcess(handle, 91);
            let _ = CloseHandle(handle);
            result.map_err(|error| AppError::Engine(error.to_string()))?;
        }
        Ok(running.generation)
    }

    #[cfg(debug_assertions)]
    pub fn restored_generation_for_smoke(&self) -> u64 {
        self.configured_generation.load(Ordering::Acquire)
    }

    pub fn cached_diagnostics(&self) -> EngineDiagnostics {
        self.last_diagnostics
            .read()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .clone()
    }

    pub fn ensure_started(self: &Arc<Self>) -> AppResult<u64> {
        if self.shutting_down.load(Ordering::Acquire) {
            return Err(AppError::Engine("native engine is shutting down".into()));
        }
        if self.paths.test_mode {
            return Err(AppError::Engine(
                "native engine startup is disabled by CLIPTURE_TEST_MODE".into(),
            ));
        }
        if let Some(generation) = self.running_generation() {
            return Ok(generation);
        }
        let _start = self
            .start_guard
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        if let Some(generation) = self.running_generation() {
            return Ok(generation);
        }
        if self.shutting_down.load(Ordering::Acquire) {
            return Err(AppError::Engine("native engine is shutting down".into()));
        }

        let command = if let Some(path) = self.paths.development_engine() {
            if !path.is_file() {
                return Err(AppError::Engine(format!(
                    "CLIPTURE_ENGINE_PATH does not point to a file: {}",
                    path.display()
                )));
            }
            self.app.shell().command(path)
        } else {
            self.app
                .shell()
                .sidecar("clipture_engine")
                .map_err(|error| AppError::Engine(error.to_string()))?
        };
        let (receiver, child) = command
            .set_raw_out(true)
            .spawn()
            .map_err(|error| AppError::Engine(format!("could not start native engine: {error}")))?;
        let generation = self.next_generation.fetch_add(1, Ordering::Relaxed);
        let pid = child.pid();
        *self
            .process
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner()) = Some(RunningEngine {
            generation,
            pid,
            child,
        });
        self.emit_status(true, format!("Native engine started (pid {pid})."));

        let client = Arc::clone(self);
        tauri::async_runtime::spawn(async move {
            client.read_process_events(generation, pid, receiver).await;
        });
        Ok(generation)
    }

    pub fn shutdown(&self) {
        self.hotkey_state.unavailable("Native engine stopped.");
        let _start = self
            .start_guard
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        self.shutting_down.store(true, Ordering::Release);
        if let Some(running) = self
            .process
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .take()
        {
            if let Err(error) = running.child.kill() {
                tracing::warn!(%error, "failed to stop native engine");
            }
        }
        self.pending.fail_all("Native engine stopped.");
    }

    /// The Windows updater calls its exit hook before launching the installer.
    /// If launch fails (for example UAC cancellation), undo only that shutdown
    /// and restore capture/hotkeys instead of leaving the app permanently inert.
    pub fn resume_after_failed_update(self: &Arc<Self>) {
        {
            let _start = self.start_guard.lock().unwrap_or_else(|p| p.into_inner());
            if !self.shutting_down.swap(false, Ordering::AcqRel) {
                return;
            }
        }
        let client = self.clone();
        tauri::async_runtime::spawn(async move {
            if let Err(error) = client.ensure_started() {
                tracing::error!(%error, "could not resume capture after failed update launch");
                return;
            }
            client.restore_configuration().await;
        });
    }

    async fn call<T: DeserializeOwned>(
        self: &Arc<Self>,
        command: &str,
        payload: &impl Serialize,
        timeout: Duration,
    ) -> AppResult<T> {
        self.ensure_started()?;
        let id = self.next_id.fetch_add(1, Ordering::Relaxed);
        let message = encode_request(id, command, payload)?;
        let receiver = self.pending.register(id);

        let write_result = self
            .process
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .as_mut()
            .ok_or_else(|| AppError::Engine("native engine stopped before request write".into()))?
            .child
            .write(&message);
        if let Err(error) = write_result {
            self.pending.remove(id);
            return Err(AppError::Engine(format!(
                "could not write engine request: {error}"
            )));
        }

        let response = receiver.wait(command, timeout).await?;
        serde_json::from_value(response).map_err(AppError::SettingsJson)
    }

    fn running_generation(&self) -> Option<u64> {
        self.process
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .as_ref()
            .map(|running| running.generation)
    }

    pub fn frame_drop_analysis(&self) -> Value {
        self.frame_diagnostics
            .lock()
            .unwrap_or_else(|p| p.into_inner())
            .report()
    }

    pub fn hotkey_diagnostics(&self) -> crate::diagnostics::HotkeyDiagnostics {
        self.hotkey_state.snapshot()
    }

    fn remember_diagnostics(&self, mut diagnostics: EngineDiagnostics) -> EngineDiagnostics {
        let sampled_at = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_millis()
            .min(u64::MAX as u128) as u64;
        self.frame_diagnostics
            .lock()
            .unwrap_or_else(|p| p.into_inner())
            .observe(&mut diagnostics, sampled_at);
        *self
            .last_diagnostics
            .write()
            .unwrap_or_else(|poisoned| poisoned.into_inner()) = diagnostics.clone();
        let _ = self.app.emit("engine://diagnostics", &diagnostics);
        diagnostics
    }

    fn emit_status(&self, running: bool, message: String) {
        let _ = self
            .app
            .emit("engine://status", EngineStatusEvent { running, message });
    }
}

fn save_timeout(duration_seconds: u32) -> Duration {
    let milliseconds =
        (u64::from(duration_seconds.clamp(5, 600)) * 3_000 + 60_000).clamp(180_000, 1_800_000);
    Duration::from_millis(milliseconds)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn save_timeout_is_bounded() {
        assert_eq!(save_timeout(5), Duration::from_secs(180));
        assert_eq!(save_timeout(600), Duration::from_secs(1_800));
    }
}
