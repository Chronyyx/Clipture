use std::{
    path::{Path, PathBuf},
    sync::{
        atomic::{AtomicUsize, Ordering},
        Arc,
    },
};

use super::super::layout_probe::read_layout;
use super::super::mp4_box::{boxed, top_level_boxes};
use super::super::trim::disk::Disk;
use super::super::trim::tests::{full_box, sample, track, words, AUDIO_SCALE, FRAME, FRAMES, KEYFRAME_EVERY, MOVIE_SCALE, PACKET, PACKETS, VIDEO_SCALE};
use super::super::{FfmpegExecutor, FfmpegJob, FfmpegOutput};
use super::{encode, finish_journal, journal, plan, read_index, ClipMixer, MixError};
use crate::error::AppResult;

struct Clip {
    path: PathBuf,
    original: Vec<u8>,
    video: Vec<Vec<u8>>,
    audio: [Vec<Vec<u8>>; 2],
}

/// Four seconds of video and two audio tracks, interleaved in time with
/// zero padding between some packets, like a Save in Place recording.
fn clip(dir: &Path) -> Clip {
    let video: Vec<Vec<u8>> = (0..FRAMES)
        .map(|index| sample(1, index, if index % KEYFRAME_EVERY == 0 { 6000 } else { 1500 }))
        .collect();
    let audio = [
        (0..PACKETS).map(|index| sample(2, index, 300 + (index % 5) * 10)).collect::<Vec<_>>(),
        (0..PACKETS).map(|index| sample(3, index, 200 + (index % 3) * 10)).collect::<Vec<_>>(),
    ];
    let mut order: Vec<(usize, usize)> = Vec::new();
    let mut next_packet = 0;
    for frame in 0..FRAMES {
        order.push((0, frame));
        let frame_end = (frame as u64 + 1) * u64::from(FRAME) * u64::from(AUDIO_SCALE) / u64::from(VIDEO_SCALE);
        while next_packet < PACKETS && (next_packet as u64) * u64::from(PACKET) < frame_end {
            order.extend([(1, next_packet), (2, next_packet)]);
            next_packet += 1;
        }
    }
    order.extend((next_packet..PACKETS).flat_map(|packet| [(1, packet), (2, packet)]));

    let ftyp = boxed(b"ftyp", b"isom\0\0\x02\0isomavc1");
    let mdat_at = ftyp.len() as u64;
    let mut payload = Vec::new();
    let mut at = [vec![0_u32; FRAMES], vec![0_u32; PACKETS], vec![0_u32; PACKETS]];
    for (position, (stream, index)) in order.iter().enumerate() {
        if position % 7 == 3 {
            payload.extend(vec![0_u8; 900]);
        }
        at[*stream][*index] = (mdat_at + 8 + payload.len() as u64) as u32;
        payload.extend(if *stream == 0 { &video[*index] } else { &audio[*stream - 1][*index] });
    }
    let mut mvhd = words(&[0, 0, MOVIE_SCALE, 4000]);
    mvhd.resize(96, 0);
    let edits = [(16, -1), (3967, 0), (17, -1)];
    let moov = boxed(
        b"moov",
        &[
            full_box(b"mvhd", 0, &mvhd),
            track(1, b"vide", VIDEO_SCALE, FRAMES as u32 * FRAME, &video, &at[0], FRAME, Some(vec![1, 31, 61, 91]), &[(3967, 100)]),
            track(2, b"soun", AUDIO_SCALE, PACKETS as u32 * PACKET, &audio[0], &at[1], PACKET, None, &edits),
            track(3, b"soun", AUDIO_SCALE, PACKETS as u32 * PACKET, &audio[1], &at[2], PACKET, None, &edits),
        ]
        .concat(),
    );
    let original = [ftyp, boxed(b"mdat", &payload), moov].concat();
    let path = dir.join("clip.mp4");
    std::fs::write(&path, &original).unwrap();
    Clip { path, original, video, audio }
}

/// Stands in for FFmpeg: writes `PACKETS + 1` packets of `size` bytes per
/// requested output, with 1024 ticks of priming, like the AAC encoder.
struct FakeEncoder {
    size: usize,
    runs: AtomicUsize,
}

impl FakeEncoder {
    fn new(size: usize) -> Arc<Self> {
        Arc::new(Self { size, runs: AtomicUsize::new(0) })
    }

    fn packets(&self, output: usize) -> Vec<Vec<u8>> {
        (0..=PACKETS).map(|index| sample(10 + output as u8, index, self.size)).collect()
    }
}

impl FfmpegExecutor for FakeEncoder {
    fn run(&self, job: FfmpegJob) -> AppResult<FfmpegOutput> {
        self.runs.fetch_add(1, Ordering::SeqCst);
        let args: Vec<String> = job.args.iter().map(|arg| arg.to_string_lossy().into_owned()).collect();
        assert!(args.windows(2).any(|pair| pair == ["-ignore_editlist", "1"]));
        let filter = &args[args.iter().position(|arg| arg == "-filter_complex").unwrap() + 1];
        let outputs = filter.split(';').count();
        let ftyp = boxed(b"ftyp", b"M4A \0\0\x02\0isomM4A ");
        let mut payload = Vec::new();
        let mut traks = Vec::new();
        for output in 0..outputs {
            let packets = self.packets(output);
            let offsets: Vec<u32> = packets
                .iter()
                .map(|packet| {
                    let at = (ftyp.len() + 8 + payload.len()) as u32;
                    payload.extend(packet);
                    at
                })
                .collect();
            let duration = packets.len() as u32 * PACKET;
            traks.push(track(output as u32 + 1, b"soun", AUDIO_SCALE, duration, &packets, &offsets, PACKET, None, &[(3967, 1024)]));
        }
        let mut mvhd = words(&[0, 0, MOVIE_SCALE, 4000]);
        mvhd.resize(96, 0);
        let moov = boxed(b"moov", &[full_box(b"mvhd", 0, &mvhd), traks.concat()].concat());
        std::fs::write(args.last().unwrap(), [ftyp, boxed(b"mdat", &payload), moov].concat()).unwrap();
        Ok(FfmpegOutput { success: true, exit_code: Some(0), stdout: Vec::new(), stderr: String::new() })
    }
}

fn mixer(dir: &Path, encoder: &Arc<FakeEncoder>) -> ClipMixer {
    ClipMixer::new(dir.join("journals"), encoder.clone())
}

fn bytes_at(file: &[u8], offset: u64, len: u32) -> &[u8] {
    &file[offset as usize..offset as usize + len as usize]
}

/// The audio edit lists of a file, in track order.
fn audio_edits(file: &[u8]) -> Vec<Vec<(u64, i64, i16, i16)>> {
    let moov = super::super::mp4_box::boxes(file).unwrap().into_iter().find(|item| &item.kind == b"moov").unwrap().body;
    super::super::mp4_box::boxes(moov)
        .unwrap()
        .into_iter()
        .filter(|item| &item.kind == b"trak")
        .skip(1)
        .map(|trak| {
            let edts = super::super::mp4_box::find(trak.body, b"edts").unwrap();
            encode::edit_entries(super::super::mp4_box::find(edts, b"elst").unwrap()).unwrap()
        })
        .collect()
}

#[test]
fn a_mix_rewrites_only_the_changed_track_in_its_own_slots() {
    let dir = tempfile::tempdir().unwrap();
    let clip = clip(dir.path());
    let encoder = FakeEncoder::new(250);
    let done = mixer(dir.path(), &encoder).mix(&clip.path, &[0.5, 1.0]).unwrap();
    assert_eq!(done.tracks_changed, 1);
    assert_eq!(done.bytes_before, clip.original.len() as u64);

    let file = std::fs::read(&clip.path).unwrap();
    assert_eq!(done.bytes_after, file.len() as u64);
    let old = {
        std::fs::write(dir.path().join("old.mp4"), &clip.original).unwrap();
        read_layout(&dir.path().join("old.mp4")).unwrap()
    };
    let new = read_layout(&clip.path).unwrap();
    // Video and the unchanged track: same places, same bytes.
    for (track, samples) in [(0, &clip.video), (2, &clip.audio[1])] {
        assert_eq!(new[track].offsets, old[track].offsets);
        for (index, bytes) in samples.iter().enumerate() {
            assert_eq!(bytes_at(&file, new[track].offsets[index], new[track].sizes[index]), &bytes[..]);
        }
    }
    // The changed track: every new packet, inside the old track's slots in
    // time order, except what no slot could take.
    let packets = encoder.packets(0);
    assert_eq!(new[1].sizes.len(), packets.len());
    let slots: Vec<(u64, u64)> = old[1].offsets.iter().zip(&old[1].sizes).map(|(at, size)| (*at, u64::from(*size))).collect();
    let mut inside = 0;
    for (index, packet) in packets.iter().enumerate() {
        let at = new[1].offsets[index];
        assert_eq!(bytes_at(&file, at, new[1].sizes[index]), &packet[..]);
        if slots.iter().any(|(slot, size)| at >= *slot && at + packet.len() as u64 <= slot + size) {
            inside += 1;
        }
    }
    assert_eq!(inside, PACKETS, "one packet per slot; the extra one goes after the media");
    assert!(new[1].offsets.windows(2).all(|pair| pair[0] < pair[1]), "stored in time order");
    // The old audio is gone: the rest of each slot is zeros.
    for (slot, size) in &slots {
        let used = packets[0].len() as u64;
        assert!(file[(slot + used) as usize..(slot + size) as usize].iter().all(|byte| *byte == 0));
    }
    // The priming is skipped by the edit list; the other track keeps its own.
    assert_eq!(audio_edits(&file), vec![vec![(16, -1, 1, 0), (3967, 1024, 1, 0), (17, -1, 1, 0)], vec![(16, -1, 1, 0), (3967, 0, 1, 0), (17, -1, 1, 0)]]);
    assert!((new[1].duration_seconds - packets.len() as f64 * 1024.0 / 48_000.0).abs() < 1e-9);
    assert_eq!(std::fs::read_dir(dir.path().join("journals")).unwrap().count(), 0, "no journal or encoded audio is left");
}

#[test]
fn louder_audio_that_outgrows_its_slots_goes_after_the_media() {
    let dir = tempfile::tempdir().unwrap();
    let clip = clip(dir.path());
    let encoder = FakeEncoder::new(400);
    mixer(dir.path(), &encoder).mix(&clip.path, &[1.0, 1.8]).unwrap();
    let file = std::fs::read(&clip.path).unwrap();
    let new = read_layout(&clip.path).unwrap();
    let mdat_end = 24 + u64::from(u32::from_be_bytes(file[24..28].try_into().unwrap()));
    for (index, packet) in encoder.packets(0).iter().enumerate() {
        let at = new[2].offsets[index];
        assert_eq!(bytes_at(&file, at, new[2].sizes[index]), &packet[..]);
        assert!(at + packet.len() as u64 <= mdat_end, "inside the media box");
    }
    assert_eq!(&file[mdat_end as usize + 4..mdat_end as usize + 8], b"moov");
    // The old packets of the changed track are all zeros now.
    std::fs::write(dir.path().join("old.mp4"), &clip.original).unwrap();
    let old = read_layout(&dir.path().join("old.mp4")).unwrap();
    for (at, size) in old[2].offsets.iter().zip(&old[2].sizes) {
        assert!(bytes_at(&file, *at, *size).iter().all(|byte| *byte == 0));
    }
}

#[test]
fn unchanged_levels_leave_the_file_alone() {
    let dir = tempfile::tempdir().unwrap();
    let clip = clip(dir.path());
    let encoder = FakeEncoder::new(250);
    let done = mixer(dir.path(), &encoder).mix(&clip.path, &[1.0, 1.001]).unwrap();
    assert_eq!(done.tracks_changed, 0);
    assert_eq!(encoder.runs.load(Ordering::SeqCst), 0);
    assert_eq!(std::fs::read(&clip.path).unwrap(), clip.original);
}

#[test]
fn levels_out_of_range_or_miscounted_are_refused() {
    let dir = tempfile::tempdir().unwrap();
    let clip = clip(dir.path());
    let encoder = FakeEncoder::new(250);
    let mixer = mixer(dir.path(), &encoder);
    assert!(matches!(mixer.mix(&clip.path, &[10.5, 1.0]), Err(MixError::Unsupported(_))));
    assert!(matches!(mixer.mix(&clip.path, &[-0.1, 1.0]), Err(MixError::Unsupported(_))));
    assert!(matches!(mixer.mix(&clip.path, &[f64::NAN, 1.0]), Err(MixError::Unsupported(_))));
    assert!(matches!(mixer.mix(&clip.path, &[0.5]), Err(MixError::Unsupported(_))));
    assert_eq!(encoder.runs.load(Ordering::SeqCst), 0);
    assert_eq!(std::fs::read(&clip.path).unwrap(), clip.original);
}

#[test]
fn a_mix_cut_short_at_any_write_is_finished_from_its_journal() {
    let dir = tempfile::tempdir().unwrap();
    let clip = clip(dir.path());
    let encoder = FakeEncoder::new(330);
    mixer(dir.path(), &encoder).mix(&clip.path, &[0.0, 0.7]).unwrap();
    let finished = std::fs::read(&clip.path).unwrap();

    std::fs::write(&clip.path, &clip.original).unwrap();
    let mut file = std::fs::File::open(&clip.path).unwrap();
    let (moov, clip_len) = read_index(&mut file).unwrap();
    let tracks = plan::audio_tracks(&moov).unwrap();
    let changed = [(0, 0.0), (1, 0.7)];
    let encoded_path = dir.path().join("encoded.m4a");
    let encoded = encode::encode(encoder.as_ref(), &clip.path, &encoded_path, &tracks, &changed).unwrap();
    let top = top_level_boxes(&mut file, clip_len).unwrap().unwrap();
    drop(file);
    let plan = plan::plan(&clip.path, clip_len, &top, &moov, &tracks, &changed, &encoded).unwrap();
    assert!(plan.final_len > clip_len, "the louder track overflows: the file grows");

    let journal_path = dir.path().join("crash.mix");
    for done in 0..=plan.writes.len() {
        std::fs::write(&clip.path, &clip.original).unwrap();
        let mut journal_file = super::super::trim::disk::open_exclusive(&journal_path, true).unwrap();
        journal::commit(&mut journal_file, &plan).unwrap();
        drop(journal_file);
        // The power goes after `done` writes.
        let mut target = std::fs::OpenOptions::new().write(true).open(&clip.path).unwrap();
        for write in &plan.writes[..done] {
            Disk::write_at(&mut target, write.at, &write.bytes).unwrap();
        }
        drop(target);
        let recovered = finish_journal(&journal_path).unwrap().unwrap();
        assert_eq!(recovered.bytes_after, finished.len() as u64);
        assert_eq!(std::fs::read(&clip.path).unwrap(), finished, "cut after {done} writes");
        assert!(!journal_path.exists());
    }
}

#[test]
fn an_uncommitted_journal_leaves_the_clip_untouched() {
    let dir = tempfile::tempdir().unwrap();
    let clip = clip(dir.path());
    let journal_path = dir.path().join("torn.mix");
    // A body without its header: the crash came before the commit point.
    let mut journal_file = super::super::trim::disk::open_exclusive(&journal_path, true).unwrap();
    Disk::write_at(&mut journal_file, 4096, &[7_u8; 5000]).unwrap();
    drop(journal_file);
    assert_eq!(finish_journal(&journal_path).unwrap(), None);
    assert!(!journal_path.exists());
    assert_eq!(std::fs::read(&clip.path).unwrap(), clip.original);
}
