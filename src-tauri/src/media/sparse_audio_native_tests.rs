use super::{AlwaysReady, CommandFfmpeg, FfmpegExecutor, FfmpegJob, MediaSaveProcessor};
use crate::{
    contracts::{AudioSourceKind, AudioSourceRule, ClipRecord, ClipSettings},
    save::SavedClipProcessor,
};
use std::{
    ffi::OsString,
    path::{Path, PathBuf},
    sync::Arc,
};

fn generate(ffmpeg: &dyn FfmpegExecutor, path: &Path, second: bool) {
    let mut args: Vec<OsString> = [
        "-hide_banner",
        "-loglevel",
        "error",
        "-nostdin",
        "-f",
        "lavfi",
        "-i",
        "color=c=blue:s=128x72:r=30:d=1",
        "-f",
        "lavfi",
        "-i",
        "sine=frequency=440:sample_rate=48000:duration=1",
        "-f",
        "lavfi",
        "-i",
        "sine=frequency=880:sample_rate=48000:duration=1",
        "-map",
        "0:v:0",
    ]
    .map(Into::into)
    .to_vec();
    if second {
        args.extend(["-map", "2:a:0"].map(Into::into));
    }
    args.extend(
        [
            "-map", "1:a:0", "-t", "1", "-threads", "2", "-c:v", "libx264", "-pix_fmt", "yuv420p",
            "-c:a", "aac",
        ]
        .map(Into::into),
    );
    args.push(path.as_os_str().into());
    let result = ffmpeg
        .run(FfmpegJob::new("generate sparse audio fixture", args))
        .unwrap();
    assert!(result.success, "{}", result.stderr);
}

fn samples(ffmpeg: &dyn FfmpegExecutor, path: &str, track: usize) -> Vec<f32> {
    let mut args: Vec<OsString> = ["-hide_banner", "-loglevel", "error", "-nostdin", "-i"]
        .map(Into::into)
        .to_vec();
    args.push(path.into());
    args.extend([
        "-map".into(),
        format!("0:a:{track}").into(),
        "-ac".into(),
        "1".into(),
        "-ar".into(),
        "48000".into(),
        "-f".into(),
        "f32le".into(),
        "pipe:1".into(),
    ]);
    let result = ffmpeg
        .run(FfmpegJob::new("decode sparse audio fixture", args))
        .unwrap();
    assert!(result.success, "{}", result.stderr);
    result
        .stdout
        .chunks_exact(4)
        .map(|bytes| f32::from_le_bytes(bytes.try_into().unwrap()))
        .collect()
}

fn rms(values: &[f32]) -> f32 {
    (values.iter().map(|value| value * value).sum::<f32>() / values.len() as f32).sqrt()
}
fn frequency(values: &[f32]) -> f32 {
    let rising = values
        .windows(2)
        .filter(|pair| pair[0] <= 0.0 && pair[1] > 0.0)
        .count();
    rising as f32 * 48_000.0 / values.len() as f32
}

#[test]
#[ignore = "requires explicit CLIPTURE_TEST_FFMPEG; generates and decodes tones but never plays them"]
fn sparse_segment_audio_preserves_identity_silence_and_timing() {
    let executable =
        PathBuf::from(std::env::var_os("CLIPTURE_TEST_FFMPEG").expect("Set explicit FFmpeg path"));
    assert!(executable.is_absolute());
    let ffmpeg = Arc::new(CommandFfmpeg::new(executable, Arc::new(AlwaysReady)));
    let root = tempfile::tempdir().unwrap();
    let first = root.path().join("first.mp4");
    let second = root.path().join("second.mp4");
    generate(ffmpeg.as_ref(), &first, false);
    generate(ffmpeg.as_ref(), &second, true);
    let mut settings =
        ClipSettings::defaults_with_save_folder(root.path().to_string_lossy().into_owned());
    settings.audio_sources.push(AudioSourceRule {
        kind: AudioSourceKind::App,
        enabled: true,
        process_name: Some("chat.exe".into()),
        ..AudioSourceRule::default()
    });
    let clip = ClipRecord {
        id: "sparse".into(),
        file_path: root
            .path()
            .join("complete.mp4")
            .to_string_lossy()
            .into_owned(),
        resolution: "128x72".into(),
        audio_tracks: vec!["microphone-pcm".into(), "app:chat.exe".into()],
        segment_files: Some(vec![
            first.to_string_lossy().into_owned(),
            second.to_string_lossy().into_owned(),
        ]),
        segment_resolutions: Some(vec!["128x72".into(); 2]),
        segment_audio_tracks: Some(vec![
            vec!["microphone-pcm".into()],
            vec!["app:chat.exe".into(), "microphone-pcm".into()],
        ]),
        ..ClipRecord::default()
    };
    let saved = MediaSaveProcessor::new(ffmpeg.clone())
        .process(clip, &settings)
        .unwrap();
    assert_eq!(saved.audio_tracks, ["microphone-pcm", "chat"]);
    let mic = samples(ffmpeg.as_ref(), &saved.file_path, 0);
    let chat = samples(ffmpeg.as_ref(), &saved.file_path, 1);
    let early = 12_000..36_000;
    let late = 60_000..84_000;
    assert!(mic.len() >= late.end && chat.len() >= late.end);
    assert!(
        rms(&chat[early.clone()]) < 0.001,
        "Absent first-segment app track must remain silent"
    );
    assert!(
        rms(&chat[late.clone()]) > 0.02,
        "App track must start in the second segment"
    );
    for region in [early.clone(), late.clone()] {
        assert!(rms(&mic[region.clone()]) > 0.02);
        assert!(
            (frequency(&mic[region]) - 440.0).abs() < 20.0,
            "Mic identity must survive stream reordering"
        );
    }
    assert!((frequency(&chat[late]) - 880.0).abs() < 20.0);
    assert!(!first.exists() && !second.exists());
    println!("Sparse audio passed: mic 440 Hz continuous; app silent then 880 Hz; reversed source order corrected");
}
