//! A bounded replay window permits concurrent worker senders to arrive out of
//! order, but never permits a duplicate or ancient ID to repeat a mutation.
use std::collections::HashSet;
const WINDOW: u64 = 4096;

#[derive(Default)]
pub struct RequestIds {
    greatest: u64,
    seen: HashSet<u64>,
}
impl RequestIds {
    pub fn accept(&mut self, id: u64) -> bool {
        if id == 0 || id <= self.greatest.saturating_sub(WINDOW) || self.seen.contains(&id) {
            return false;
        }
        if id > self.greatest {
            self.greatest = id;
            let floor = id.saturating_sub(WINDOW);
            self.seen.retain(|previous| *previous > floor);
        }
        self.seen.insert(id)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn reordered_requests_are_valid_but_replays_and_zero_are_not() {
        let mut ids = RequestIds::default();
        assert!(ids.accept(3));
        assert!(ids.accept(1));
        assert!(ids.accept(2));
        assert!(!ids.accept(1));
        assert!(!ids.accept(0));
        for id in 4..20_000 {
            assert!(ids.accept(id));
        }
        assert!(!ids.accept(1));
        assert!(ids.seen.len() <= WINDOW as usize);
        assert!(ids.accept(u64::MAX));
        assert!(!ids.accept(u64::MAX));
        assert!(ids.accept(u64::MAX - 1));
    }
}
