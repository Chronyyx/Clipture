use std::{
    sync::{
        atomic::{AtomicUsize, Ordering},
        Arc,
    },
    time::Duration,
};
use tokio::sync::{OwnedSemaphorePermit, Semaphore};

// Six commands run at once, two of them reserved for opening/releasing
// playback so a burst of background work (process icons, diagnostics) cannot
// starve the player. Media range reads and thumbnails are admitted separately
// by `media::MediaAdmission`.
//
// A command that finds the slots full waits its turn instead of being turned
// away: a window opening sends a burst, and rejecting the overflow left
// whatever lost the race (the friends list) failing until the user retried.
// Only a host that stays saturated for `QUEUE_TIMEOUT`, or a queue past
// `MAXIMUM_WAITING`, answers busy.
pub const QUEUE_TIMEOUT: Duration = Duration::from_secs(15);
pub const MAXIMUM_WAITING: usize = 128;

pub struct Admission {
    total: Arc<Semaphore>,
    background: Arc<Semaphore>,
    waiting: Arc<AtomicUsize>,
    timeout: Duration,
}

pub struct Permit {
    _total: OwnedSemaphorePermit,
    _background: Option<OwnedSemaphorePermit>,
}

impl Admission {
    pub fn new() -> Self {
        Self::with_timeout(QUEUE_TIMEOUT)
    }

    fn with_timeout(timeout: Duration) -> Self {
        Self {
            total: Arc::new(Semaphore::new(6)),
            background: Arc::new(Semaphore::new(4)),
            waiting: Arc::new(AtomicUsize::new(0)),
            timeout,
        }
    }

    /// Waits for a slot; `None` when the host stayed busy past the timeout
    /// or too many requests are already waiting.
    pub async fn acquire(&self, command: Option<&str>) -> Option<Permit> {
        let interactive = matches!(command, Some("clip_playback_url" | "release_playback_cache"));
        if self.waiting.fetch_add(1, Ordering::AcqRel) >= MAXIMUM_WAITING {
            self.waiting.fetch_sub(1, Ordering::AcqRel);
            return None;
        }
        let total = self.total.clone();
        let background = self.background.clone();
        // A background command takes its background slot before a total
        // slot, so waiting background work never holds the playback reserve.
        let permit = tokio::time::timeout(self.timeout, async move {
            let background = if interactive {
                None
            } else {
                Some(background.acquire_owned().await.ok()?)
            };
            let total = total.acquire_owned().await.ok()?;
            Some(Permit { _total: total, _background: background })
        })
        .await
        .ok()
        .flatten();
        self.waiting.fetch_sub(1, Ordering::AcqRel);
        permit
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn quick() -> Admission {
        Admission::with_timeout(Duration::from_millis(200))
    }

    #[tokio::test]
    async fn a_burst_waits_its_turn_instead_of_failing() {
        let gate = Arc::new(Admission::new());
        let held: Vec<_> = futures_join((0..4).map(|_| gate.acquire(None))).await;
        let waiter = {
            let gate = gate.clone();
            tokio::spawn(async move { gate.acquire(Some("sharing_get_state")).await.is_some() })
        };
        tokio::time::sleep(Duration::from_millis(50)).await;
        assert!(!waiter.is_finished(), "the fifth background command must wait, not fail");
        drop(held);
        assert!(waiter.await.unwrap(), "a freed slot admits the waiting command");
    }

    #[tokio::test]
    async fn saturated_background_cannot_starve_playback() {
        let gate = quick();
        let background: Vec<_> = futures_join((0..4).map(|_| gate.acquire(None))).await;
        assert!(gate.acquire(Some("get_diagnostics")).await.is_none(), "times out while saturated");
        let playback = gate.acquire(Some("clip_playback_url")).await.unwrap();
        let release = gate.acquire(Some("release_playback_cache")).await.unwrap();
        assert!(gate.acquire(Some("clip_playback_url")).await.is_none());
        drop(playback);
        assert!(gate.acquire(Some("clip_playback_url")).await.is_some());
        drop(release);
        drop(background);
        assert_eq!(gate.total.available_permits(), 6);
        assert_eq!(gate.background.available_permits(), 4);
    }

    #[tokio::test]
    async fn waiting_background_work_does_not_hold_the_playback_reserve() {
        let gate = Arc::new(quick());
        let background: Vec<_> = futures_join((0..4).map(|_| gate.acquire(None))).await;
        let queued = {
            let gate = gate.clone();
            tokio::spawn(async move { gate.acquire(None).await.is_some() })
        };
        tokio::time::sleep(Duration::from_millis(20)).await;
        assert!(gate.acquire(Some("clip_playback_url")).await.is_some(), "playback admitted past a queue");
        assert!(!queued.await.unwrap(), "the queued command times out while background stays full");
        drop(background);
        assert_eq!(gate.waiting.load(Ordering::Acquire), 0);
    }

    #[tokio::test]
    async fn a_runaway_queue_is_turned_away() {
        let gate = Arc::new(Admission::new());
        let held: Vec<_> = futures_join((0..4).map(|_| gate.acquire(None))).await;
        let waiters: Vec<_> = (0..MAXIMUM_WAITING)
            .map(|_| {
                let gate = gate.clone();
                tokio::spawn(async move { gate.acquire(None).await.is_some() })
            })
            .collect();
        while gate.waiting.load(Ordering::Acquire) < MAXIMUM_WAITING {
            tokio::task::yield_now().await;
        }
        assert!(gate.acquire(None).await.is_none(), "past the queue limit is busy at once");
        drop(held);
        for waiter in waiters {
            assert!(waiter.await.unwrap(), "every queued command is served once slots free");
        }
    }

    async fn futures_join<F: std::future::Future<Output = Option<Permit>>>(
        acquisitions: impl Iterator<Item = F>,
    ) -> Vec<Permit> {
        let mut permits = Vec::new();
        for acquisition in acquisitions {
            permits.push(acquisition.await.expect("free slot"));
        }
        permits
    }
}
