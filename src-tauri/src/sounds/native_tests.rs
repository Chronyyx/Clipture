//! Opt-in hardware-output test: generated silence only; no microphone/capture.
use super::{DecodedSoundPlayer, DecodedWave, SoundLibrary, SoundService, WaveSoundSink};
use crate::error::AppResult;
use crate::{
    media::{AlwaysReady, CommandFfmpeg, FfmpegExecutor, FfmpegJob},
    platform::windows::WindowsWaveSoundSink,
};
use std::{
    ffi::OsString,
    path::PathBuf,
    sync::{mpsc, Arc},
    time::Duration,
};

struct ReportingSink(mpsc::SyncSender<Result<(), String>>);
impl WaveSoundSink for ReportingSink {
    fn play_wave(&self, wave: &DecodedWave) -> AppResult<()> {
        let result = WindowsWaveSoundSink.play_wave(wave);
        self.0
            .send(result.as_ref().map(|_| ()).map_err(ToString::to_string))
            .unwrap();
        result
    }
}

#[test]
#[ignore = "requires explicit FFmpeg path and Windows audio output; plays generated silence"]
fn native_background_sound_formats_without_webview() {
    let executable = PathBuf::from(
        std::env::var_os("CLIPTURE_TEST_FFMPEG")
            .expect("Set CLIPTURE_TEST_FFMPEG explicitly for the silent native test"),
    );
    assert!(executable.is_absolute());
    let ffmpeg = Arc::new(CommandFfmpeg::new(executable, Arc::new(AlwaysReady)));
    let root = tempfile::tempdir().unwrap();
    let (sent, completed) = mpsc::sync_channel(1);
    let service = SoundService::new(
        Arc::new(SoundLibrary::new(root.path(), vec![])),
        Arc::new(DecodedSoundPlayer::new(
            ffmpeg.clone(),
            Arc::new(ReportingSink(sent)),
        )),
    );
    for (extension, codec) in [
        ("wav", "pcm_s16le"),
        ("mp3", "libmp3lame"),
        ("ogg", "libvorbis"),
    ] {
        let path = root.path().join(format!("silent fixture.{extension}"));
        let mut args: Vec<OsString> = [
            "-nostdin",
            "-hide_banner",
            "-loglevel",
            "error",
            "-f",
            "lavfi",
            "-i",
            "anullsrc=r=48000:cl=stereo",
            "-t",
            "0.1",
            "-c:a",
            codec,
        ]
        .into_iter()
        .map(Into::into)
        .collect();
        args.push(path.as_os_str().to_owned());
        let generated = ffmpeg
            .run(FfmpegJob::new("generate silent sound fixture", args))
            .unwrap();
        assert!(generated.success, "{extension}: {}", generated.stderr);
        assert!(service
            .play(&format!("custom:silent fixture.{extension}"))
            .unwrap());
        completed
            .recv_timeout(Duration::from_secs(10))
            .unwrap()
            .unwrap();
        println!("Native {extension} decode/play completed without a WebView");
    }
}
