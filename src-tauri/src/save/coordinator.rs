use std::{future::Future, pin::Pin, sync::Arc};

use tauri::{AppHandle, Emitter};

use crate::{
    clips::ClipService,
    contracts::{
        ClipSettings, NotificationPosition, SaveClipResult, SaveLifecycleEvent, SavePhase,
        SaveSource,
    },
    engine::EngineClient,
    error::AppResult,
    notifications::{NotificationPlacement, NotificationService},
    sounds::SoundService,
};

use super::{
    operation::CaptureOperationGate, CaptureOperationLease, SaveIoAnalyzer, SavedClipProcessor,
};

type EngineFuture<'a, T> = Pin<Box<dyn Future<Output = AppResult<T>> + Send + 'a>>;

trait SaveEngine: Send + Sync {
    fn configure<'a>(&'a self, settings: &'a ClipSettings) -> EngineFuture<'a, ()>;
    fn save<'a>(
        &'a self,
        duration_seconds: u32,
        save_folder: &'a str,
        analyze_io: bool,
    ) -> EngineFuture<'a, SaveClipResult>;
}

struct NativeSaveEngine(Arc<EngineClient>);

impl SaveEngine for NativeSaveEngine {
    fn configure<'a>(&'a self, settings: &'a ClipSettings) -> EngineFuture<'a, ()> {
        Box::pin(async move { self.0.configure_settings(settings).await.map(|_| ()) })
    }

    fn save<'a>(
        &'a self,
        duration_seconds: u32,
        save_folder: &'a str,
        analyze_io: bool,
    ) -> EngineFuture<'a, SaveClipResult> {
        Box::pin(async move {
            self.0
                .save_clip(duration_seconds, save_folder, analyze_io)
                .await
        })
    }
}

pub struct SaveCoordinator {
    busy: CaptureOperationGate,
    engine: Arc<dyn SaveEngine>,
    clips: Arc<ClipService>,
    sounds: Arc<SoundService>,
    notifications: Arc<NotificationService>,
    analyzer: Arc<SaveIoAnalyzer>,
    processor: Arc<dyn SavedClipProcessor>,
}

impl SaveCoordinator {
    pub fn new(
        engine: Arc<EngineClient>,
        clips: Arc<ClipService>,
        sounds: Arc<SoundService>,
        notifications: Arc<NotificationService>,
        analyzer: Arc<SaveIoAnalyzer>,
        processor: Arc<dyn SavedClipProcessor>,
    ) -> Self {
        Self::with_engine(
            Arc::new(NativeSaveEngine(engine)),
            clips,
            sounds,
            notifications,
            analyzer,
            processor,
        )
    }

    fn with_engine(
        engine: Arc<dyn SaveEngine>,
        clips: Arc<ClipService>,
        sounds: Arc<SoundService>,
        notifications: Arc<NotificationService>,
        analyzer: Arc<SaveIoAnalyzer>,
        processor: Arc<dyn SavedClipProcessor>,
    ) -> Self {
        Self {
            busy: CaptureOperationGate::default(),
            engine,
            clips,
            sounds,
            notifications,
            analyzer,
            processor,
        }
    }

    /// Used by the updater to defer installation while a clip is anywhere in
    /// the save pipeline, including native feedback and metadata persistence.
    pub fn is_busy(&self) -> bool {
        self.busy.is_busy()
    }

    pub fn reserve_installation(&self) -> Option<CaptureOperationLease> {
        self.busy.acquire()
    }

    pub async fn save(
        &self,
        app: &AppHandle,
        settings: &ClipSettings,
        requested_duration: Option<u32>,
        source: SaveSource,
    ) -> SaveClipResult {
        let duration_seconds = requested_duration
            .unwrap_or(settings.clip_length_seconds)
            .clamp(5, 600);
        let Some(_busy) = self.busy.acquire() else {
            let result = SaveClipResult::rejected(
                "A clip save or update installation is in progress. Please wait before saving another clip.",
            );
            emit_save(app, SavePhase::Rejected, source, duration_seconds, &result);
            return result;
        };

        emit_save(
            app,
            SavePhase::Started,
            source,
            duration_seconds,
            &SaveClipResult::default(),
        );
        let result = self.perform_save(settings, duration_seconds).await;
        let phase = if result.ok {
            SavePhase::Completed
        } else {
            SavePhase::Failed
        };
        emit_save(app, phase, source, duration_seconds, &result);
        if let Some(clip) = result.clip.clone().filter(|_| result.ok) {
            let _ = app.emit("library://changed", clip);
        }
        result
    }

    async fn perform_save(&self, settings: &ClipSettings, duration_seconds: u32) -> SaveClipResult {
        let placement = notification_placement(settings.notification_position);
        if settings.clip_sound != "none" && !settings.clip_sound.trim().is_empty() {
            if let Err(error) = self.sounds.play(&settings.clip_sound) {
                tracing::warn!(%error, "could not play the configured native clip sound");
            }
        }
        if settings.show_notification {
            if let Err(error) = self.notifications.saving(placement) {
                tracing::warn!(%error, "could not show native saving notification");
            }
        }

        // Consume this before configuration so every accepted save request,
        // regardless of source or later failure, observes one-shot semantics.
        let analyze_io = self.analyzer.begin_save();
        let mut result = match self.engine.configure(settings).await {
            Ok(()) => self
                .engine
                .save(duration_seconds, &settings.save_folder, analyze_io)
                .await
                .unwrap_or_else(|error| SaveClipResult::rejected(error.to_string())),
            Err(error) => SaveClipResult::rejected(error.to_string()),
        };

        if result.ok {
            if let Some(clip) = result.clip.take() {
                let processor = self.processor.clone();
                let settings = settings.clone();
                match tauri::async_runtime::spawn_blocking(move || {
                    processor.process(clip, &settings)
                })
                .await
                {
                    Ok(Ok(clip)) => result.clip = Some(clip),
                    Ok(Err(error)) => result = failed_result(result, &error.to_string()),
                    Err(error) => {
                        result = failed_result(
                            result,
                            &format!("Clip processing worker failed: {error}"),
                        )
                    }
                }
            }
        }
        self.analyzer.finish_save(
            result.save_io_analysis.clone(),
            result.clip.as_ref().map(|clip| clip.file_path.clone()),
        );
        result = self.persist_success(result);

        if settings.show_notification {
            let notification = if result.ok {
                self.notifications.saved(placement, None)
            } else {
                self.notifications.failed(placement)
            };
            if let Err(error) = notification {
                tracing::warn!(%error, "could not show native save result notification");
            }
        }
        result
    }

    fn persist_success(&self, mut result: SaveClipResult) -> SaveClipResult {
        if !result.ok {
            // An engine-side partial record was not committed and must not be
            // announced as though it were part of the library.
            result.clip = None;
            return result;
        }
        let Some(mut clip) = result.clip.take() else {
            return failed_result(result, "The engine reported success without a clip record.");
        };
        if clip.id.trim().is_empty() || clip.file_path.trim().is_empty() {
            return failed_result(result, "The engine returned an incomplete clip record.");
        }

        clip.library_source = Some("clip".into());
        if let Err(error) = self.clips.append_saved(clip.clone()) {
            return failed_result(
                result,
                &format!("The clip was created but its library record could not be saved: {error}"),
            );
        }
        result.clip = Some(clip);
        result
    }
}

fn failed_result(mut result: SaveClipResult, message: &str) -> SaveClipResult {
    result.ok = false;
    result.clip = None;
    if result.message.trim().is_empty() {
        result.message = message.into();
    } else {
        result.message = format!("{} {message}", result.message.trim());
    }
    result
}

fn notification_placement(position: NotificationPosition) -> NotificationPlacement {
    match position {
        NotificationPosition::TopLeft => NotificationPlacement::TopLeft,
        NotificationPosition::BottomRight => NotificationPlacement::BottomRight,
        NotificationPosition::BottomLeft => NotificationPlacement::BottomLeft,
        NotificationPosition::TopCenter => NotificationPlacement::TopCenter,
        NotificationPosition::TopRight | NotificationPosition::Unknown => {
            NotificationPlacement::TopRight
        }
    }
}

fn emit_save(
    app: &AppHandle,
    phase: SavePhase,
    source: SaveSource,
    duration_seconds: u32,
    result: &SaveClipResult,
) {
    let _ = app.emit(
        "clip://save",
        SaveLifecycleEvent {
            phase,
            source,
            duration_seconds,
            message: (!result.message.is_empty()).then(|| result.message.clone()),
            clip: result.clip.clone(),
        },
    );
}

#[cfg(test)]
#[path = "coordinator_tests.rs"]
mod tests;
