use std::sync::{
    atomic::{AtomicBool, Ordering},
    Arc,
};

/// One atomic admission point for saves and installation. Owned permits survive
/// awaits and release on error/cancellation, without holding a mutex guard.
#[derive(Default)]
pub struct CaptureOperationGate(Arc<AtomicBool>);

impl CaptureOperationGate {
    pub fn is_busy(&self) -> bool {
        self.0.load(Ordering::Acquire)
    }

    pub fn acquire(&self) -> Option<CaptureOperationLease> {
        self.0
            .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .ok()
            .map(|_| CaptureOperationLease(self.0.clone()))
    }
}

pub struct CaptureOperationLease(Arc<AtomicBool>);

impl Drop for CaptureOperationLease {
    fn drop(&mut self) {
        self.0.store(false, Ordering::Release);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn save_and_install_cannot_enter_together_and_failure_releases() {
        let gate = CaptureOperationGate::default();
        let save = gate.acquire().unwrap();
        assert!(gate.is_busy());
        assert!(gate.acquire().is_none());
        drop(save);
        let install = gate.acquire().unwrap();
        assert!(gate.acquire().is_none());
        drop(install);
        assert!(!gate.is_busy());
        assert!(gate.acquire().is_some());
    }

    #[test]
    fn concurrent_admission_has_exactly_one_owner() {
        let gate = Arc::new(CaptureOperationGate::default());
        let barrier = Arc::new(std::sync::Barrier::new(16));
        let threads: Vec<_> = (0..16)
            .map(|_| {
                let gate = gate.clone();
                let barrier = barrier.clone();
                std::thread::spawn(move || {
                    barrier.wait();
                    let permit = gate.acquire();
                    barrier.wait();
                    permit.is_some()
                })
            })
            .collect();
        assert_eq!(
            threads
                .into_iter()
                .filter_map(|t| t.join().ok())
                .filter(|won| *won)
                .count(),
            1
        );
        assert!(!gate.is_busy());
    }
}
