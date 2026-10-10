use std::{
    cell::RefCell,
    io,
    path::{Path, PathBuf},
    rc::Rc,
};

use super::super::{layout_probe::read_layout, mp4_box::boxed};
use super::{apply, disk::Disk, finish_journal, journal, plan_for, ClipTrimmer, TrimError};

pub(in crate::media) const MOVIE_SCALE: u32 = 1000;
pub(in crate::media) const VIDEO_SCALE: u32 = 3000;
pub(in crate::media) const FRAME: u32 = 100;
pub(in crate::media) const FRAMES: usize = 120;
pub(in crate::media) const KEYFRAME_EVERY: usize = 30;
pub(in crate::media) const AUDIO_SCALE: u32 = 48_000;
pub(in crate::media) const PACKET: u32 = 1024;
pub(in crate::media) const PACKETS: usize = 188;

pub(in crate::media) fn words(values: &[u32]) -> Vec<u8> {
    values.iter().flat_map(|value| value.to_be_bytes()).collect()
}

pub(in crate::media) fn full_box(kind: &[u8; 4], version: u8, body: &[u8]) -> Vec<u8> {
    boxed(kind, &[&[version, 0, 0, 0][..], body].concat())
}

/// Bytes unique to one sample, so any misplaced byte shows.
pub(in crate::media) fn sample(track: u8, index: usize, len: usize) -> Vec<u8> {
    (0..len).map(|at| (track as usize * 97 + index * 13 + at * 7) as u8 | 1).collect()
}

pub(in crate::media) struct Fixture {
    pub(in crate::media) path: PathBuf,
    pub(in crate::media) original: Vec<u8>,
    pub(in crate::media) video: Vec<Vec<u8>>,
    pub(in crate::media) audio: Vec<Vec<u8>>,
}

/// A four-second clip laid out like a Save in Place recording: interleaved
/// in time, but stored as a wrapped ring (later footage first) with zero
/// padding between packets. Video skips its first frame by edit list and
/// has a keyframe every second; audio starts 16 ms late.
pub(in crate::media) fn fixture(dir: &Path) -> Fixture {
    let video: Vec<Vec<u8>> = (0..FRAMES)
        .map(|index| sample(1, index, if index % KEYFRAME_EVERY == 0 { 6000 } else { 1500 + (index % 7) * 100 }))
        .collect();
    let audio: Vec<Vec<u8>> = (0..PACKETS).map(|index| sample(2, index, 300 + (index % 5) * 10)).collect();
    // Time order: each frame, then the audio playing during it.
    let mut order: Vec<(u8, usize)> = Vec::new();
    let mut next_packet = 0;
    for frame in 0..FRAMES {
        order.push((1, frame));
        let frame_end = (frame as u64 + 1) * u64::from(FRAME) * u64::from(AUDIO_SCALE) / u64::from(VIDEO_SCALE);
        while next_packet < PACKETS && (next_packet as u64) * u64::from(PACKET) < frame_end {
            order.push((2, next_packet));
            next_packet += 1;
        }
    }
    order.extend((next_packet..PACKETS).map(|packet| (2, packet)));
    let split = order.len() * 3 / 5;
    let ring: Vec<_> = order[split..].iter().chain(&order[..split]).copied().collect();

    let ftyp = boxed(b"ftyp", b"isom\0\0\x02\0isomavc1");
    let free = boxed(b"free", b"");
    let mdat_at = (ftyp.len() + free.len()) as u64;
    let mut payload = Vec::new();
    let mut video_at = vec![0_u32; FRAMES];
    let mut audio_at = vec![0_u32; PACKETS];
    for (position, (track, index)) in ring.iter().enumerate() {
        if position % 7 == 3 {
            payload.extend(vec![0_u8; 3000]);
        }
        let at = (mdat_at + 8 + payload.len() as u64) as u32;
        if *track == 1 {
            video_at[*index] = at;
            payload.extend(&video[*index]);
        } else {
            audio_at[*index] = at;
            payload.extend(&audio[*index]);
        }
    }
    let mut file = [ftyp, free, boxed(b"mdat", &payload)].concat();
    file.extend(movie(&video, &video_at, &audio, &audio_at));
    let path = dir.join("clip.mp4");
    std::fs::write(&path, &file).unwrap();
    Fixture { path, original: file, video, audio }
}

fn movie(video: &[Vec<u8>], video_at: &[u32], audio: &[Vec<u8>], audio_at: &[u32]) -> Vec<u8> {
    let mut mvhd = words(&[0, 0, MOVIE_SCALE, 4000]);
    mvhd.resize(96, 0);
    let video_track = track(1, b"vide", VIDEO_SCALE, (FRAMES as u32) * FRAME, video, video_at, FRAME, Some((0..FRAMES).step_by(KEYFRAME_EVERY).map(|i| i as u32 + 1).collect()), &[(3967, 100)]);
    let audio_track = track(2, b"soun", AUDIO_SCALE, PACKETS as u32 * PACKET, audio, audio_at, PACKET, None, &[(16, -1), (3967, 0), (17, -1)]);
    boxed(b"moov", &[full_box(b"mvhd", 0, &mvhd), video_track, audio_track].concat())
}

#[allow(clippy::too_many_arguments)]
pub(in crate::media) fn track(id: u32, handler: &[u8; 4], scale: u32, duration: u32, samples: &[Vec<u8>], offsets: &[u32], delta: u32, sync: Option<Vec<u32>>, edits: &[(u32, i32)]) -> Vec<u8> {
    let mut tkhd = words(&[0, 0, id, 0, 4000]);
    tkhd.resize(80, 0);
    let mut elst = words(&[edits.len() as u32]);
    for (span, media_time) in edits {
        elst.extend(words(&[*span, *media_time as u32, 0x0001_0000]));
    }
    let count = samples.len() as u32;
    let mut stbl = vec![
        full_box(b"stsd", 0, &[words(&[1]), boxed(b"test", &[0; 8])].concat()),
        full_box(b"stts", 0, &words(&[1, count, delta])),
    ];
    if let Some(sync) = sync {
        stbl.push(full_box(b"stss", 0, &[words(&[sync.len() as u32]), words(&sync)].concat()));
    }
    stbl.push(full_box(b"stsc", 0, &words(&[1, 1, 1, 1])));
    stbl.push(full_box(b"stsz", 0, &[words(&[0, count]), samples.iter().flat_map(|bytes| (bytes.len() as u32).to_be_bytes()).collect()].concat()));
    stbl.push(full_box(b"stco", 0, &[words(&[count]), words(offsets)].concat()));
    let mut hdlr = words(&[0]);
    hdlr.extend_from_slice(handler);
    hdlr.extend([0; 13]);
    let mdia = [
        full_box(b"mdhd", 0, &[words(&[0, 0, scale, duration]), vec![0; 4]].concat()),
        full_box(b"hdlr", 0, &hdlr),
        boxed(b"minf", &boxed(b"stbl", &stbl.concat())),
    ]
    .concat();
    boxed(b"trak", &[full_box(b"tkhd", 0, &tkhd), boxed(b"edts", &full_box(b"elst", 0, &elst)), boxed(b"mdia", &mdia)].concat())
}

/// Each track's edit list as (duration, media time) pairs.
fn edits(file: &[u8]) -> Vec<Vec<(u64, i64)>> {
    let mut found = Vec::new();
    let mut at = 0;
    while let Some(position) = file[at..].windows(4).position(|w| w == b"elst") {
        let start = at + position + 4;
        let count = u32::from_be_bytes(file[start + 4..start + 8].try_into().unwrap()) as usize;
        let wide = file[start] == 1;
        let entries = (0..count)
            .map(|index| {
                if wide {
                    let entry = start + 8 + index * 20;
                    (u64::from_be_bytes(file[entry..entry + 8].try_into().unwrap()), i64::from_be_bytes(file[entry + 8..entry + 16].try_into().unwrap()))
                } else {
                    let entry = start + 8 + index * 12;
                    (u64::from(u32::from_be_bytes(file[entry..entry + 4].try_into().unwrap())), i64::from(i32::from_be_bytes(file[entry + 4..entry + 8].try_into().unwrap())))
                }
            })
            .collect();
        found.push(entries);
        at = start;
    }
    found
}

fn journals(dir: &Path) -> PathBuf {
    dir.join("journals")
}

#[test]
fn a_trim_keeps_exactly_the_frames_it_needs_and_deletes_the_rest() {
    let dir = tempfile::tempdir().unwrap();
    let clip = fixture(dir.path());
    let trimmer = ClipTrimmer::new(journals(dir.path()));
    let done = trimmer.trim(&clip.path, 1.0, 2.5).unwrap();
    assert!((done.duration_seconds - 1.5).abs() < 1e-9);
    let bytes = std::fs::read(&clip.path).unwrap();
    assert_eq!(bytes.len() as u64, done.bytes_after);
    assert!(done.bytes_after < done.bytes_before / 2, "{} -> {}", done.bytes_before, done.bytes_after);

    let tracks = read_layout(&clip.path).unwrap();
    // Video from the keyframe at 1 s (frame 30) to the frame shown at 2.5 s.
    let video: Vec<_> = (30..=75).map(|index| &clip.video[index]).collect();
    // Audio from one packet before the cut (priming) to the end.
    let audio: Vec<_> = (45..=116).map(|index| &clip.audio[index]).collect();
    for (track, expected) in tracks.iter().zip([video, audio]) {
        assert_eq!(track.offsets.len(), expected.len());
        for ((offset, size), sample) in track.offsets.iter().zip(&track.sizes).zip(expected) {
            assert_eq!(&bytes[*offset as usize..(*offset + u64::from(*size)) as usize], sample.as_slice());
        }
    }
    // Packed: the media is exactly the kept samples, then the index.
    let kept: u64 = tracks.iter().flat_map(|track| &track.sizes).map(|size| u64::from(*size)).sum();
    let media_end = tracks.iter().flat_map(|track| track.offsets.iter().zip(&track.sizes)).map(|(o, s)| o + u64::from(*s)).max().unwrap();
    let mdat_at = 24 + 8;
    assert_eq!(media_end, mdat_at + 8 + kept);
    assert_eq!(&bytes[mdat_at as usize + 4..mdat_at as usize + 8], b"mdat");
    assert_eq!(u32::from_be_bytes(bytes[mdat_at as usize..mdat_at as usize + 4].try_into().unwrap()) as u64, 8 + kept);
    assert_eq!(&bytes[media_end as usize + 4..media_end as usize + 8], b"moov");
    // Presentation starts exactly at the cut: frame 31 (1 s in, after the
    // one skipped frame) is 100 video ticks past keyframe 30; audio starts
    // 47232 ticks in, 1152 past the priming packet.
    assert_eq!(edits(&bytes), vec![vec![(1500, 100)], vec![(1500, 1152)]]);
    assert!(std::fs::read_dir(journals(dir.path())).unwrap().next().is_none(), "the journal is removed");
}

#[test]
fn trims_that_keep_everything_or_almost_nothing_are_refused() {
    let dir = tempfile::tempdir().unwrap();
    let clip = fixture(dir.path());
    let trimmer = ClipTrimmer::new(journals(dir.path()));
    assert!(matches!(trimmer.trim(&clip.path, 0.0, 4.0), Err(TrimError::Range(_))));
    assert!(matches!(trimmer.trim(&clip.path, 1.0, 1.05), Err(TrimError::Range(_))));
    assert!(matches!(trimmer.trim(&clip.path, 3.0, 2.0), Err(TrimError::Range(_))));
    assert_eq!(std::fs::read(&clip.path).unwrap(), clip.original);
}

#[test]
fn index_first_layouts_are_refused_untouched() {
    let dir = tempfile::tempdir().unwrap();
    let clip = fixture(dir.path());
    // Move the index in front of the media; offsets no longer matter, the
    // layout check refuses it first.
    let moov_at = clip.original.windows(4).rposition(|w| w == b"moov").unwrap() - 4;
    let reordered = [&clip.original[..32], &clip.original[moov_at..], &clip.original[32..moov_at]].concat();
    std::fs::write(&clip.path, &reordered).unwrap();
    let trimmer = ClipTrimmer::new(journals(dir.path()));
    assert!(matches!(trimmer.trim(&clip.path, 1.0, 2.0), Err(TrimError::Unsupported(_))));
    assert_eq!(std::fs::read(&clip.path).unwrap(), reordered);
}

#[test]
fn a_clip_open_elsewhere_is_not_opened_for_trimming() {
    let dir = tempfile::tempdir().unwrap();
    let clip = fixture(dir.path());
    let held = std::fs::File::open(&clip.path).unwrap();
    let error = super::disk::open_exclusive(&clip.path, false).unwrap_err();
    assert!(super::disk::is_in_use(&error), "{error}");
    drop(held);
    assert!(super::disk::open_exclusive(&clip.path, false).is_ok());
}

// Power loss: writes not yet flushed may all be lost, all land, or land in part.

#[derive(Clone, Copy, Debug)]
enum Landing {
    Nothing,
    Everything,
    EveryOther,
}

struct Power {
    budget: usize,
    used: usize,
    out: bool,
}

enum Pending {
    Write(u64, Vec<u8>),
    SetLen(u64),
}

/// A disk whose writes reach the file only when flushed, and that fails
/// every operation once the shared budget runs out.
struct Fragile {
    file: std::fs::File,
    pending: Vec<Pending>,
    power: Rc<RefCell<Power>>,
}

impl Fragile {
    fn spend(&mut self) -> io::Result<()> {
        let mut power = self.power.borrow_mut();
        if power.out || power.used >= power.budget {
            power.out = true;
            return Err(io::Error::other("power lost"));
        }
        power.used += 1;
        Ok(())
    }

    fn apply(file: &mut std::fs::File, op: &Pending) -> io::Result<()> {
        match op {
            Pending::Write(offset, bytes) => Disk::write_at(file, *offset, bytes),
            Pending::SetLen(len) => file.set_len(*len),
        }
    }

    /// What reaches the disk when the power goes.
    fn lose_power(mut self, landing: Landing) {
        let pending = std::mem::take(&mut self.pending);
        for (index, op) in pending.iter().enumerate() {
            let lands = match landing {
                Landing::Nothing => false,
                Landing::Everything => true,
                Landing::EveryOther => index % 2 == 0,
            };
            if lands {
                Self::apply(&mut self.file, op).unwrap();
            }
        }
    }
}

impl Disk for Fragile {
    fn read_at(&mut self, offset: u64, buffer: &mut [u8]) -> io::Result<()> {
        let on_disk = self.file.metadata()?.len();
        buffer.fill(0);
        if offset < on_disk {
            let readable = ((on_disk - offset) as usize).min(buffer.len());
            Disk::read_at(&mut self.file, offset, &mut buffer[..readable])?;
        }
        let end = offset + buffer.len() as u64;
        for op in &self.pending {
            match op {
                Pending::Write(at, bytes) => {
                    let (from, to) = ((*at).max(offset), (at + bytes.len() as u64).min(end));
                    if from < to {
                        buffer[(from - offset) as usize..(to - offset) as usize].copy_from_slice(&bytes[(from - at) as usize..(to - at) as usize]);
                    }
                }
                Pending::SetLen(len) if *len < end => {
                    let from = (*len).max(offset);
                    buffer[(from - offset) as usize..].fill(0);
                }
                Pending::SetLen(_) => {}
            }
        }
        Ok(())
    }

    fn write_at(&mut self, offset: u64, bytes: &[u8]) -> io::Result<()> {
        self.spend()?;
        self.pending.push(Pending::Write(offset, bytes.to_vec()));
        Ok(())
    }

    fn sync(&mut self) -> io::Result<()> {
        self.spend()?;
        // Durability is what this simulates; the real flush is not needed.
        for op in std::mem::take(&mut self.pending) {
            Self::apply(&mut self.file, &op)?;
        }
        Ok(())
    }

    fn set_len(&mut self, len: u64) -> io::Result<()> {
        self.spend()?;
        self.pending.push(Pending::SetLen(len));
        Ok(())
    }

    fn len(&mut self) -> io::Result<u64> {
        let mut len = self.file.metadata()?.len();
        for op in &self.pending {
            match op {
                Pending::Write(at, bytes) => len = len.max(at + bytes.len() as u64),
                Pending::SetLen(new) => len = *new,
            }
        }
        Ok(len)
    }
}

/// Most of the fixture goes.
const HEAVY: (f64, f64) = (1.0, 2.5);
/// Little goes: most kept bytes move a short way.
const LIGHT: (f64, f64) = (0.3, 3.7);

fn planned(clip: &Fixture, range: (f64, f64)) -> super::layout::Plan {
    let mut clip_file = super::disk::open_exclusive(&clip.path, false).unwrap();
    let len = Disk::len(&mut clip_file).unwrap();
    let mut plan = plan_for(&mut clip_file, &clip.path, len, range.0, range.1).unwrap();
    // Tiny blocks: moves span many blocks, and some overlap their own source.
    plan.block_bytes = 4096;
    plan
}

/// Runs a trim until the power budget is spent.
fn trim_until_power_loss(dir: &Path, clip: &Fixture, range: (f64, f64), budget: usize, landing: Landing) -> (PathBuf, bool) {
    std::fs::write(&clip.path, &clip.original).unwrap();
    let journal_dir = journals(dir);
    let _ = std::fs::remove_dir_all(&journal_dir);
    std::fs::create_dir_all(&journal_dir).unwrap();
    let journal_path = journal_dir.join("crash.trim");
    let plan = planned(clip, range);
    let clip_file = super::disk::open_exclusive(&clip.path, false).unwrap();
    let power = Rc::new(RefCell::new(Power { budget, used: 0, out: false }));
    let mut clip_disk = Fragile { file: clip_file, pending: Vec::new(), power: power.clone() };
    let mut journal_disk = Fragile {
        file: super::disk::open_exclusive(&journal_path, true).unwrap(),
        pending: Vec::new(),
        power: power.clone(),
    };
    let finished = journal::commit(&mut journal_disk, &plan)
        .and_then(|(plan_len, progress)| apply::run(&mut clip_disk, &mut journal_disk, &plan, plan_len, progress))
        .is_ok();
    clip_disk.lose_power(landing);
    journal_disk.lose_power(landing);
    (journal_path, finished)
}

/// Times a sample is stored before the one recorded ahead of it: the
/// recorder's arena wraps once per lap, nothing else goes backward.
fn backward_steps(path: &Path) -> usize {
    read_layout(path)
        .unwrap()
        .iter()
        .map(|track| track.offsets.windows(2).filter(|pair| pair[1] < pair[0]).count())
        .sum()
}

#[test]
fn trims_pack_the_media_and_keep_the_recorded_order() {
    let dir = tempfile::tempdir().unwrap();
    let clip = fixture(dir.path());
    let wraps = backward_steps(&clip.path);
    assert!(wraps >= 1, "the fixture's ring has wrapped");
    for range in [HEAVY, LIGHT] {
        let (journal_path, finished) = trim_until_power_loss(dir.path(), &clip, range, usize::MAX, Landing::Nothing);
        assert!(finished);
        std::fs::remove_file(journal_path).unwrap();
        let bytes = std::fs::read(&clip.path).unwrap();
        assert!(bytes.len() < clip.original.len(), "{range:?}");
        // Packed: media then index, and nothing after.
        let tracks = read_layout(&clip.path).unwrap();
        let kept: u64 = tracks.iter().flat_map(|track| &track.sizes).map(|size| u64::from(*size)).sum();
        assert_eq!(u32::from_be_bytes(bytes[32..36].try_into().unwrap()) as u64, 8 + kept);
        assert_eq!(&bytes[(40 + kept) as usize + 4..(40 + kept) as usize + 8], b"moov");
        assert!(backward_steps(&clip.path) <= wraps, "{range:?}: no new disorder");
    }
}

#[test]
fn power_loss_at_any_point_leaves_the_original_or_the_finished_trim() {
    let dir = tempfile::tempdir().unwrap();
    let clip = fixture(dir.path());
    let mut outcomes = [0_usize; 2];
    // HEAVY at every crash point; LIGHT, which moves far more, sampled.
    for (range, stride) in [(HEAVY, 1), (LIGHT, 7)] {
        // The finished result, from an uninterrupted run with the same blocks.
        let (journal_path, finished) = trim_until_power_loss(dir.path(), &clip, range, usize::MAX, Landing::Nothing);
        assert!(finished);
        std::fs::remove_file(journal_path).unwrap();
        let trimmed = std::fs::read(&clip.path).unwrap();
        // Every crash point when unflushed writes are lost; a sample of the
        // others, which share most paths.
        for (landing, step) in [(Landing::Nothing, 1), (Landing::Everything, 3), (Landing::EveryOther, 3)] {
            for budget in (0..).step_by(step * stride) {
                let (journal_path, finished) = trim_until_power_loss(dir.path(), &clip, range, budget, landing);
                if finished {
                    break;
                }
                // Clipture starts again.
                let recovered = finish_journal(&journal_path).unwrap();
                let bytes = std::fs::read(&clip.path).unwrap();
                if recovered.is_some() {
                    assert_eq!(bytes, trimmed, "{range:?}, budget {budget}, {landing:?}: recovery must finish the trim");
                    outcomes[1] += 1;
                } else {
                    assert_eq!(bytes, clip.original, "{range:?}, budget {budget}, {landing:?}: an uncommitted trim must not touch the clip");
                    outcomes[0] += 1;
                }
                assert!(!journal_path.exists(), "the journal is gone after recovery");
            }
        }
    }
    assert!(outcomes[0] > 0 && outcomes[1] > 150, "{outcomes:?}");
}

#[test]
fn startup_recovery_finishes_interrupted_trims() {
    let dir = tempfile::tempdir().unwrap();
    let clip = fixture(dir.path());
    let (journal_path, finished) = trim_until_power_loss(dir.path(), &clip, HEAVY, 40, Landing::EveryOther);
    assert!(!finished && journal_path.exists());
    let done = ClipTrimmer::new(journals(dir.path())).finish_interrupted();
    assert_eq!(done.len(), 1);
    assert!((done[0].duration_seconds - 1.5).abs() < 1e-9);
    assert_eq!(read_layout(&clip.path).unwrap()[0].offsets.len(), 46);
}

