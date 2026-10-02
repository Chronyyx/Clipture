//! Short interface cues synthesized in memory: no bundled files, no decoder
//! process. Each note is a soft bell (a sine with a quiet octave partial and
//! an exponential decay), so cues stay gentle at any system volume.
use std::sync::{
    atomic::{AtomicBool, Ordering},
    Arc,
};

use super::{decoder::wave_from_pcm, DecodedWave, WaveSoundSink};
use crate::error::AppResult;

const RATE: f32 = 48_000.0;
const ATTACK_SECONDS: f32 = 0.004;
/// Peak level after mixing, well under full scale.
const PEAK: f32 = 0.32;
const TAIL_SECONDS: f32 = 0.05;

/// One bell strike.
#[derive(Clone, Copy, Debug)]
pub struct Note {
    pub hz: f32,
    pub at_seconds: f32,
    /// Time for the note to fall to about 5% of its level.
    pub ring_seconds: f32,
    pub gain: f32,
}

/// Mixes `notes` into a playable wave.
pub fn synthesize(notes: &[Note]) -> AppResult<DecodedWave> {
    let end = notes
        .iter()
        .map(|note| note.at_seconds + note.ring_seconds)
        .fold(0.0_f32, f32::max)
        + TAIL_SECONDS;
    let frames = (end * RATE).ceil() as usize;
    let mut mix = vec![0.0_f32; frames];
    for note in notes {
        let first = (note.at_seconds * RATE) as usize;
        let length = ((note.ring_seconds * RATE) as usize).min(frames.saturating_sub(first));
        // exp(-3) is about 5%: the note has faded by `ring_seconds`.
        let decay = 3.0 / (note.ring_seconds * RATE);
        for index in 0..length {
            let time = index as f32 / RATE;
            let phase = std::f32::consts::TAU * note.hz * time;
            let attack = (time / ATTACK_SECONDS).min(1.0);
            let envelope = attack * (-decay * index as f32).exp();
            let tone = phase.sin() + 0.18 * (2.0 * phase).sin() * (-2.0 * decay * index as f32).exp();
            mix[first + index] += note.gain * envelope * tone;
        }
    }
    let loudest = mix.iter().fold(0.0_f32, |peak, sample| peak.max(sample.abs()));
    let scale = if loudest > 0.0 { PEAK / loudest } else { 0.0 };
    let mut pcm = Vec::with_capacity(frames * 4);
    for sample in mix {
        let value = ((sample * scale).clamp(-1.0, 1.0) * i16::MAX as f32) as i16;
        let bytes = value.to_le_bytes();
        pcm.extend_from_slice(&bytes);
        pcm.extend_from_slice(&bytes);
    }
    wave_from_pcm(pcm)
}

/// Plays cues off the calling thread. A cue that arrives while another is
/// playing is dropped rather than queued, so bursts never pile up.
pub struct CuePlayer {
    sink: Arc<dyn WaveSoundSink>,
    busy: Arc<AtomicBool>,
}

impl CuePlayer {
    pub fn new(sink: Arc<dyn WaveSoundSink>) -> Self {
        Self {
            sink,
            busy: Arc::new(AtomicBool::new(false)),
        }
    }

    pub fn play(&self, notes: &[Note]) {
        if self
            .busy
            .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .is_err()
        {
            return;
        }
        let busy = self.busy.clone();
        let sink = self.sink.clone();
        let notes = notes.to_vec();
        let spawned = std::thread::Builder::new()
            .name("clipture-cue".into())
            .spawn(move || {
                if let Err(error) = synthesize(&notes).and_then(|wave| sink.play_wave(&wave)) {
                    tracing::debug!(%error, "interface cue failed");
                }
                busy.store(false, Ordering::Release);
            });
        if spawned.is_err() {
            self.busy.store(false, Ordering::Release);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Mutex;

    #[test]
    fn notes_mix_into_a_bounded_stereo_wave() {
        let wave = synthesize(&[
            Note { hz: 660.0, at_seconds: 0.0, ring_seconds: 0.3, gain: 1.0 },
            Note { hz: 990.0, at_seconds: 0.1, ring_seconds: 0.3, gain: 0.8 },
        ])
        .unwrap();
        let bytes = wave.as_bytes();
        let frames = (bytes.len() - 44) / 4;
        assert_eq!(frames, ((0.4 + TAIL_SECONDS) * RATE).ceil() as usize);
        let peak = bytes[44..]
            .chunks_exact(2)
            .map(|pair| i16::from_le_bytes([pair[0], pair[1]]).unsigned_abs())
            .max()
            .unwrap();
        let expected = (PEAK * i16::MAX as f32) as u16;
        assert!(peak <= expected && peak > expected - 64, "normalized to the cue peak");
    }

    struct Recording(Mutex<usize>);
    impl WaveSoundSink for Recording {
        fn play_wave(&self, wave: &DecodedWave) -> AppResult<()> {
            assert!(wave.as_bytes().len() > 44);
            *self.0.lock().unwrap() += 1;
            Ok(())
        }
    }

    #[test]
    fn cues_play_in_the_background() {
        let sink = Arc::new(Recording(Mutex::new(0)));
        let player = CuePlayer::new(sink.clone());
        player.play(&[Note { hz: 440.0, at_seconds: 0.0, ring_seconds: 0.1, gain: 1.0 }]);
        for _ in 0..100 {
            if *sink.0.lock().unwrap() == 1 {
                return;
            }
            std::thread::sleep(std::time::Duration::from_millis(10));
        }
        panic!("the cue never played");
    }
}
