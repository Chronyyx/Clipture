//! End-to-end sharing between real iroh endpoints on loopback. No relays,
//! discovery servers or user data are touched; everything lives in temp dirs.
use std::{
    path::{Path, PathBuf},
    sync::{Arc, Mutex},
    time::Duration,
};

use iroh::address_lookup::MemoryLookup;

use super::*;
use crate::sharing::{
    core::ShareCue,
    model::{DownloadPhase, FriendStatus, Presence, ShareAnswer, TransferPurpose, TransferState},
    node::is_unavailable,
    wire::{RangePurpose, Request},
};

/// Records sound cues; UI hints are ignored.
#[derive(Default)]
struct Cues(Arc<Mutex<Vec<ShareCue>>>);

impl SharingEvents for Cues {
    fn changed(&self) {}
    fn library_changed(&self) {}
    fn cue(&self, cue: ShareCue) {
        self.0.lock().unwrap().push(cue);
    }
}

pub(super) struct FakeLibrary {
    folder: PathBuf,
    published: Mutex<Vec<ClipRecord>>,
    pub(super) copies: std::sync::atomic::AtomicBool,
}

impl ClipLibrary for FakeLibrary {
    fn shared_clips_folder(&self) -> PathBuf {
        self.folder.clone()
    }

    fn publish(&self, record: ClipRecord) -> AppResult<()> {
        self.published.lock().unwrap().push(record);
        Ok(())
    }

    fn outgoing_copies_folder(&self) -> PathBuf {
        self.folder.with_file_name(".clipture-sharing")
    }

    fn write_stream_copy(&self, source: &Path, destination: &Path) -> AppResult<bool> {
        if !self.copies.load(std::sync::atomic::Ordering::Relaxed) {
            return Ok(false);
        }
        let mut bytes = std::fs::read(source).unwrap();
        bytes[8..].reverse();
        std::fs::write(destination, bytes).unwrap();
        Ok(true)
    }
}

pub(super) struct Peer {
    pub(super) service: Arc<SharingService>,
    pub(super) library: Arc<FakeLibrary>,
    cues: Arc<Mutex<Vec<ShareCue>>>,
    _directory: tempfile::TempDir,
}

impl Peer {
    fn new(lookup: &MemoryLookup) -> Self {
        let directory = tempfile::tempdir().unwrap();
        let library = Arc::new(FakeLibrary {
            folder: directory.path().join("clips").join("Shared"),
            published: Mutex::new(Vec::new()),
            copies: Default::default(),
        });
        let cues = Arc::new(Mutex::new(Vec::new()));
        let service = SharingService::new(
            directory.path().join("sharing"),
            Box::new(Cues(cues.clone())),
            library.clone(),
            tokio::runtime::Handle::current(),
            Network::Local(lookup.clone()),
        );
        Self {
            service,
            library,
            cues,
            _directory: directory,
        }
    }

    pub(super) fn code(&self) -> String {
        self.service
            .snapshot()
            .friend_code
            .expect("enabled peers have a code")
    }
}

pub(super) async fn until(label: &str, mut condition: impl FnMut() -> bool) {
    for _ in 0..200 {
        if condition() {
            return;
        }
        tokio::time::sleep(Duration::from_millis(50)).await;
    }
    panic!("timed out waiting for {label}");
}

pub(super) async fn online_pair() -> (Peer, Peer, MemoryLookup) {
    let lookup = MemoryLookup::new();
    let alice = Peer::new(&lookup);
    let bob = Peer::new(&lookup);
    alice.service.set_enabled(true).unwrap();
    bob.service.set_enabled(true).unwrap();
    until("both nodes online", || {
        alice.service.node().is_some() && bob.service.node().is_some()
    })
    .await;
    (alice, bob, lookup)
}

pub(super) async fn befriend(alice: &Peer, bob: &Peer) {
    let bob_code = bob.code();
    let alice_code = alice.code();
    assert_eq!(
        alice.service.add_friend(&bob_code, "Bob").unwrap(),
        FriendStatus::Outgoing
    );
    until("bob sees the request", || {
        bob.service.snapshot().friends.iter().any(|view| {
            view.friend.id == alice_code && view.friend.status == FriendStatus::Incoming
        })
    })
    .await;
    bob.service.accept_friend(&alice_code).unwrap();
    until("alice sees the acceptance", || {
        alice
            .service
            .core
            .read(|state| state.is_accepted(&bob_code))
    })
    .await;
}

fn write_clip(folder: &Path, name: &str, bytes: usize, seed: u8) -> PathBuf {
    std::fs::create_dir_all(folder).unwrap();
    let mut content = b"\0\0\0\x20ftypisom\0\0\x02\0".to_vec();
    content.extend((0..bytes).map(|index| (index as u8).wrapping_mul(31).wrapping_add(seed)));
    let path = folder.join(name);
    std::fs::write(&path, content).unwrap();
    path
}

fn source(path: &Path) -> ShareSource {
    ShareSource {
        path: path.to_owned(),
        title: "Ace on Mirage".into(),
        duration_seconds: 30,
        resolution: "1920x1080".into(),
        game_or_app: "cs2".into(),
        fps: 60,
        audio_tracks: vec!["Game".into(), "Mic".into()],
    }
}

pub(super) async fn share(alice: &Peer, friend: &str, path: &Path) -> String {
    let service = alice.service.clone();
    let friend = friend.to_owned();
    let source = source(path);
    tokio::task::spawn_blocking(move || service.share_clip(&friend, source))
        .await
        .unwrap()
        .unwrap()
}

/// Waits for a friend's clip to arrive, then accepts it.
pub(super) async fn receive(bob: &Peer, share_id: &str) {
    until("bob is asked about the clip", || {
        bob.service
            .snapshot()
            .inbox
            .iter()
            .any(|clip| clip.share_id == share_id && clip.answer == ShareAnswer::Pending)
    })
    .await;
    bob.service.answer_shared_clip(share_id, true).unwrap();
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn badly_laid_out_clips_are_sent_as_a_stream_copy() {
    let (alice, bob, _lookup) = online_pair().await;
    befriend(&alice, &bob).await;
    alice.library.copies.store(true, std::sync::atomic::Ordering::Relaxed);
    let clip = write_clip(&alice._directory.path().join("clips"), "ring.mp4", 300 * 1024, 4);
    let share_id = share(&alice, &bob.code(), &clip).await;
    let copy = alice.library.outgoing_copies_folder().join(format!("{share_id}.mp4"));
    assert!(copy.is_file(), "the copy is what gets served");
    receive(&bob, &share_id).await;

    // Bob keeps the copy, verified against the copy's own digest.
    bob.service.save_shared_clip(&share_id).unwrap();
    until("download ends", || {
        bob.service
            .snapshot()
            .downloads
            .iter()
            .any(|row| row.phase != DownloadPhase::Running)
    })
    .await;
    let kept = bob.library.published.lock().unwrap()[0].file_path.clone();
    assert_eq!(std::fs::read(kept).unwrap(), std::fs::read(&copy).unwrap());
    assert_ne!(std::fs::read(&copy).unwrap(), std::fs::read(&clip).unwrap());

    // Sharing the unchanged clip again reuses the copy; revoking deletes it.
    assert_eq!(share(&alice, &bob.code(), &clip).await, share_id);
    alice.service.revoke_share(&share_id).unwrap();
    // Copies younger than the in-progress window survive one sweep; age it.
    let old = std::time::SystemTime::now() - Duration::from_secs(3600);
    std::fs::File::options().write(true).open(&copy).unwrap().set_modified(old).unwrap();
    alice.service.revoke_share(&share_id).unwrap();
    assert!(!copy.exists(), "a copy nobody uses is deleted");
    assert!(clip.exists(), "the library clip is never touched");
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn sharing_again_brings_back_a_clip_the_friend_removed() {
    let (alice, bob, _lookup) = online_pair().await;
    befriend(&alice, &bob).await;
    let folder = alice._directory.path().join("clips");
    let clip = write_clip(&folder, "again.mp4", 64 * 1024, 2);
    let has = |bob: &Peer, id: &str| bob.service.snapshot().inbox.iter().any(|c| c.share_id == id);

    let first = share(&alice, &bob.code(), &clip).await;
    until("bob receives it", || has(&bob, &first)).await;
    bob.service.dismiss_shared_clip(&first).unwrap();
    assert!(!has(&bob, &first));

    // Same clip again: it comes back under the same share.
    assert_eq!(share(&alice, &bob.code(), &clip).await, first);
    until("bob receives it again", || has(&bob, &first)).await;

    // Edited since: the stale share is replaced by a new one.
    write_clip(&folder, "again.mp4", 64 * 1024, 3);
    let second = share(&alice, &bob.code(), &clip).await;
    assert_ne!(second, first);
    until("bob receives the edited clip", || has(&bob, &second)).await;
    assert_eq!(alice.service.snapshot().outbox.len(), 1);
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn friends_stream_and_keep_a_verified_clip() {
    let (alice, bob, _lookup) = online_pair().await;
    befriend(&alice, &bob).await;
    let bob_code = bob.code();

    let clip = write_clip(
        &alice._directory.path().join("clips"),
        "ace.mp4",
        3 * 1024 * 1024 + 777,
        1,
    );
    let original = std::fs::read(&clip).unwrap();
    let share_id = share(&alice, &bob_code, &clip).await;
    receive(&bob, &share_id).await;

    // Streaming: bounded ranges served straight from memory.
    let service = bob.service.clone();
    let id = share_id.clone();
    let (head, tail) = tokio::task::spawn_blocking(move || {
        let stream = service.open_stream("main", &id).unwrap();
        let head = service
            .read_video(&stream, "main", Some("bytes=0-"))
            .unwrap();
        let tail = service
            .read_video(&stream, "main", Some("bytes=3145000-"))
            .unwrap();
        assert!(service.read_video(&stream, "another-window", None).is_err());
        (head, tail)
    })
    .await
    .unwrap();
    // Whatever has arrived in order, at least the first block.
    let served = head.range.end_inclusive as usize + 1;
    assert!(served >= 1024 * 1024 && served.is_multiple_of(1024 * 1024) || served == original.len());
    assert_eq!(head.bytes, original[..served]);
    assert_eq!(tail.bytes, original[3_145_000..=tail.range.end_inclusive as usize]);
    // The runway fetches in order: the streamed bytes become one range.
    until("the whole clip has streamed", || {
        bob.service.snapshot().inbox[0].streamed == vec![[0, original.len() as u64]]
    })
    .await;
    // Fully arrived: a temporary copy lets every audio track be mixed.
    until("every audio track can play", || {
        bob.service.snapshot().inbox[0].all_audio_ready
    })
    .await;
    assert_eq!(bob.service.snapshot().inbox[0].audio_tracks, ["Game", "Mic"]);
    assert!(
        !bob.library.folder.exists(),
        "streaming must not write files"
    );

    // Keeping: a verified copy joins the library.
    bob.service.save_shared_clip(&share_id).unwrap();
    until("download finishes", || {
        bob.service
            .snapshot()
            .downloads
            .iter()
            .any(|row| row.share_id == share_id && row.phase != DownloadPhase::Running)
    })
    .await;
    let download = bob.service.snapshot().downloads.remove(0);
    assert_eq!(
        download.phase,
        DownloadPhase::Done,
        "{:?}",
        download.message
    );
    let published = bob.library.published.lock().unwrap().clone();
    assert_eq!(published.len(), 1);
    assert_eq!(published[0].audio_tracks, vec!["Game", "Mic"]);
    assert_eq!(std::fs::read(&published[0].file_path).unwrap(), original);
    assert!(bob.service.snapshot().inbox[0].saved);
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn strangers_and_revoked_shares_get_nothing() {
    let (alice, bob, lookup) = online_pair().await;
    befriend(&alice, &bob).await;
    let clip = write_clip(
        &alice._directory.path().join("clips"),
        "clip.mp4",
        64 * 1024,
        2,
    );
    let share_id = share(&alice, &bob.code(), &clip).await;

    // A third node that knows the share id still cannot read it.
    let mallory = Peer::new(&lookup);
    mallory.service.set_enabled(true).unwrap();
    until("mallory online", || mallory.service.node().is_some()).await;
    let alice_id = parse_friend_code(&alice.code()).unwrap();
    let node = mallory.service.node().unwrap();
    assert!(node.open_range(alice_id, &share_id, 0, 16, RangePurpose::Watch).await.is_err());
    assert!(node
        .notify(
            alice_id,
            &Request::Offer {
                offer: alice
                    .service
                    .core
                    .read(|state| state.outbox[0].offer.clone()),
            }
        )
        .await
        .is_err());
    assert!(alice.service.snapshot().inbox.is_empty());

    // Revoking stops the friend too.
    alice.service.revoke_share(&share_id).unwrap();
    until("bob has the offer", || {
        !bob.service.snapshot().inbox.is_empty()
    })
    .await;
    let bob_node = bob.service.node().unwrap();
    assert!(bob_node
        .open_range(alice_id, &share_id, 0, 16, RangePurpose::Watch)
        .await
        .is_err());
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_tampered_clip_is_discarded() {
    let (alice, bob, _lookup) = online_pair().await;
    befriend(&alice, &bob).await;
    let folder = alice._directory.path().join("clips");
    let clip = write_clip(&folder, "clip.mp4", 256 * 1024, 3);
    let share_id = share(&alice, &bob.code(), &clip).await;
    receive(&bob, &share_id).await;
    // Same size, different bytes: only the digest can catch this.
    write_clip(&folder, "clip.mp4", 256 * 1024, 4);

    bob.service.save_shared_clip(&share_id).unwrap();
    until("download ends", || {
        bob.service
            .snapshot()
            .downloads
            .iter()
            .any(|row| row.phase != DownloadPhase::Running)
    })
    .await;
    let download = bob.service.snapshot().downloads.remove(0);
    assert_eq!(download.phase, DownloadPhase::Failed);
    assert!(download.message.unwrap().contains("integrity"));
    assert!(bob.library.published.lock().unwrap().is_empty());
    let leftovers = std::fs::read_dir(&bob.library.folder)
        .map(|entries| entries.count())
        .unwrap_or(0);
    assert_eq!(leftovers, 0, "partial files must be removed");
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn an_interrupted_download_resumes_from_its_partial_file() {
    let (alice, bob, _lookup) = online_pair().await;
    befriend(&alice, &bob).await;
    let clip = write_clip(&alice._directory.path().join("clips"), "clip.mp4", 512 * 1024, 7);
    let share_id = share(&alice, &bob.code(), &clip).await;
    receive(&bob, &share_id).await;
    // What an earlier, interrupted attempt left behind.
    let original = std::fs::read(&clip).unwrap();
    std::fs::create_dir_all(&bob.library.folder).unwrap();
    let partial = crate::sharing::download::partial_path(&bob.library.folder, &share_id);
    std::fs::write(&partial, &original[..200 * 1024]).unwrap();

    bob.service.save_shared_clip(&share_id).unwrap();
    until("download ends", || {
        bob.service
            .snapshot()
            .downloads
            .iter()
            .any(|row| row.phase != DownloadPhase::Running)
    })
    .await;
    let download = bob.service.snapshot().downloads.remove(0);
    assert_eq!(download.phase, DownloadPhase::Done, "{:?}", download.message);
    let saved = bob.library.published.lock().unwrap()[0].file_path.clone();
    assert_eq!(std::fs::read(saved).unwrap(), original);
    assert!(!partial.exists(), "the partial becomes the clip");
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn friends_answer_and_the_sender_follows_the_transfer() {
    let (alice, bob, _lookup) = online_pair().await;
    befriend(&alice, &bob).await;
    let clip = write_clip(&alice._directory.path().join("clips"), "ask.mp4", 700 * 1024, 9);
    let size = std::fs::metadata(&clip).unwrap().len();
    let outbox = |alice: &Peer| alice.service.snapshot().outbox.remove(0);

    // Nothing streams before bob says yes.
    let share_id = share(&alice, &bob.code(), &clip).await;
    until("bob is asked", || {
        bob.service.snapshot().inbox.first().is_some_and(|clip| clip.answer == ShareAnswer::Pending)
    })
    .await;
    assert!(bob.cues.lock().unwrap().contains(&ShareCue::Incoming));
    assert!(bob.service.open_stream("main", &share_id).is_err());
    assert!(bob.service.save_shared_clip(&share_id).is_err());
    until("alice sees it delivered", || outbox(&alice).delivered).await;
    assert_eq!(outbox(&alice).answer, ShareAnswer::Pending);

    // Declining tells alice, and she stops serving it.
    bob.service.answer_shared_clip(&share_id, false).unwrap();
    assert!(bob.service.snapshot().inbox.is_empty());
    until("alice hears no", || outbox(&alice).answer == ShareAnswer::Declined).await;
    assert!(alice.cues.lock().unwrap().contains(&ShareCue::Declined));
    let alice_id = parse_friend_code(&alice.code()).unwrap();
    let bob_node = bob.service.node().unwrap();
    assert!(bob_node.open_range(alice_id, &share_id, 0, 16, RangePurpose::Watch).await.is_err());

    // Asked again, bob accepts and keeps it; alice watches it arrive.
    assert_eq!(share(&alice, &bob.code(), &clip).await, share_id);
    receive(&bob, &share_id).await;
    until("alice hears yes", || outbox(&alice).answer == ShareAnswer::Accepted).await;
    bob.service.save_shared_clip(&share_id).unwrap();
    until("alice has sent every byte", || {
        outbox(&alice).transfer.is_some_and(|transfer| transfer.state == TransferState::Complete)
    })
    .await;
    let transfer = outbox(&alice).transfer.unwrap();
    assert_eq!((transfer.sent_bytes, transfer.total_bytes), (size, size));
    assert_eq!(transfer.purpose, TransferPurpose::Keep);
    assert!(alice.service.core.read(|state| state.outbox[0].received_whole), "kept across restarts");

    // Bob's verified copy closes the share: he has it, so it is never
    // served again, by stream or download.
    until("alice hears bob kept it", || outbox(&alice).saved).await;
    assert!(alice.cues.lock().unwrap().contains(&ShareCue::Complete));
    for purpose in [RangePurpose::Watch, RangePurpose::Keep] {
        assert!(bob_node.open_range(alice_id, &share_id, 0, 16, purpose).await.is_err());
    }
    // Bob still plays his own copy, without asking alice.
    assert!(bob.service.snapshot().inbox[0].saved);
}

/// Turns sharing off and waits out the goodbye budget, after which the
/// endpoint is closed and the peer is truly unreachable.
async fn go_offline(peer: &Peer) {
    peer.service.set_enabled(false).unwrap();
    until("node detached", || peer.service.node().is_none()).await;
    tokio::time::sleep(Duration::from_millis(2500)).await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn the_sender_stops_serving_when_the_window_closes() {
    let (alice, bob, _lookup) = online_pair().await;
    befriend(&alice, &bob).await;
    alice.service.set_share_window(1_500);
    let clip = write_clip(&alice._directory.path().join("clips"), "brief.mp4", 64 * 1024, 6);
    let share_id = share(&alice, &bob.code(), &clip).await;
    receive(&bob, &share_id).await;
    until("alice hears yes", || {
        alice.service.snapshot().outbox[0].available_until_ms.is_some()
    })
    .await;
    let alice_id = parse_friend_code(&alice.code()).unwrap();
    let bob_node = bob.service.node().unwrap();
    assert!(bob_node.open_range(alice_id, &share_id, 0, 16, RangePurpose::Watch).await.is_ok());

    tokio::time::sleep(Duration::from_millis(1_800)).await;
    for purpose in [RangePurpose::Watch, RangePurpose::Keep] {
        let refused = bob_node.open_range(alice_id, &share_id, 0, 16, purpose).await;
        assert!(refused.is_err_and(|error| is_unavailable(&error)), "closed for {purpose:?}");
    }
}

fn knows(viewer: &Peer, friend: &str) -> bool {
    viewer.service.core.read(|state| state.friend(friend).is_some())
}

fn owes_goodbye(viewer: &Peer, friend: &str) -> bool {
    viewer.service.core.read(|state| state.goodbyes.iter().any(|id| id == friend))
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_friend_removed_while_offline_learns_it_when_back() {
    let (alice, bob, _lookup) = online_pair().await;
    befriend(&alice, &bob).await;
    let (alice_code, bob_code) = (alice.code(), bob.code());

    // Bob is away when Alice removes him: the goodbye cannot reach him.
    go_offline(&bob).await;
    alice.service.remove_friend(&bob_code).unwrap();
    assert!(!knows(&alice, &bob_code));
    tokio::time::sleep(Duration::from_millis(500)).await;
    assert!(owes_goodbye(&alice, &bob_code), "kept until bob hears it");
    assert!(knows(&bob, &alice_code), "bob has not heard yet");

    // Back online, Bob announces himself; Alice says they are not friends.
    bob.service.set_enabled(true).unwrap();
    until("bob drops alice", || !knows(&bob, &alice_code)).await;
    until("alice's goodbye is settled", || !owes_goodbye(&alice, &bob_code)).await;
    assert!(!knows(&alice, &bob_code), "nobody is re-added: {:?}", alice.service.core.read(|state| state.friends.clone()));
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn an_owed_goodbye_goes_out_when_the_remover_returns() {
    let (alice, bob, _lookup) = online_pair().await;
    befriend(&alice, &bob).await;
    let (alice_code, bob_code) = (alice.code(), bob.code());

    go_offline(&bob).await;
    alice.service.remove_friend(&bob_code).unwrap();
    go_offline(&alice).await;

    // Bob returns while Alice is away: nobody can tell him yet.
    bob.service.set_enabled(true).unwrap();
    until("bob online", || bob.service.node().is_some()).await;
    tokio::time::sleep(Duration::from_millis(500)).await;
    assert!(knows(&bob, &alice_code));

    // Alice returns and delivers what she owes.
    alice.service.set_enabled(true).unwrap();
    until("bob drops alice", || !knows(&bob, &alice_code)).await;
    until("alice's goodbye is settled", || !owes_goodbye(&alice, &bob_code)).await;
}

fn presence_of(viewer: &Peer, friend: &Peer) -> Presence {
    let code = friend.code();
    viewer
        .service
        .snapshot()
        .friends
        .into_iter()
        .find(|view| view.friend.id == code)
        .map(|view| view.presence)
        .unwrap_or(Presence::Unknown)
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn friends_see_each_other_come_and_go() {
    let (alice, bob, _lookup) = online_pair().await;
    befriend(&alice, &bob).await;
    until("both see each other online", || {
        presence_of(&alice, &bob) == Presence::Online
            && presence_of(&bob, &alice) == Presence::Online
    })
    .await;

    // A clean exit says goodbye instead of waiting for the heartbeat.
    let alice_code = alice.code();
    alice.service.set_enabled(false).unwrap();
    until("bob sees alice leave", || {
        bob.service
            .snapshot()
            .friends
            .iter()
            .any(|view| view.friend.id == alice_code && view.presence == Presence::Offline)
    })
    .await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn appearing_offline_refuses_everything_until_visible() {
    let (alice, bob, _lookup) = online_pair().await;
    befriend(&alice, &bob).await;
    until("alice sees bob", || {
        presence_of(&alice, &bob) == Presence::Online
    })
    .await;

    bob.service.set_appear_offline(true).unwrap();
    until("bob looks offline to alice", || {
        presence_of(&alice, &bob) == Presence::Offline
    })
    .await;
    until("bob restarted hidden", || bob.service.node().is_some()).await;
    assert_eq!(presence_of(&bob, &alice), Presence::Unknown);

    // A clip shared while bob is hidden is refused and stays queued.
    let clip = write_clip(
        &alice._directory.path().join("clips"),
        "queued.mp4",
        32 * 1024,
        5,
    );
    let share_id = share(&alice, &bob.code(), &clip).await;
    tokio::time::sleep(Duration::from_millis(1500)).await;
    assert!(
        bob.service.snapshot().inbox.is_empty(),
        "nothing is received while hidden"
    );
    assert!(!alice.service.snapshot().outbox[0].delivered);
    let bob_id = parse_friend_code(&bob.code()).unwrap();
    let alice_node = alice.service.node().unwrap();
    // Refused or unreachable are both fine; succeeding is not.
    let direct = tokio::time::timeout(
        Duration::from_secs(3),
        alice_node.notify(
            bob_id,
            &Request::Hello {
                name: "Alice".into(),
            },
        ),
    )
    .await;
    assert!(
        !matches!(direct, Ok(Ok(()))),
        "a hidden peer must not answer"
    );

    // Becoming visible announces bob, and alice delivers at once.
    bob.service.set_appear_offline(false).unwrap();
    until("bob receives the queued clip", || {
        bob.service
            .snapshot()
            .inbox
            .iter()
            .any(|clip| clip.share_id == share_id)
    })
    .await;
    // Alice records delivery once bob's answer arrives, just after bob has it.
    until("alice marks the clip delivered", || {
        alice.service.snapshot().outbox[0].delivered
    })
    .await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn an_invite_link_waits_for_confirmation_then_befriends() {
    let lookup = MemoryLookup::new();
    let alice = Peer::new(&lookup);
    let bob = Peer::new(&lookup);
    alice.service.set_enabled(true).unwrap();
    until("alice online", || alice.service.node().is_some()).await;
    let link = alice
        .service
        .snapshot()
        .invite_link
        .expect("enabled peers have a link");

    // Bob has never turned sharing on. Opening the link changes nothing yet.
    let invite = parse_invite(&link).unwrap();
    bob.service.receive_invite(invite);
    let pending = bob.service.snapshot().pending_invite.expect("invite waits");
    assert_eq!(pending.code, alice.code());
    assert!(bob.service.snapshot().friends.is_empty());
    assert!(!bob.service.snapshot().enabled);

    // Confirming turns sharing on and sends the request.
    bob.service.accept_invite().unwrap();
    assert!(bob.service.snapshot().pending_invite.is_none());
    until("alice sees bob's request", || {
        alice
            .service
            .snapshot()
            .friends
            .iter()
            .any(|view| view.friend.status == FriendStatus::Incoming)
    })
    .await;

    // A link to yourself is ignored.
    let own = parse_invite(&alice.service.snapshot().invite_link.unwrap()).unwrap();
    alice.service.receive_invite(own);
    assert!(alice.service.snapshot().pending_invite.is_none());
}
