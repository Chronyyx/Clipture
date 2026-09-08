use std::{
    fs,
    sync::{Arc, Mutex, RwLock},
};

use tauri::{AppHandle, Emitter};
use tauri_plugin_updater::{Update, UpdaterExt};
use tempfile::NamedTempFile;

use super::{
    clock::now_rfc3339,
    model::{UpdateOperation, UpdateState, UpdateStatus},
};

pub const UPDATE_STATE_EVENT: &str = "updates://state-changed";

pub trait UpdateGate: Send + Sync + 'static {
    fn block_reason(&self, operation: UpdateOperation) -> Option<String>;
    /// Windows updater exits directly, so sidecars must stop in its hook.
    fn before_exit(&self) {}
    fn reserve_installation(&self) -> Result<Box<dyn Send>, String> {
        Ok(Box::new(()))
    }
    fn installation_failed(&self) {}
    fn capture_pressure(&self) -> crate::contracts::CapturePressure {
        crate::contracts::CapturePressure::Healthy
    }
}

pub struct AllowUpdates;

impl UpdateGate for AllowUpdates {
    fn block_reason(&self, _operation: UpdateOperation) -> Option<String> {
        None
    }
}

struct PendingUpdate {
    update: Update,
    public_key: String,
    verified_payload: Option<NamedTempFile>,
}

pub struct UpdateService {
    state: RwLock<UpdateState>,
    pending: Mutex<Option<PendingUpdate>>,
    operation: tokio::sync::Mutex<()>,
    gate: Arc<dyn UpdateGate>,
    check_disabled_message: Option<String>,
}

impl Default for UpdateService {
    fn default() -> Self {
        Self::new(Arc::new(AllowUpdates))
    }
}

impl UpdateService {
    pub fn new(gate: Arc<dyn UpdateGate>) -> Self {
        Self::with_check_policy(gate, None)
    }

    pub fn without_network_checks(gate: Arc<dyn UpdateGate>, message: impl Into<String>) -> Self {
        Self::with_check_policy(gate, Some(message.into()))
    }

    fn with_check_policy(
        gate: Arc<dyn UpdateGate>,
        check_disabled_message: Option<String>,
    ) -> Self {
        Self {
            state: RwLock::new(UpdateState::default()),
            pending: Mutex::new(None),
            operation: tokio::sync::Mutex::new(()),
            gate,
            check_disabled_message,
        }
    }

    pub fn get(&self) -> UpdateState {
        self.state
            .read()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .clone()
    }

    pub async fn check(&self, app: &AppHandle) -> Result<UpdateState, UpdateError> {
        let _operation = self.operation.lock().await;
        let previous = self.get();
        if previous.status == UpdateStatus::Ready {
            return Ok(previous);
        }

        let checked_at = now_rfc3339();
        if let Some(message) = &self.check_disabled_message {
            self.replace_pending(None);
            return Ok(self.publish(app, UpdateState::unavailable(message.clone(), checked_at)));
        }
        self.publish(app, UpdateState::checking(&previous, checked_at.clone()));

        let gate = self.gate.clone();
        let builder = app
            .updater_builder()
            .on_before_exit(move || gate.before_exit());
        #[cfg(debug_assertions)]
        let builder = builder.configure_client(super::smoke::configure_client);
        let updater = builder
            .build()
            .map_err(|error| self.fail(app, error.into()))?;
        match updater.check().await {
            Ok(Some(update)) => {
                let version = update.version.clone();
                self.replace_pending(Some(PendingUpdate {
                    update,
                    public_key: app
                        .config()
                        .plugins
                        .0
                        .get("updater")
                        .and_then(|config| config.get("pubkey"))
                        .and_then(serde_json::Value::as_str)
                        .unwrap_or_default()
                        .to_owned(),
                    verified_payload: None,
                }));
                Ok(self.publish(app, UpdateState::available(version, checked_at)))
            }
            Ok(None) => {
                self.replace_pending(None);
                Ok(self.publish(app, UpdateState::current(previous.version, checked_at)))
            }
            Err(error) => Err(self.fail(app, error.into())),
        }
    }

    pub async fn download(&self, app: &AppHandle) -> Result<(), UpdateError> {
        let _operation = self.operation.lock().await;
        self.ensure_allowed(app, UpdateOperation::Download)?;

        let mut pending = self.take_pending().ok_or(UpdateError::NoPendingUpdate)?;
        let version = pending.update.version.clone();
        let checked_at = self.get().checked_at.unwrap_or_else(now_rfc3339);

        if pending.verified_payload.is_some() {
            self.replace_pending(Some(pending));
            self.publish(app, UpdateState::ready(version, checked_at));
            return Ok(());
        }

        self.publish(
            app,
            UpdateState::downloading(
                version.clone(),
                "Preparing update...".into(),
                checked_at.clone(),
            ),
        );

        let mut downloaded = 0_u64;
        let mut last_percent = None;
        let progress_app = app.clone();
        let progress_version = version.clone();
        let progress_checked_at = checked_at.clone();
        let result = super::stream_download::download(
            &pending.update,
            &pending.public_key,
            self.gate.as_ref(),
            |chunk_length, content_length| {
                downloaded = downloaded.saturating_add(chunk_length as u64);
                let percent = content_length
                    .filter(|total| *total > 0)
                    .map(|total| downloaded.saturating_mul(100) / total)
                    .map(|percent| percent.min(100) as u8);
                if percent == last_percent {
                    return;
                }
                last_percent = percent;
                let message = percent.map_or_else(
                    || format!("Preparing update ({} MiB)", downloaded / 1_048_576),
                    |value| format!("Preparing update {value}%"),
                );
                self.publish(
                    &progress_app,
                    UpdateState::downloading(
                        progress_version.clone(),
                        message,
                        progress_checked_at.clone(),
                    ),
                );
            },
        )
        .await;

        let staged = match result {
            Ok(staged) => staged,
            Err(error) => {
                self.replace_pending(Some(pending));
                return Err(self.fail(app, error.into()));
            }
        };

        pending.verified_payload = Some(staged);
        self.replace_pending(Some(pending));
        self.publish(app, UpdateState::ready(version, checked_at));
        Ok(())
    }

    pub async fn install(&self, app: &AppHandle) -> Result<(), UpdateError> {
        let _operation = self.operation.lock().await;
        self.ensure_allowed(app, UpdateOperation::Install)?;

        let (update, public_key, payload_path) = {
            let pending = self
                .pending
                .lock()
                .unwrap_or_else(|poisoned| poisoned.into_inner());
            let pending = pending.as_ref().ok_or(UpdateError::NoPendingUpdate)?;
            let payload = pending
                .verified_payload
                .as_ref()
                .ok_or(UpdateError::UpdateNotDownloaded)?;
            (
                pending.update.clone(),
                pending.public_key.clone(),
                payload.path().to_owned(),
            )
        };

        let bytes = fs::read(payload_path).map_err(|error| self.fail(app, error.into()))?;
        // Reauthenticate exactly the bytes passed to the installer, not merely
        // a path that was verified at download time and could have changed.
        super::stream_download::verify_before_install(&bytes, &public_key, &update.signature)
            .map_err(|error| self.fail(app, error))?;
        self.ensure_allowed(app, UpdateOperation::Install)?;
        // Admission is atomic with the save pipeline and remains held through
        // installer launch. A read-only busy check alone has a TOCTOU race.
        let _capture_lease = self
            .gate
            .reserve_installation()
            .map_err(|reason| self.fail(app, UpdateError::Blocked(reason)))?;
        if let Err(error) = update.install(&bytes) {
            self.gate.installation_failed();
            return Err(self.fail(app, error.into()));
        }

        app.restart();
        #[allow(unreachable_code)]
        Ok(())
    }

    fn ensure_allowed(
        &self,
        app: &AppHandle,
        operation: UpdateOperation,
    ) -> Result<(), UpdateError> {
        let Some(reason) = self.gate.block_reason(operation) else {
            return Ok(());
        };
        let previous = self.get();
        self.publish(app, UpdateState::blocked(&previous, reason.clone()));
        Err(UpdateError::Blocked(reason))
    }

    fn publish(&self, app: &AppHandle, next: UpdateState) -> UpdateState {
        *self
            .state
            .write()
            .unwrap_or_else(|poisoned| poisoned.into_inner()) = next.clone();
        let _ = app.emit(UPDATE_STATE_EVENT, &next);
        next
    }

    fn fail(&self, app: &AppHandle, error: UpdateError) -> UpdateError {
        let previous = self.get();
        self.publish(app, UpdateState::failed(&previous, error.to_string()));
        error
    }

    fn take_pending(&self) -> Option<PendingUpdate> {
        self.pending
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .take()
    }

    fn replace_pending(&self, pending: Option<PendingUpdate>) {
        *self
            .pending
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner()) = pending;
    }
}

#[derive(Debug, thiserror::Error)]
pub enum UpdateError {
    #[error(transparent)]
    Network(#[from] reqwest::Error),
    #[error("{0}")]
    Transfer(String),
    #[error(transparent)]
    Updater(#[from] tauri_plugin_updater::Error),
    #[error(transparent)]
    Io(#[from] std::io::Error),
    #[error("there is no pending update")]
    NoPendingUpdate,
    #[error("the pending update has not been downloaded")]
    UpdateNotDownloaded,
    #[error("update deferred: {0}")]
    Blocked(String),
}

impl serde::Serialize for UpdateError {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: serde::Serializer,
    {
        serializer.serialize_str(&self.to_string())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn disabled_service_records_that_checks_must_not_touch_the_network() {
        let service = UpdateService::without_network_checks(
            Arc::new(AllowUpdates),
            "Updates are disabled by CLIPTURE_TEST_MODE.",
        );

        assert_eq!(
            service.check_disabled_message.as_deref(),
            Some("Updates are disabled by CLIPTURE_TEST_MODE.")
        );
    }
}
