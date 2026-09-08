use crate::{
    error::{AppError, AppResult},
    sounds::{DecodedWave, WaveSoundSink},
};

pub struct WindowsWaveSoundSink;

impl WaveSoundSink for WindowsWaveSoundSink {
    fn play_wave(&self, wave: &DecodedWave) -> AppResult<()> {
        #[link(name = "winmm")]
        extern "system" {
            fn PlaySoundW(sound: *const u8, module: usize, flags: u32) -> i32;
        }
        // SAFETY: the domain decoder supplies a complete bounded PCM RIFF/WAVE.
        // SND_SYNC keeps this buffer alive until native playback finishes.
        // SND_NODEFAULT prevents unexpected beeps when no output device exists.
        let played = unsafe { PlaySoundW(wave.as_bytes().as_ptr(), 0, 0x0004 | 0x0002) };
        if played == 0 {
            Err(AppError::Integration(
                "Windows could not play the clip sound".into(),
            ))
        } else {
            Ok(())
        }
    }
}
