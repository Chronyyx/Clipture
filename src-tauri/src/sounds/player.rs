use super::SoundLibrary;
use crate::error::AppResult;
use std::{path::Path, sync::Arc};

pub trait SoundPlayer: Send + Sync {
    fn play(&self, path: &Path) -> AppResult<()>;
}

#[derive(Default)]
pub struct SilentSoundPlayer;
impl SoundPlayer for SilentSoundPlayer {
    fn play(&self, _: &Path) -> AppResult<()> {
        Ok(())
    }
}

pub struct SoundService {
    library: Arc<SoundLibrary>,
    player: Arc<dyn SoundPlayer>,
}
impl SoundService {
    pub fn new(library: Arc<SoundLibrary>, player: Arc<dyn SoundPlayer>) -> Self {
        Self { library, player }
    }
    pub fn play(&self, sound_id: &str) -> AppResult<bool> {
        let Some(path) = self.library.resolve(sound_id)? else {
            return Ok(false);
        };
        self.player.play(&path)?;
        Ok(true)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{
        fs,
        sync::atomic::{AtomicBool, Ordering},
    };
    struct RecordingPlayer(AtomicBool);
    impl SoundPlayer for RecordingPlayer {
        fn play(&self, _: &Path) -> AppResult<()> {
            self.0.store(true, Ordering::Relaxed);
            Ok(())
        }
    }
    #[test]
    fn service_resolves_ids_before_dispatching_native_playback() {
        let root = tempfile::tempdir().unwrap();
        let sounds = root.path().join("sounds");
        fs::create_dir(&sounds).unwrap();
        fs::write(sounds.join("clip.wav"), b"wave").unwrap();
        let player = Arc::new(RecordingPlayer(AtomicBool::new(false)));
        let service =
            SoundService::new(Arc::new(SoundLibrary::new(&sounds, vec![])), player.clone());
        assert!(service.play("custom:clip.wav").unwrap());
        assert!(player.0.load(Ordering::Relaxed));
        assert!(!service.play("none").unwrap());
    }
}
