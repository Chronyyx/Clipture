use super::{DecodedSoundPlayer, DecodedWave, SoundPlayer, WaveSoundSink};
use crate::{
    error::AppResult,
    media::{FfmpegExecutor, FfmpegJob, FfmpegOutput},
};
use std::{
    path::Path,
    sync::{Arc, Condvar, Mutex},
    time::{Duration, Instant},
};

struct GateDecoder {
    gate: Arc<(Mutex<bool>, Condvar)>,
}
impl FfmpegExecutor for GateDecoder {
    fn run(&self, job: FfmpegJob) -> AppResult<FfmpegOutput> {
        assert_eq!(job.maximum_stdout_bytes, 16 * 1024 * 1024);
        let (gate, ready) = self.gate.as_ref();
        let mut released = gate.lock().unwrap();
        while !*released {
            released = ready.wait(released).unwrap();
        }
        Ok(FfmpegOutput {
            success: false,
            exit_code: Some(1),
            stdout: vec![],
            stderr: "fixture failure".into(),
        })
    }
}
struct NoPlayback;
impl WaveSoundSink for NoPlayback {
    fn play_wave(&self, _: &DecodedWave) -> AppResult<()> {
        panic!("Failed decode must not reach audio output")
    }
}

#[test]
fn feedback_workers_are_bounded_and_failure_releases_the_slot() {
    let gate = Arc::new((Mutex::new(false), Condvar::new()));
    let player = DecodedSoundPlayer::new(
        Arc::new(GateDecoder { gate: gate.clone() }),
        Arc::new(NoPlayback),
    );
    player.play(Path::new("C:/Fixture/sound.ogg")).unwrap();
    assert!(player.play(Path::new("C:/Fixture/second.wav")).is_err());
    *gate.0.lock().unwrap() = true;
    gate.1.notify_all();
    let deadline = Instant::now() + Duration::from_secs(2);
    loop {
        if player.play(Path::new("C:/Fixture/retry.mp3")).is_ok() {
            break;
        }
        assert!(
            Instant::now() < deadline,
            "Failed worker did not release the playback slot"
        );
        std::thread::sleep(Duration::from_millis(5));
    }
}
