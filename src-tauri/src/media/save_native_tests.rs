//! Synthetic media only; no screen, microphone, or user library access.
use super::{
    AlwaysReady, CommandFfmpeg, FfmpegExecutor, FfmpegJob, FfmpegOutput, MediaSaveProcessor,
};
use crate::{
    contracts::{AudioSourceKind, AudioSourceRule, ClipRecord, ClipSettings},
    error::AppResult,
    save::SavedClipProcessor,
};
use std::{
    ffi::OsString,
    fs,
    path::{Path, PathBuf},
    sync::Arc,
};

struct SoftwareFixture(Arc<CommandFfmpeg>);
impl FfmpegExecutor for SoftwareFixture {
    fn run(&self, job: FfmpegJob) -> AppResult<FfmpegOutput> {
        if job.args.iter().any(|arg| arg == "h264_nvenc") {
            return Ok(FfmpegOutput {
                success: false,
                exit_code: Some(1),
                stdout: vec![],
                stderr: "Simulated unavailable NVENC for portable fallback coverage".into(),
            });
        }
        self.0.run(job)
    }
}

fn generate(executor: &dyn FfmpegExecutor, path: &Path, size: &str, audio: bool) {
    let mut args: Vec<OsString> = [
        "-hide_banner",
        "-loglevel",
        "error",
        "-nostdin",
        "-f",
        "lavfi",
        "-i",
    ]
    .map(Into::into)
    .to_vec();
    args.push(format!("color=c=blue:s={size}:r=30:d=1").into());
    if audio {
        args.extend(
            [
                "-f",
                "lavfi",
                "-i",
                "anullsrc=r=48000:cl=stereo",
                "-map",
                "0:v:0",
                "-map",
                "1:a:0",
                "-map",
                "1:a:0",
                "-map",
                "1:a:0",
                "-map",
                "1:a:0",
            ]
            .map(Into::into),
        );
    }
    args.extend(
        [
            "-t", "1", "-threads", "2", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac",
        ]
        .map(Into::into),
    );
    args.push(path.as_os_str().into());
    let result = executor
        .run(FfmpegJob::new("generate segmented save fixture", args))
        .unwrap();
    assert!(result.success, "{}", result.stderr);
}

#[test]
#[ignore = "requires explicit CLIPTURE_TEST_FFMPEG; generates and decodes synthetic video"]
fn segmented_saves_decode_with_expected_dimensions_audio_and_duration() {
    let executable =
        PathBuf::from(std::env::var_os("CLIPTURE_TEST_FFMPEG").expect("Set explicit FFmpeg path"));
    assert!(executable.is_absolute());
    let ffmpeg = Arc::new(CommandFfmpeg::new(executable, Arc::new(AlwaysReady)));
    let processor = MediaSaveProcessor::new(Arc::new(SoftwareFixture(ffmpeg.clone())));
    for (sizes, audio) in [
        (["320x180", "320x180"], false),
        (["320x180", "160x90"], true),
    ] {
        let root = tempfile::tempdir().unwrap();
        // An apostrophe and spaces exercise concat manifest quoting.
        let files: Vec<_> = sizes
            .iter()
            .enumerate()
            .map(|(index, size)| {
                let path = root.path().join(format!("segment '{index}.mp4"));
                generate(ffmpeg.as_ref(), &path, size, audio);
                path.to_string_lossy().into_owned()
            })
            .collect();
        let mut settings =
            ClipSettings::defaults_with_save_folder(root.path().to_string_lossy().into_owned());
        settings.audio_sources.push(AudioSourceRule {
            kind: AudioSourceKind::App,
            enabled: true,
            process_name: Some("chat.exe".into()),
            ..AudioSourceRule::default()
        });
        let clip = ClipRecord {
            id: "segmented".into(),
            game_or_app: "Synthetic/Game".into(),
            is_game: Some(true),
            file_path: root
                .path()
                .join("finished.mp4")
                .to_string_lossy()
                .into_owned(),
            resolution: "320x180".into(),
            segment_files: Some(files.clone()),
            segment_resolutions: Some(sizes.map(String::from).into()),
            segment_audio_tracks: Some(vec![
                if audio {
                    [
                        "microphone-pcm",
                        "app:browser.exe",
                        "system-loopback-pcm",
                        "app:chat.exe",
                    ]
                    .map(String::from)
                    .into()
                } else {
                    vec![]
                };
                2
            ]),
            audio_tracks: if audio {
                [
                    "microphone-pcm",
                    "app:browser.exe",
                    "system-loopback-pcm",
                    "app:chat.exe",
                ]
                .map(String::from)
                .into()
            } else {
                vec![]
            },
            ..ClipRecord::default()
        };
        let saved = processor.process(clip, &settings).unwrap();
        assert!(saved.segment_files.is_none());
        assert_eq!(saved.folder_name.as_deref(), Some("Synthetic_Game"));
        assert_eq!(saved.resolution, "320x180");
        assert!(files.iter().all(|path| !Path::new(path).exists()));
        let mut args: Vec<OsString> = ["-hide_banner", "-loglevel", "error", "-nostdin", "-i"]
            .map(Into::into)
            .to_vec();
        args.push(saved.file_path.clone().into());
        args.extend(
            [
                "-map",
                "0:v:0",
                "-map",
                "0:a?",
                "-progress",
                "pipe:1",
                "-f",
                "null",
                "-",
            ]
            .map(Into::into),
        );
        let decoded = ffmpeg
            .run(FfmpegJob::new("decode complete saved fixture", args))
            .unwrap();
        assert!(decoded.success, "{}", decoded.stderr);
        let progress = String::from_utf8(decoded.stdout).unwrap();
        let duration = progress
            .lines()
            .filter_map(|line| line.strip_prefix("out_time_us=")?.parse::<u64>().ok())
            .max()
            .unwrap();
        assert!((1_950_000..2_150_000).contains(&duration), "{progress}");
        let mut frame: Vec<OsString> = ["-hide_banner", "-loglevel", "error", "-i"]
            .map(Into::into)
            .to_vec();
        frame.push(saved.file_path.clone().into());
        frame.extend(
            [
                "-map",
                "0:v:0",
                "-frames:v",
                "1",
                "-pix_fmt",
                "gray",
                "-f",
                "rawvideo",
                "pipe:1",
            ]
            .map(Into::into),
        );
        let frame = ffmpeg
            .run(FfmpegJob::new("verify saved dimensions", frame))
            .unwrap();
        assert!(frame.success, "{}", frame.stderr);
        assert_eq!(frame.stdout.len(), 320 * 180);
        if audio {
            assert_eq!(
                saved.audio_tracks,
                ["System audio", "microphone-pcm", "chat"]
            );
            for (track, expected) in [(2, true), (3, false)] {
                let mut args: Vec<OsString> = ["-hide_banner", "-loglevel", "error", "-i"]
                    .map(Into::into)
                    .to_vec();
                args.push(saved.file_path.clone().into());
                args.extend([
                    "-map".into(),
                    format!("0:a:{track}").into(),
                    "-f".into(),
                    "null".into(),
                    "-".into(),
                ]);
                assert_eq!(
                    ffmpeg
                        .run(FfmpegJob::new("verify saved audio count", args))
                        .unwrap()
                        .success,
                    expected
                );
            }
        }
        assert!(fs::read_dir(root.path()).unwrap().all(|entry| !entry
            .unwrap()
            .file_name()
            .to_string_lossy()
            .starts_with(".clipture-save-")));
        println!("Saved and decoded {sizes:?}, audio={audio}, duration={duration} us");
    }
}
