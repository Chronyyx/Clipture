use super::*;
use std::{
    fs,
    path::Path,
    sync::{
        atomic::{AtomicUsize, Ordering},
        Mutex,
    },
};

use serde_json::json;

use crate::{
    clips::ClipRepository,
    contracts::ClipRecord,
    notifications::{NotificationKind, NotificationRequest, NotificationSink},
    sounds::{SoundLibrary, SoundPlayer},
};

struct FakeEngine {
    analyze_flags: Mutex<Vec<bool>>,
    result: SaveClipResult,
}

struct FixtureProcessor(bool);
impl SavedClipProcessor for FixtureProcessor {
    fn process(&self, clip: ClipRecord, _: &ClipSettings) -> AppResult<ClipRecord> {
        if !self.0 {
            return Err(crate::error::AppError::Integration(
                "fixture processing failure".into(),
            ));
        }
        Ok(clip)
    }
}

impl SaveEngine for FakeEngine {
    fn configure<'a>(&'a self, _: &'a ClipSettings) -> EngineFuture<'a, ()> {
        Box::pin(async { Ok(()) })
    }

    fn save<'a>(
        &'a self,
        _: u32,
        _: &'a str,
        analyze_io: bool,
    ) -> EngineFuture<'a, SaveClipResult> {
        self.analyze_flags
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .push(analyze_io);
        let result = self.result.clone();
        Box::pin(async move { Ok(result) })
    }
}

#[derive(Default)]
struct RecordingPlayer(AtomicUsize);

impl SoundPlayer for RecordingPlayer {
    fn play(&self, _: &Path) -> AppResult<()> {
        self.0.fetch_add(1, Ordering::Relaxed);
        Ok(())
    }
}

#[derive(Default)]
struct RecordingNotifications(Mutex<Vec<NotificationKind>>);

impl NotificationSink for RecordingNotifications {
    fn show(&self, request: &NotificationRequest) -> AppResult<()> {
        self.0
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .push(request.kind);
        Ok(())
    }

    fn hide(&self) -> AppResult<()> {
        Ok(())
    }
}

#[test]
fn successful_save_consumes_analysis_and_commits_before_success_feedback() {
    run_save(true);
}

#[test]
fn processing_failure_never_commits_or_announces_a_saved_clip() {
    run_save(false);
}

fn run_save(processing_succeeds: bool) {
    let root = tempfile::tempdir().unwrap();
    let clip_path = root.path().join("saved.mp4");
    let sound_path = root.path().join("sounds/save.wav");
    fs::create_dir_all(sound_path.parent().unwrap()).unwrap();
    fs::write(&sound_path, b"wave").unwrap();
    let repository = Arc::new(ClipRepository::new(root.path().join("clips.json")));
    let clips = Arc::new(ClipService::new(repository.clone()));
    let player = Arc::new(RecordingPlayer::default());
    let sounds = Arc::new(SoundService::new(
        Arc::new(SoundLibrary::new(root.path().join("sounds"), vec![])),
        player.clone(),
    ));
    let notification_sink = Arc::new(RecordingNotifications::default());
    let notifications = Arc::new(NotificationService::new(notification_sink.clone()));
    let analyzer = Arc::new(SaveIoAnalyzer::new(true));
    analyzer.set_armed(true);
    let engine = Arc::new(FakeEngine {
        analyze_flags: Mutex::new(Vec::new()),
        result: SaveClipResult {
            ok: true,
            message: "Saved.".into(),
            clip: Some(ClipRecord {
                id: "clip-1".into(),
                file_path: clip_path.to_string_lossy().into_owned(),
                ..ClipRecord::default()
            }),
            save_io_analysis: Some(vec![json!({ "bytes": 42 })]),
        },
    });
    let coordinator = SaveCoordinator::with_engine(
        engine.clone(),
        clips,
        sounds,
        notifications,
        analyzer.clone(),
        Arc::new(FixtureProcessor(processing_succeeds)),
    );
    let settings = ClipSettings {
        clip_sound: "custom:save.wav".into(),
        show_notification: true,
        save_folder: root.path().to_string_lossy().into_owned(),
        ..ClipSettings::default()
    };

    let result = tauri::async_runtime::block_on(coordinator.perform_save(&settings, 30));

    if !processing_succeeds {
        assert!(!result.ok);
        assert!(result.clip.is_none());
        assert!(result.message.contains("fixture processing failure"));
        assert!(repository.load().unwrap().is_empty());
        assert_eq!(
            *notification_sink.0.lock().unwrap(),
            vec![NotificationKind::Saving, NotificationKind::Failed]
        );
        return;
    }
    assert!(result.ok);
    assert_eq!(
        result.clip.as_ref().unwrap().library_source.as_deref(),
        Some("clip")
    );
    let persisted = repository.load().unwrap();
    assert_eq!(persisted.len(), 1);
    assert_eq!(persisted[0].id, result.clip.unwrap().id);
    assert_eq!(
        *engine
            .analyze_flags
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner()),
        vec![true]
    );
    assert_eq!(player.0.load(Ordering::Relaxed), 1);
    assert_eq!(
        *notification_sink
            .0
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner()),
        vec![NotificationKind::Saving, NotificationKind::Saved]
    );
    assert!(analyzer.state().trace_ready);
    assert!(!analyzer.state().armed);
}

#[test]
fn successful_engine_result_without_a_record_is_not_announced_as_success() {
    let result = failed_result(
        SaveClipResult {
            ok: true,
            message: "Engine finished.".into(),
            ..SaveClipResult::default()
        },
        "The engine reported success without a clip record.",
    );
    assert!(!result.ok);
    assert!(result.clip.is_none());
    assert!(result.message.contains("without a clip record"));
}

#[test]
fn busy_lease_is_visible_and_always_released() {
    let busy = CaptureOperationGate::default();
    {
        let _lease = busy.acquire().unwrap();
        assert!(busy.is_busy());
        assert!(busy.acquire().is_none());
    }
    assert!(!busy.is_busy());
}
