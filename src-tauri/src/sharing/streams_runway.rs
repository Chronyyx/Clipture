//! Runways: the sequential fetches behind a stream session.
//!
//! Each runway fetches forward in order from one read position, in requests
//! of `REQUEST_BLOCKS`. Players read from several places at once (Clipture
//! stores audio apart from video, and MP4 indexes sit at the end), so up to
//! `MAX_RUNWAYS` run side by side. The link is shared by priority, as in a
//! torrent streamer: a runway less than `LANE_AHEAD_BLOCKS` ahead of what the
//! player last read through it is urgent; while any runway is urgent the
//! others wait, so the part being watched gets the whole upload. Once every
//! read position is comfortably ahead, runways fill the rest of the clip: in
//! playback order once the MP4 index has arrived (see `streams_order.rs`),
//! otherwise front to back.
use std::{
    sync::{Arc, Mutex, Weak},
    time::{Duration, Instant},
};

use tokio::{sync::Notify, task::AbortHandle, time::timeout};

use crate::error::{AppError, AppResult};

use super::{
    super::{
        node::{is_unavailable, Node},
        wire::RangePurpose,
    },
    local_blocks,
    order::{find_moov, playback_order, Lookup, PlaybackOrder},
    store::BlockStore,
    StreamRegistry, StreamTarget, BLOCK_BYTES,
};

/// A read this close ahead of a runway waits for it instead of starting one.
const RUNWAY_SLACK_BLOCKS: u64 = 4;
/// Read positions served at once: video, audio, index, plus a seek.
const MAX_RUNWAYS: usize = 4;
/// How far a runway runs ahead of its reader while others are urgent.
const LANE_AHEAD_BLOCKS: u64 = 32;
/// One request's length. Waiting between requests really stops the sender;
/// a single request to the end would keep it pushing a full flow window.
const REQUEST_BLOCKS: u64 = 8;
/// A runway's reader counts as active this long after its last read. A
/// reader blocked on it renews this while it waits.
const READER_ACTIVE: Duration = Duration::from_secs(1);
/// How often a blocked reader renews its claim.
const WAIT_RENEW: Duration = Duration::from_millis(250);
/// How often a waiting runway re-checks whether it may go.
const YIELD_POLL: Duration = Duration::from_millis(200);
const READ_TIMEOUT: Duration = Duration::from_secs(30);
const RETRY_DELAY: Duration = Duration::from_millis(500);
const MAX_FAILURES: u32 = 20;
const PROGRESS_INTERVAL: Duration = Duration::from_millis(250);

#[derive(Default)]
pub(super) struct Cache {
    /// The bytes received, on disk.
    pub(super) store: BlockStore,
    /// The block the player most recently needed (the playhead).
    wanted: u64,
    pub(super) runways: Vec<Runway>,
    last_runway: u64,
    failure: Option<String>,
    /// The clip's playback order, once its index has arrived.
    order: Option<PlaybackOrder>,
    /// The clip has no usable index; fill front to back.
    unordered: bool,
    /// Where in the playback order the player is.
    cursor: usize,
}

pub(super) struct Runway {
    id: u64,
    /// Where it started; it has delivered (or skipped) `from..next`.
    from: u64,
    /// The next block it will deliver.
    next: u64,
    /// The end of its request in flight: `next..until` is claimed.
    until: u64,
    /// The furthest block the player has read through it.
    read: u64,
    /// When the player last needed something from it.
    used: Instant,
    /// Filling ahead in playback order for no reader in particular; never
    /// urgent until the player reads through it.
    filling: bool,
    pub(super) abort: AbortHandle,
}

impl Runway {
    /// Close enough to a reader that is still reading that the player may
    /// soon wait on it. A position the player has left (the opening read at
    /// the start, a seek it moved on from) is not worth the link.
    fn urgent(&self) -> bool {
        !self.filling
            && self.next < self.read + LANE_AHEAD_BLOCKS
            && self.used.elapsed() < READER_ACTIVE
    }
}

/// What a runway does next.
#[derive(Debug, PartialEq)]
enum Step {
    /// Fetch `count` blocks from `start`.
    Fetch { start: u64, count: u64 },
    /// Others are urgent, or the cache is full ahead.
    Wait,
    /// Nothing left, or it reached another runway.
    Stop,
}

impl Cache {
    /// First block at or after `from` that is neither cached nor local.
    pub(super) fn first_missing(&self, from: u64, local: u64, total: u64) -> Option<u64> {
        (from.max(local)..total).find(|index| !self.store.contains(*index))
    }

    /// A runway (other than `except`) that will deliver `block` shortly.
    fn lane_for(&mut self, block: u64, except: Option<u64>) -> Option<&mut Runway> {
        self.runways.iter_mut().find(|runway| {
            Some(runway.id) != except
                && runway.next <= block
                && block < runway.next + RUNWAY_SLACK_BLOCKS
        })
    }

    /// The player needs `block`: it is the playhead, and the runway that
    /// serves that position learns how far its reader has got.
    pub(super) fn note_read(&mut self, block: u64) {
        self.wanted = block;
        if let Some(rank) = self.order.as_ref().and_then(|order| order.rank.get(&block)) {
            self.cursor = *rank;
        }
        for runway in &mut self.runways {
            if runway.from <= block && block < runway.next + RUNWAY_SLACK_BLOCKS {
                runway.read = runway.read.max(block);
                runway.used = Instant::now();
                runway.filling = false;
            }
        }
    }

    /// Bytes at `offset` if every block they span has arrived.
    fn cached_bytes(&self, offset: u64, length: usize) -> Option<Vec<u8>> {
        self.store.read(offset, length)
    }

    /// Reads the playback order from the index once it has arrived.
    fn learn_order(&mut self, size: u64) {
        if self.order.is_some() || self.unordered {
            return;
        }
        match find_moov(size, |offset, length| self.cached_bytes(offset, length)) {
            Lookup::Pending => {}
            Lookup::Absent => self.unordered = true,
            Lookup::Found((offset, length)) => {
                let Some(index) = self.cached_bytes(offset, length as usize) else {
                    return; // The index itself is still arriving.
                };
                match playback_order(&index, BLOCK_BYTES) {
                    Some(order) => {
                        self.cursor = order.rank.get(&self.wanted).copied().unwrap_or(0);
                        self.order = Some(order);
                    }
                    None => self.unordered = true,
                }
            }
        }
    }

    /// Whether another runway's request in flight covers `block`.
    fn claimed(&self, block: u64, id: u64) -> bool {
        self.runways
            .iter()
            .any(|runway| runway.id != id && runway.next <= block && block < runway.until)
    }

    fn wanted_here(&self, block: u64, id: u64, local: u64) -> bool {
        block >= local && !self.store.contains(block) && !self.claimed(block, id)
    }

    /// The next block playback will need that nobody has or is fetching.
    fn next_in_order(&self, id: u64, local: u64) -> Option<u64> {
        let order = self.order.as_ref()?;
        order.blocks[self.cursor.min(order.blocks.len())..]
            .iter()
            .copied()
            .find(|block| self.wanted_here(*block, id, local))
    }

    fn fetch(&mut self, me: usize, start: u64, total: u64, local: u64) -> Step {
        let id = self.runways[me].id;
        let count = (start..total)
            .take(REQUEST_BLOCKS as usize)
            .take_while(|index| *index == start || self.wanted_here(*index, id, local))
            .count() as u64;
        self.runways[me].next = start;
        self.runways[me].until = start + count;
        Step::Fetch { start, count }
    }

    fn plan(&mut self, id: u64, local: u64, total: u64, size: u64) -> Step {
        let Some(me) = self.runways.iter().position(|runway| runway.id == id) else {
            return Step::Stop;
        };
        self.learn_order(size);
        // Onward past what is already here; after the end, the first gap.
        let next = self.runways[me].next;
        let onward = self
            .first_missing(next, local, total)
            .or_else(|| self.first_missing(0, local, total))
            .filter(|start| self.lane_for(*start, Some(id)).is_none() && !self.claimed(*start, id));
        let ordered = self.next_in_order(id, local);
        if onward.is_none() && ordered.is_none() {
            return Step::Stop; // Nothing left, or another runway carries on.
        }
        let urgent = self.runways[me].urgent();
        if !urgent && self.runways.iter().any(Runway::urgent) {
            return Step::Wait;
        }
        // An urgent runway serves its reader; an idle link goes to what
        // playback needs next, in playback order when the index is known.
        if let (false, Some(start)) = (urgent, ordered) {
            let runway = &mut self.runways[me];
            runway.filling = true;
            runway.from = start;
            runway.read = start;
            return self.fetch(me, start, total, local);
        }
        match onward {
            Some(start) => self.fetch(me, start, total, local),
            None => Step::Stop,
        }
    }
}

pub(super) struct Shared {
    cache: Mutex<Cache>,
    /// Bytes arrived: wakes readers.
    arrived: Notify,
    /// The player moved on, or a runway finished: wakes waiting runways.
    demand: Notify,
}

impl Shared {
    pub(super) fn new() -> Self {
        Self {
            cache: Mutex::default(),
            arrived: Notify::new(),
            demand: Notify::new(),
        }
    }

    pub(super) fn lock(&self) -> std::sync::MutexGuard<'_, Cache> {
        self.cache
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
    }

    pub(super) fn note_read(&self, block: u64) {
        self.lock().note_read(block);
        self.demand.notify_waiters();
    }
}

/// What a session's fetching needs; cheap to clone into tasks.
#[derive(Clone)]
pub(super) struct Fetch {
    pub(super) runtime: tokio::runtime::Handle,
    pub(super) node: Arc<Node>,
    pub(super) target: Arc<StreamTarget>,
    pub(super) shared: Arc<Shared>,
    pub(super) progress: Weak<StreamRegistry>,
}

impl Fetch {
    /// Starts a runway at `start`, retiring the least recently needed one
    /// when `MAX_RUNWAYS` are already running.
    pub(super) fn start_runway(&self, cache: &mut Cache, start: u64) {
        if cache.runways.len() >= MAX_RUNWAYS {
            if let Some(oldest) = (0..cache.runways.len()).min_by_key(|i| cache.runways[*i].used) {
                cache.runways.remove(oldest).abort.abort();
            }
        }
        cache.last_runway += 1;
        let id = cache.last_runway;
        let task = self.runtime.spawn(run_runway(self.clone(), id));
        cache.runways.push(Runway {
            id,
            from: start,
            next: start,
            until: start,
            read: start,
            used: Instant::now(),
            filling: false,
            abort: task.abort_handle(),
        });
    }

    fn progress(&self) {
        if let Some(registry) = self.progress.upgrade() {
            (registry.on_progress)();
        }
    }
}

/// Waits until `block` is cached, steering a runway to it.
pub(super) async fn wait_for(fetch: &Fetch, block: u64, local: u64) -> AppResult<()> {
    let deadline = Instant::now() + READ_TIMEOUT;
    loop {
        // Renewed on every wake: a reader that waits keeps its runway urgent.
        fetch.shared.note_read(block);
        let arrived = fetch.shared.arrived.notified();
        {
            let mut cache = fetch.shared.lock();
            if cache.store.contains(block) {
                return Ok(());
            }
            if cache.lane_for(block, None).is_none() {
                // A new read position (or a seek): fetch from here.
                let start = cache
                    .first_missing(block, local, fetch.target.blocks())
                    .unwrap_or(block);
                fetch.start_runway(&mut cache, start);
            }
        }
        let remaining = deadline.saturating_duration_since(Instant::now());
        if remaining.is_zero() {
            let failure = fetch.shared.lock().failure.clone();
            return Err(AppError::Path(failure.unwrap_or_else(|| {
                "your friend's connection is too slow right now".into()
            })));
        }
        let _ = timeout(remaining.min(WAIT_RENEW), arrived).await;
    }
}

/// One runway: plan, fetch a request's worth in order, repeat.
async fn run_runway(fetch: Fetch, id: u64) {
    let target = &fetch.target;
    let total = target.blocks();
    let mut last_progress = Instant::now();
    let mut failures = 0_u32;
    loop {
        let local = local_blocks(target);
        let step = fetch.shared.lock().plan(id, local, total, target.size);
        let (start, count) = match step {
            Step::Fetch { start, count } => (start, count),
            Step::Wait => {
                let demand = fetch.shared.demand.notified();
                let _ = timeout(YIELD_POLL, demand).await;
                continue;
            }
            Step::Stop => {
                fetch.shared.lock().runways.retain(|runway| runway.id != id);
                fetch.shared.demand.notify_waiters();
                fetch.progress();
                return;
            }
        };
        let offset = start * BLOCK_BYTES;
        let length = ((start + count) * BLOCK_BYTES).min(target.size) - offset;
        let opened = fetch
            .node
            .open_range(target.peer, &target.share_id, offset, length, RangePurpose::Watch)
            .await;
        let mut stream = match opened {
            Ok((size, stream)) if size == target.size => stream,
            Ok(_) => return fail(&fetch.shared, id, "the shared clip changed on your friend's PC"),
            Err(error) if is_unavailable(&error) || failures >= MAX_FAILURES => {
                return fail(&fetch.shared, id, &error.to_string())
            }
            Err(error) => {
                failures += 1;
                fetch.shared.lock().failure = Some(error.to_string());
                tokio::time::sleep(RETRY_DELAY).await;
                continue;
            }
        };
        for index in start..start + count {
            let mut block = vec![0_u8; target.block_len(index) as usize];
            if !matches!(
                timeout(READ_TIMEOUT, stream.read_exact(&mut block)).await,
                Ok(Ok(_))
            ) {
                failures += 1;
                if failures >= MAX_FAILURES {
                    return fail(&fetch.shared, id, "the connection to your friend was interrupted");
                }
                tokio::time::sleep(RETRY_DELAY).await;
                break; // Plan again from the first missing block.
            }
            failures = 0;
            {
                let mut cache = fetch.shared.lock();
                let Some(me) = cache.runways.iter_mut().find(|runway| runway.id == id) else {
                    return; // Retired.
                };
                me.next = index + 1;
                cache.failure = None;
                if let Err(error) = cache.store.put(index, &block) {
                    drop(cache);
                    tracing::warn!(%error, "could not keep streamed bytes");
                    return fail(&fetch.shared, id, "there is not enough free disk space to stream this clip");
                }
            }
            fetch.shared.arrived.notify_waiters();
            if last_progress.elapsed() >= PROGRESS_INTERVAL {
                last_progress = Instant::now();
                fetch.progress();
            }
        }
    }
}

fn fail(shared: &Shared, id: u64, message: &str) {
    {
        let mut cache = shared.lock();
        let before = cache.runways.len();
        cache.runways.retain(|runway| runway.id != id);
        if cache.runways.len() == before {
            return; // Retired already; its failure no longer matters.
        }
        cache.failure = Some(message.into());
    }
    shared.arrived.notify_waiters();
    shared.demand.notify_waiters();
}

#[cfg(test)]
mod tests {
    use super::*;

    fn lane(id: u64, from: u64, next: u64, read: u64) -> Runway {
        Runway {
            id,
            from,
            next,
            until: next,
            read,
            used: Instant::now(),
            filling: false,
            abort: tokio::spawn(async {}).abort_handle(),
        }
    }

    #[test]
    fn the_next_fetch_is_the_first_gap() {
        let mut cache = Cache::default();
        for index in [0, 1, 2, 5] {
            cache.store.put(index, &[0; 4]).unwrap();
        }
        assert_eq!(cache.first_missing(0, 0, 8), Some(3));
        assert_eq!(cache.first_missing(4, 0, 8), Some(4));
        assert_eq!(cache.first_missing(5, 0, 8), Some(6));
        // Blocks a local copy holds are never fetched.
        assert_eq!(cache.first_missing(0, 8, 8), None);
    }

    #[tokio::test]
    async fn reads_join_the_runway_already_heading_their_way() {
        let mut cache = Cache::default();
        cache.runways.push(lane(1, 0, 10, 0));
        cache.runways.push(lane(2, 290, 300, 290));
        assert_eq!(cache.lane_for(12, None).map(|r| r.id), Some(1));
        assert_eq!(cache.lane_for(301, None).map(|r| r.id), Some(2));
        assert!(cache.lane_for(150, None).is_none());
        // A runway reaching another hands over instead of duplicating it.
        assert!(cache.lane_for(300, Some(1)).is_some());
    }

    #[tokio::test]
    async fn the_watched_position_gets_the_link_first() {
        let mut cache = Cache::default();
        // Video: the player is at 20 and the runway only at 25 (urgent).
        cache.runways.push(lane(1, 0, 25, 20));
        // Audio: far ahead of its reader (comfortable).
        cache.runways.push(lane(2, 400, 480, 410));
        assert_eq!(cache.plan(1, 0, 1000, 1000 * BLOCK_BYTES), Step::Fetch { start: 25, count: REQUEST_BLOCKS });
        assert_eq!(cache.plan(2, 0, 1000, 1000 * BLOCK_BYTES), Step::Wait, "background yields to foreground");
        // Once the video runway is comfortably ahead, the rest may fill.
        cache.runways[0].next = 25 + LANE_AHEAD_BLOCKS;
        assert!(matches!(cache.plan(2, 0, 1000, 1000 * BLOCK_BYTES), Step::Fetch { start: 480, .. }));
    }

    #[tokio::test]
    async fn an_idle_link_fetches_what_playback_needs_next() {
        let mut cache = Cache::default();
        let blocks = vec![3, 60, 20, 61, 90];
        let rank = blocks.iter().enumerate().map(|(i, b)| (*b, i)).collect();
        cache.order = Some(PlaybackOrder { blocks, rank });
        cache.store.put(60, &[0; 4]).unwrap();
        // Comfortably ahead of its reader: not urgent, so it may fill.
        cache.runways.push(lane(1, 0, 40, 0));
        cache.note_read(3);
        assert_eq!(cache.cursor, 0);
        // 3 is behind the runway's claim? No: nobody has it, so it goes first.
        assert_eq!(cache.plan(1, 0, 100, 100 * BLOCK_BYTES), Step::Fetch { start: 3, count: 8 });
        cache.store.put(3, &[0; 4]).unwrap();
        // 60 is here already, so next in playback order is 20.
        assert!(matches!(cache.plan(1, 0, 100, 100 * BLOCK_BYTES), Step::Fetch { start: 20, .. }));
        // A second idle runway skips what the first has claimed.
        cache.runways.push(lane(2, 0, 45, 0));
        assert!(matches!(cache.plan(2, 0, 100, 100 * BLOCK_BYTES), Step::Fetch { start: 61, .. }));
    }

    #[tokio::test]
    async fn reading_through_a_runway_keeps_it_urgent() {
        let mut cache = Cache::default();
        cache.runways.push(lane(1, 0, 40, 0));
        assert!(!cache.runways[0].urgent());
        cache.note_read(30);
        assert!(cache.runways[0].urgent(), "the player caught up");
    }

    #[tokio::test]
    async fn requests_stop_at_bytes_already_here_and_runways_merge() {
        let mut cache = Cache::default();
        cache.store.put(13, &[0; 4]).unwrap();
        cache.runways.push(lane(1, 0, 10, 10));
        assert_eq!(cache.plan(1, 0, 100, 100 * BLOCK_BYTES), Step::Fetch { start: 10, count: 3 });
        cache.runways.push(lane(2, 50, 52, 52));
        cache.runways[0].next = 52;
        assert_eq!(cache.plan(1, 0, 100, 100 * BLOCK_BYTES), Step::Stop, "reached runway 2");
    }
}
