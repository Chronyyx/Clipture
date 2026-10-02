use std::{path::Path, sync::Arc};

use super::{
    layout_probe::{needs_stream_layout, tests::track_box},
    linear_view::{layout_of_view, LinearView, LinearViews},
    PlaybackPatch,
};

const MDAT_REGION: u32 = 6 * 1024 * 1024;

fn boxed(kind: &[u8; 4], body: &[u8]) -> Vec<u8> {
    let mut out = ((body.len() + 8) as u32).to_be_bytes().to_vec();
    out.extend_from_slice(kind);
    out.extend_from_slice(body);
    out
}

fn ftyp() -> Vec<u8> {
    boxed(b"ftyp", b"isom\0\0\x02\0isomavc1")
}

/// Where sample `index` of `track` starts inside the media region.
type Placement = fn(track: u32, index: u32) -> u32;

struct Track {
    handler: &'static [u8; 4],
    count: u32,
    size: u32,
    /// A whole `edts` box to put in front of the track's media box.
    edits: Option<Vec<u8>>,
}

fn sample_bytes(track: u32, index: u32, size: u32) -> Vec<u8> {
    (0..size).map(|at| (track * 101 + index * 7 + at) as u8 | 1).collect()
}

/// A clip whose samples sit where `place` says, inside a mostly zero media region.
fn write_clip(path: &Path, tracks: &[Track], place: Placement) -> Vec<Vec<Vec<u8>>> {
    write_sized_clip(path, tracks, place, MDAT_REGION)
}

fn write_sized_clip(path: &Path, tracks: &[Track], place: Placement, region: u32) -> Vec<Vec<Vec<u8>>> {
    let ftyp = ftyp();
    let base = ftyp.len() as u32 + 8;
    let mut region = vec![0_u8; region as usize];
    let mut traks = Vec::new();
    let mut samples = Vec::new();
    for (track_index, track) in tracks.iter().enumerate() {
        let track_index = track_index as u32;
        let mut offsets = Vec::new();
        let mut contents = Vec::new();
        for index in 0..track.count {
            let at = place(track_index, index);
            let bytes = sample_bytes(track_index, index, track.size);
            region[at as usize..(at + track.size) as usize].copy_from_slice(&bytes);
            offsets.push(base + at);
            contents.push(bytes);
        }
        let trak = track_box(track.handler, &offsets, track.size, 100);
        traks.push(match &track.edits {
            Some(edits) => boxed(b"trak", &[edits.clone(), trak[8..].to_vec()].concat()),
            None => trak,
        });
        samples.push(contents);
    }
    let mut file = ftyp;
    file.extend(boxed(b"mdat", &region));
    file.extend(boxed(b"moov", &traks.concat()));
    std::fs::write(path, file).unwrap();
    samples
}

fn video_and_audio(count: u32) -> Vec<Track> {
    vec![
        Track { handler: b"vide", count, size: 1000, edits: None },
        Track { handler: b"soun", count, size: 10, edits: None },
    ]
}

/// Video in reused ring slots, audio packed at the front: like Save in place.
fn ring(track: u32, index: u32) -> u32 {
    if track == 0 {
        ((index * 37) % 60) * 90_000 + 200_000
    } else {
        index * 16
    }
}

fn read_all(view: &LinearView, piece: usize) -> Vec<u8> {
    let mut file = view.open_source().unwrap();
    let mut out = vec![0_u8; view.len() as usize];
    for (index, chunk) in out.chunks_mut(piece).enumerate() {
        view.read_at(&mut file, (index * piece) as u64, chunk).unwrap();
    }
    out
}

#[test]
fn a_scrambled_clip_plays_start_to_end_with_every_sample_intact() {
    let root = tempfile::tempdir().unwrap();
    let path = root.path().join("ring.mp4");
    let samples = write_clip(&path, &video_and_audio(60), ring);
    let view = LinearView::open(&path, &[]).unwrap().expect("a scrambled clip gets a view");

    let bytes = read_all(&view, view.len() as usize);
    assert_eq!(read_all(&view, 777), bytes, "reads of any size agree");
    let payload = 60 * 1000 + 60 * 10;
    assert!(view.len() < payload + 64 * 1024, "the zero padding is gone");

    let tracks = layout_of_view(&view).unwrap();
    for (track, contents) in tracks.iter().zip(&samples) {
        for ((offset, size), expected) in track.offsets.iter().zip(&track.sizes).zip(contents) {
            assert_eq!(&bytes[*offset as usize..(*offset + u64::from(*size)) as usize], expected);
        }
    }
    // Interleaved by time: each video frame, then the audio playing with it.
    let mut by_time: Vec<(f64, usize, u64)> = tracks
        .iter()
        .enumerate()
        .flat_map(|(index, track)| track.times.iter().zip(&track.offsets).map(move |(time, offset)| (*time, index, *offset)))
        .collect();
    by_time.sort_by(|a, b| a.0.total_cmp(&b.0).then(a.1.cmp(&b.1)));
    assert!(by_time.windows(2).all(|pair| pair[0].2 < pair[1].2));
    assert!(!needs_stream_layout(&tracks, view.len()));
}

#[test]
fn clips_that_already_play_well_are_served_as_they_are() {
    let root = tempfile::tempdir().unwrap();
    let path = root.path().join("clean.mp4");
    // Front to back, each frame followed by its audio, no padding.
    write_sized_clip(&path, &video_and_audio(60), |track, index| index * 1010 + track * 1000, 60 * 1010);
    assert!(LinearView::open(&path, &[]).unwrap().is_none());

    std::fs::write(&path, b"not an mp4 at all").unwrap();
    assert!(LinearView::open(&path, &[]).unwrap().is_none());
}

/// An edit list that starts the video half a second in.
fn skip_half_second() -> Vec<u8> {
    let mut elst = vec![0, 0, 0, 0];
    elst.extend(1_u32.to_be_bytes());
    elst.extend(1000_u32.to_be_bytes());
    elst.extend(500_u32.to_be_bytes());
    elst.extend(0x0001_0000_u32.to_be_bytes());
    boxed(b"edts", &boxed(b"elst", &elst))
}

#[test]
fn edit_lists_line_tracks_up_by_when_they_play() {
    let root = tempfile::tempdir().unwrap();
    let path = root.path().join("edited.mp4");
    let tracks = vec![
        Track { handler: b"vide", count: 10, size: 1000, edits: Some(skip_half_second()) },
        Track { handler: b"soun", count: 10, size: 10, edits: None },
    ];
    write_clip(&path, &tracks, |track, index| if track == 0 { (9 - index) * 300_000 } else { 4_000_000 + index * 16 });
    let view = LinearView::open(&path, &[]).unwrap().unwrap();
    let layout = layout_of_view(&view).unwrap();
    let (video, audio) = (&layout[0].offsets, &layout[1].offsets);
    // Video frame 5 plays at 0 s, with the first audio.
    assert!(video[4] < audio[0] && video[5] < audio[0] && audio[0] < video[6]);
}

#[test]
fn patches_to_the_index_are_kept_in_the_view() {
    let root = tempfile::tempdir().unwrap();
    let path = root.path().join("patched.mp4");
    write_clip(&path, &video_and_audio(60), ring);
    let original = std::fs::read(&path).unwrap();
    let hdlr = original.windows(4).rposition(|w| w == b"soun").unwrap() as u64;
    let patch = PlaybackPatch { offset: hdlr, bytes: b"text".to_vec() };
    let view = LinearView::open(&path, &[patch]).unwrap().unwrap();
    let layout = layout_of_view(&view).unwrap();
    assert_eq!(&layout[1].handler, b"text");
}

#[test]
fn cached_views_follow_changes_to_the_clip() {
    let root = tempfile::tempdir().unwrap();
    let path = root.path().join("ring.mp4");
    write_clip(&path, &video_and_audio(60), ring);
    let views = LinearViews::default();
    let first = views.get(&path).unwrap().unwrap();
    assert!(Arc::ptr_eq(&first, &views.get(&path).unwrap().unwrap()));

    write_clip(&path, &video_and_audio(50), ring);
    let old = std::time::SystemTime::now() - std::time::Duration::from_secs(60);
    std::fs::File::options().write(true).open(&path).unwrap().set_modified(old).unwrap();
    assert!(!first.is_current());
    let second = views.get(&path).unwrap().unwrap();
    assert!(!Arc::ptr_eq(&first, &second));
    assert_eq!(layout_of_view(&second).unwrap()[0].offsets.len(), 50);
}


/// A clip laid out like a Save in place recording, for other domains' tests.
pub(crate) fn write_scrambled_clip(path: &Path) {
    write_clip(path, &video_and_audio(60), ring);
}
