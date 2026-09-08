use super::{FfmpegExecutor, FfmpegJob, FfmpegOutput, MediaSaveProcessor};
use crate::{
    contracts::{ClipRecord, ClipSettings},
    error::AppResult,
    save::SavedClipProcessor,
};
use std::{fs, path::Path, sync::Arc};

struct FailingFfmpeg;
impl FfmpegExecutor for FailingFfmpeg {
    fn run(&self, _: FfmpegJob) -> AppResult<FfmpegOutput> {
        Ok(FfmpegOutput {
            success: false,
            exit_code: Some(1),
            stdout: vec![],
            stderr: "fixture failure".into(),
        })
    }
}

fn fixture(root: &Path) -> (ClipRecord, ClipSettings) {
    let segments: Vec<_> = ["one.mp4", "two.mp4"]
        .iter()
        .map(|name| {
            let path = root.join(name);
            fs::write(&path, [42; 64]).unwrap();
            path.to_string_lossy().into_owned()
        })
        .collect();
    (
        ClipRecord {
            id: "fixture".into(),
            file_path: root.join("complete.mp4").to_string_lossy().into_owned(),
            segment_files: Some(segments),
            segment_resolutions: Some(vec!["320x180".into(); 2]),
            resolution: "320x180".into(),
            ..ClipRecord::default()
        },
        ClipSettings::defaults_with_save_folder(root.to_string_lossy().into_owned()),
    )
}

#[test]
fn failed_stitch_preserves_all_inputs_and_does_not_publish_a_clip() {
    let root = tempfile::tempdir().unwrap();
    let (clip, settings) = fixture(root.path());
    let processor = MediaSaveProcessor::new(Arc::new(FailingFfmpeg));
    let failure = processor.process(clip.clone(), &settings).unwrap_err();
    assert!(failure.to_string().contains("fixture failure"));
    for path in clip.segment_files.unwrap() {
        assert_eq!(fs::read(path).unwrap(), [42; 64]);
    }
    assert!(!root.path().join("Apps/complete.mp4").exists());
    assert!(!root.path().join("complete.mp4").exists());
    assert!(fs::read_dir(root.path()).unwrap().all(|entry| !entry
        .unwrap()
        .file_name()
        .to_string_lossy()
        .starts_with(".clipture-save-")));
}

#[test]
fn missing_duplicate_or_external_segments_fail_before_processing() {
    let root = tempfile::tempdir().unwrap();
    let outside = tempfile::tempdir().unwrap();
    let (clip, settings) = fixture(root.path());
    let processor = MediaSaveProcessor::new(Arc::new(FailingFfmpeg));
    for path in [
        root.path().join("missing.mp4"),
        root.path().join("one.mp4"),
        outside.path().join("outside.mp4"),
    ] {
        let mut candidate = clip.clone();
        candidate.segment_files.as_mut().unwrap()[1] = path.to_string_lossy().into_owned();
        let error = processor
            .process(candidate, &settings)
            .unwrap_err()
            .to_string();
        assert!(
            !error.contains("fixture failure"),
            "FFmpeg must not run: {error}"
        );
        assert_eq!(fs::read(root.path().join("one.mp4")).unwrap(), [42; 64]);
    }
}

#[test]
fn unprocessed_save_is_categorized_without_running_ffmpeg() {
    let root = tempfile::tempdir().unwrap();
    let path = root.path().join("desktop.mp4");
    fs::write(&path, [77; 64]).unwrap();
    let clip = ClipRecord {
        id: "desktop".into(),
        game_or_app: "Desktop".into(),
        file_path: path.to_string_lossy().into_owned(),
        resolution: "320x180".into(),
        audio_tracks: vec!["system-loopback-pcm".into()],
        ..ClipRecord::default()
    };
    let settings =
        ClipSettings::defaults_with_save_folder(root.path().to_string_lossy().into_owned());
    let saved = MediaSaveProcessor::new(Arc::new(FailingFfmpeg))
        .process(clip, &settings)
        .unwrap();
    assert_eq!(saved.folder_name.as_deref(), Some("Explorer"));
    assert_eq!(saved.audio_tracks, ["System audio"]);
    assert_eq!(fs::read(&saved.file_path).unwrap(), [77; 64]);
    assert!(!path.exists());
}
