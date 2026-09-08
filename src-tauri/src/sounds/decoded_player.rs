use super::{decoder::decode_wave, DecodedWave, SoundPlayer};
use crate::{
    error::{AppError, AppResult},
    media::FfmpegExecutor,
};
use std::{
    path::Path,
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc,
    },
};

/// The sink must finish using the bytes before returning.
pub trait WaveSoundSink: Send + Sync {
    fn play_wave(&self, wave: &DecodedWave) -> AppResult<()>;
}

pub struct DecodedSoundPlayer {
    ffmpeg: Arc<dyn FfmpegExecutor>,
    sink: Arc<dyn WaveSoundSink>,
    busy: Arc<AtomicBool>,
}

impl DecodedSoundPlayer {
    pub fn new(ffmpeg: Arc<dyn FfmpegExecutor>, sink: Arc<dyn WaveSoundSink>) -> Self {
        Self {
            ffmpeg,
            sink,
            busy: Arc::new(AtomicBool::new(false)),
        }
    }
}

struct PlaybackPermit(Arc<AtomicBool>);
impl Drop for PlaybackPermit {
    fn drop(&mut self) {
        self.0.store(false, Ordering::Release);
    }
}

impl SoundPlayer for DecodedSoundPlayer {
    fn play(&self, path: &Path) -> AppResult<()> {
        if self
            .busy
            .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .is_err()
        {
            return Err(AppError::Integration(
                "A clip feedback sound is already playing".into(),
            ));
        }
        let permit = PlaybackPermit(self.busy.clone());
        let ffmpeg = self.ffmpeg.clone();
        let sink = self.sink.clone();
        let path = path.to_owned();
        std::thread::Builder::new()
            .name("clipture-sound".into())
            .spawn(move || {
                let _permit = permit;
                if let Err(error) =
                    decode_wave(ffmpeg.as_ref(), &path).and_then(|wave| sink.play_wave(&wave))
                {
                    tracing::warn!(%error, "native clip sound failed");
                }
            })
            .map_err(|error| AppError::Integration(format!("Start clip sound worker: {error}")))?;
        Ok(())
    }
}
