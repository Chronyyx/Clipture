use std::sync::Arc;
use tokio::sync::{OwnedSemaphorePermit, Semaphore};

// Keep the total at six, reserving two for opening/releasing playback even
// when thumbnails, icons and diagnostic work saturate background slots. Media
// range reads are admitted separately by `media::MediaAdmission`.
pub struct Admission {
    total: Arc<Semaphore>,
    background: Arc<Semaphore>,
}

pub struct Permit {
    _total: OwnedSemaphorePermit,
    _background: Option<OwnedSemaphorePermit>,
}

impl Admission {
    pub fn new() -> Self {
        Self { total: Arc::new(Semaphore::new(6)), background: Arc::new(Semaphore::new(4)) }
    }

    pub fn acquire(&self, command: Option<&str>) -> Option<Permit> {
        let interactive = matches!(command, Some("clip_playback_url" | "release_playback_cache"));
        let background = if interactive { None } else {
            Some(self.background.clone().try_acquire_owned().ok()?)
        };
        let total = self.total.clone().try_acquire_owned().ok()?;
        Some(Permit { _total: total, _background: background })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn saturated_background_cannot_starve_playback() {
        let gate = Admission::new();
        let background: Vec<_> = (0..4).map(|_| gate.acquire(None).unwrap()).collect();
        assert!(gate.acquire(None).is_none());
        assert!(gate.acquire(Some("get_diagnostics")).is_none());
        let playback = gate.acquire(Some("clip_playback_url")).unwrap();
        let release = gate.acquire(Some("release_playback_cache")).unwrap();
        assert!(gate.acquire(Some("clip_playback_url")).is_none());
        drop(playback);
        assert!(gate.acquire(Some("clip_playback_url")).is_some());
        drop(release);
        drop(background);
        assert_eq!(gate.total.available_permits(), 6);
        assert_eq!(gate.background.available_permits(), 4);
    }

    #[test]
    fn rejected_background_request_returns_its_background_permit() {
        let gate = Admission::new();
        let interactive: Vec<_> = (0..6).map(|_| gate.acquire(Some("clip_playback_url")).unwrap()).collect();
        assert!(gate.acquire(None).is_none());
        assert_eq!(gate.background.available_permits(), 4);
        drop(interactive);
        assert!(gate.acquire(None).is_some());
    }
}
