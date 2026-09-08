//! Child output framing, termination, and restart/configuration recovery.
use super::*;

impl EngineClient {
    pub(super) async fn read_process_events(
        self: Arc<Self>,
        generation: u64,
        pid: u32,
        mut receiver: tokio::sync::mpsc::Receiver<CommandEvent>,
    ) {
        let mut termination_message = "Native engine output channel closed.".to_owned();
        let mut stdout = JsonLineDecoder::default();
        while let Some(event) = receiver.recv().await {
            match event {
                CommandEvent::Stdout(bytes) => {
                    for frame in stdout.push(&bytes) {
                        match frame {
                            Ok(line) => self.handle_line(line.trim()),
                            Err(error) => tracing::warn!(
                                generation,
                                pid,
                                %error,
                                "invalid native engine stdout frame"
                            ),
                        }
                    }
                }
                CommandEvent::Stderr(bytes) => {
                    let line = String::from_utf8_lossy(&bytes);
                    tracing::warn!(target: "clipture_engine", "{}", line.trim());
                }
                CommandEvent::Error(error) => {
                    termination_message = format!("Native engine stream error: {error}");
                }
                CommandEvent::Terminated(status) => {
                    termination_message =
                        format!("Native engine exited with code {:?}.", status.code);
                    break;
                }
                _ => {}
            }
        }
        self.mark_stopped(generation, pid, termination_message);
    }

    pub(super) fn handle_line(&self, line: &str) {
        if line.is_empty() {
            return;
        }
        let envelope = match ResponseEnvelope::parse(line) {
            Ok(envelope) => envelope,
            Err(error) => {
                tracing::warn!(%error, bytes = line.len(), "native engine emitted invalid JSON");
                return;
            }
        };
        if envelope.event.as_deref() == Some("hotkey") {
            let event = HotkeyEvent {
                source: envelope.source.unwrap_or_else(|| "native".into()),
            };
            self.hotkey_state.triggered(&event.source);
            let _ = self.hotkey_events.send(event.clone());
            let _ = self.app.emit("engine://hotkey", event);
            return;
        }
        let Some(id) = envelope.id else { return };
        self.pending.complete(
            id,
            match envelope.error {
                Some(error) => Err(error),
                None => Ok(envelope.payload.unwrap_or(Value::Null)),
            },
        );
    }

    fn mark_stopped(self: &Arc<Self>, generation: u64, pid: u32, message: String) {
        let was_current = {
            let mut process = self
                .process
                .lock()
                .unwrap_or_else(|poisoned| poisoned.into_inner());
            if process
                .as_ref()
                .is_some_and(|running| running.pid == pid && running.generation == generation)
            {
                process.take();
                true
            } else {
                false
            }
        };
        if !was_current {
            return;
        }
        self.configured_generation.store(0, Ordering::Release);
        self.hotkey_state.unavailable(&message);
        self.pending.fail_all(&message);
        self.remember_diagnostics(EngineDiagnostics::unavailable(message.clone()));
        self.emit_status(false, message);
        if self.shutting_down.load(Ordering::Acquire) || self.paths.test_mode {
            return;
        }

        let client = Arc::clone(self);
        tauri::async_runtime::spawn(async move {
            tokio::time::sleep(RESTART_DELAY).await;
            if client.ensure_started().is_ok() {
                client.restore_configuration().await;
            }
        });
    }

    pub(super) async fn restore_configuration(self: &Arc<Self>) {
        let configure = self
            .last_configure
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .clone();
        if let Some(configure) = configure {
            if let Ok(diagnostics) = self
                .call::<EngineDiagnostics>("configure", &configure, CONFIGURE_TIMEOUT)
                .await
            {
                self.configured_generation
                    .store(self.running_generation().unwrap_or(0), Ordering::Release);
                self.remember_diagnostics(diagnostics);
            }
        }
        let hotkey = self
            .desired_hotkey
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .clone();
        if !hotkey.is_empty() {
            let _ = self.configure_hotkey(&hotkey).await;
        }
    }
}
