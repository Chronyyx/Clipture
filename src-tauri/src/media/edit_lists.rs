//! Carries a clip's edit lists over to a lossless copy of it.
//!
//! FFmpeg applies edit lists while it reads, and copying a track whose
//! list ends in an empty edit (silence after the track stops) makes it
//! invent packets for that silence by repeating earlier ones. The repair
//! therefore copies samples exactly as stored (`-ignore_editlist 1`), which
//! drops the lists, and this module writes the original lists back into the
//! copy's index. Track timescales survive a copy; segment durations are in
//! the movie timescale, which may change, so they are rescaled.
use std::{
    fs::{self, OpenOptions},
    io::{Read, Seek, SeekFrom, Write},
    path::Path,
};

use crate::error::{AppError, AppResult};

use super::mp4_box::{boxed, boxes, top_level_boxes, u32_at, u64_at};

/// One edit: how long it plays (movie timescale), where in the media it
/// starts (-1 for an empty edit), and its rate.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Edit {
    duration: u64,
    media_time: i64,
    rate: i32,
}

/// Rewrites `copy`'s index so each track carries the edit list it has in
/// `original`. The copy's index must be its last box, as FFmpeg writes it.
pub(super) fn restore_edit_lists(original: &Path, copy: &Path) -> AppResult<()> {
    let (_, original_moov, _) = read_moov(original)?;
    let (at, copy_moov, last) = read_moov(copy)?;
    if !has_edit_lists(&original_moov) && !has_edit_lists(&copy_moov) {
        return Ok(()); // Nothing to carry over or take away.
    }
    if !last {
        return Err(AppError::Integration("repaired copy rejected: its index is not at the end".into()));
    }
    let restored = restore_in_moov(&original_moov, &copy_moov)
        .ok_or_else(|| AppError::Integration("repaired copy rejected: its index could not take the edit lists".into()))?;
    let mut file = OpenOptions::new().write(true).open(copy).map_err(|source| io_error("open repaired copy", copy, source))?;
    file.set_len(at).map_err(|source| io_error("rewrite repaired copy index", copy, source))?;
    file.seek(SeekFrom::End(0))
        .and_then(|_| file.write_all(&boxed(b"moov", &restored)))
        .and_then(|()| file.sync_all())
        .map_err(|source| io_error("rewrite repaired copy index", copy, source))
}

fn has_edit_lists(moov: &[u8]) -> bool {
    boxes(moov).is_some_and(|items| {
        items.iter().filter(|item| &item.kind == b"trak").any(|trak| super::mp4_box::find(trak.body, b"edts").is_some())
    })
}

fn io_error(action: &'static str, path: &Path, source: std::io::Error) -> AppError {
    AppError::Io { action, path: path.to_owned(), source }
}

/// The `moov` body, where its box starts, and whether it is the file's last box.
fn read_moov(path: &Path) -> AppResult<(u64, Vec<u8>, bool)> {
    let bad = || AppError::Integration(format!("{} has no usable index", path.display()));
    let mut file = fs::File::open(path).map_err(|source| io_error("read clip index", path, source))?;
    let end = file.metadata().map_err(|source| io_error("read clip index", path, source))?.len();
    let tops = top_level_boxes(&mut file, end).map_err(|source| io_error("read clip index", path, source))?.ok_or_else(bad)?;
    let moov = tops.iter().rev().find(|top| &top.kind == b"moov").ok_or_else(bad)?;
    let last = tops.last().is_some_and(|top| top.at == moov.at);
    let mut body = vec![0_u8; usize::try_from(moov.size - moov.header_len).map_err(|_| bad())?];
    file.seek(SeekFrom::Start(moov.at + moov.header_len))
        .and_then(|_| file.read_exact(&mut body))
        .map_err(|source| io_error("read clip index", path, source))?;
    Ok((moov.at, body, last))
}

/// The copy's `moov` body with the original's edit lists, track durations
/// and movie duration. `None` when the two do not match track for track.
fn restore_in_moov(original: &[u8], copy: &[u8]) -> Option<Vec<u8>> {
    let original_scale = movie_timescale(original)?;
    let copy_scale = movie_timescale(copy)?;
    let original_edits: Vec<Option<Vec<Edit>>> = boxes(original)?
        .into_iter()
        .filter(|item| &item.kind == b"trak")
        .map(|trak| edits_of(trak.body))
        .collect::<Option<_>>()?;
    let copy_items = boxes(copy)?;
    if copy_items.iter().filter(|item| &item.kind == b"trak").count() != original_edits.len() {
        return None;
    }
    let mut tracks = original_edits.into_iter();
    let mut longest = 0_u64;
    let mut out = Vec::with_capacity(copy.len() + 256);
    let mut mvhd_at = None;
    for item in &copy_items {
        match &item.kind {
            b"trak" => {
                let edits = tracks.next()?.map(|list| rescale(&list, original_scale, copy_scale));
                let (trak, duration) = rebuild_trak(item.body, edits.as_deref())?;
                longest = longest.max(duration);
                out.extend(boxed(b"trak", &trak));
            }
            b"mvhd" => {
                mvhd_at = Some(out.len());
                out.extend_from_slice(item.full);
            }
            _ => out.extend_from_slice(item.full),
        }
    }
    // The movie lasts as long as its longest track now does.
    let at = mvhd_at?;
    let header = if u32_at(&out, at)? == 1 { 16 } else { 8 };
    set_duration(&mut out[at + header..], longest, MVHD_DURATION)?;
    Some(out)
}

/// Where a duration sits in a full box body: (version 0 offset, version 1 offset).
const MVHD_DURATION: (usize, usize) = (16, 24);
const TKHD_DURATION: (usize, usize) = (20, 28);

fn movie_timescale(moov: &[u8]) -> Option<u64> {
    let mvhd = super::mp4_box::find(moov, b"mvhd")?;
    let scale = if *mvhd.first()? == 1 { u32_at(mvhd, 20)? } else { u32_at(mvhd, 12)? };
    (scale > 0).then_some(u64::from(scale))
}

/// The track's edit list, `Some(None)` when it has none.
fn edits_of(trak: &[u8]) -> Option<Option<Vec<Edit>>> {
    let Some(edts) = super::mp4_box::find(trak, b"edts") else { return Some(None) };
    let elst = super::mp4_box::find(edts, b"elst")?;
    let version = *elst.first()?;
    let count = u32_at(elst, 4)? as usize;
    let width = if version == 1 { 20 } else { 12 };
    if elst.len() < 8 + count.checked_mul(width)? {
        return None;
    }
    (0..count)
        .map(|index| {
            let at = 8 + index * width;
            Some(if version == 1 {
                Edit { duration: u64_at(elst, at)?, media_time: u64_at(elst, at + 8)? as i64, rate: u32_at(elst, at + 16)? as i32 }
            } else {
                Edit { duration: u64::from(u32_at(elst, at)?), media_time: i64::from(u32_at(elst, at + 4)? as i32), rate: u32_at(elst, at + 8)? as i32 }
            })
        })
        .collect::<Option<Vec<_>>>()
        .map(Some)
}

fn rescale(edits: &[Edit], from: u64, to: u64) -> Vec<Edit> {
    edits
        .iter()
        .map(|edit| Edit { duration: ((u128::from(edit.duration) * u128::from(to) + u128::from(from) / 2) / u128::from(from)) as u64, ..*edit })
        .collect()
}

fn edts_box(edits: &[Edit]) -> Vec<u8> {
    let mut elst = vec![1, 0, 0, 0];
    elst.extend_from_slice(&(edits.len() as u32).to_be_bytes());
    for edit in edits {
        elst.extend_from_slice(&edit.duration.to_be_bytes());
        elst.extend_from_slice(&edit.media_time.to_be_bytes());
        elst.extend_from_slice(&edit.rate.to_be_bytes());
    }
    boxed(b"edts", &boxed(b"elst", &elst))
}

/// The track with its own edit list dropped and `edits` (if any) placed
/// after its header, whose duration then becomes the edits' total. Returns
/// the body and the track's duration in the movie timescale.
fn rebuild_trak(trak: &[u8], edits: Option<&[Edit]>) -> Option<(Vec<u8>, u64)> {
    let mut out = Vec::with_capacity(trak.len() + 64);
    let mut duration = 0;
    for item in boxes(trak)? {
        match &item.kind {
            b"edts" => {}
            b"tkhd" => {
                let header = item.full.len() - item.body.len();
                let mut tkhd = item.full.to_vec();
                match edits {
                    Some(list) => {
                        duration = list.iter().map(|edit| edit.duration).sum();
                        set_duration(&mut tkhd[header..], duration, TKHD_DURATION)?;
                    }
                    None => duration = read_duration(item.body, TKHD_DURATION)?,
                }
                out.extend(tkhd);
                if let Some(list) = edits {
                    out.extend(edts_box(list));
                }
            }
            _ => out.extend_from_slice(item.full),
        }
    }
    Some((out, duration))
}

fn read_duration(body: &[u8], (v0, v1): (usize, usize)) -> Option<u64> {
    if *body.first()? == 1 { u64_at(body, v1) } else { u32_at(body, v0).map(u64::from) }
}

fn set_duration(body: &mut [u8], duration: u64, (v0, v1): (usize, usize)) -> Option<()> {
    if *body.first()? == 1 {
        body.get_mut(v1..v1 + 8)?.copy_from_slice(&duration.to_be_bytes());
    } else {
        let value = u32::try_from(duration).unwrap_or(u32::MAX);
        body.get_mut(v0..v0 + 4)?.copy_from_slice(&value.to_be_bytes());
    }
    Some(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn full_box(kind: &[u8; 4], version: u8, rest: &[u8]) -> Vec<u8> {
        let mut body = vec![version, 0, 0, 0];
        body.extend_from_slice(rest);
        boxed(kind, &body)
    }

    fn mvhd(timescale: u32, duration: u32) -> Vec<u8> {
        let mut rest = vec![0; 8];
        rest.extend_from_slice(&timescale.to_be_bytes());
        rest.extend_from_slice(&duration.to_be_bytes());
        rest.resize(96, 0);
        full_box(b"mvhd", 0, &rest)
    }

    fn tkhd(duration: u32) -> Vec<u8> {
        let mut rest = vec![0; 16];
        rest.extend_from_slice(&duration.to_be_bytes());
        rest.resize(80, 0);
        full_box(b"tkhd", 0, &rest)
    }

    fn elst_v0(edits: &[(u32, i32)]) -> Vec<u8> {
        let mut rest = (edits.len() as u32).to_be_bytes().to_vec();
        for (duration, media_time) in edits {
            rest.extend_from_slice(&duration.to_be_bytes());
            rest.extend_from_slice(&media_time.to_be_bytes());
            rest.extend_from_slice(&0x0001_0000_u32.to_be_bytes());
        }
        boxed(b"edts", &full_box(b"elst", 0, &rest))
    }

    fn trak(parts: &[Vec<u8>]) -> Vec<u8> {
        boxed(b"trak", &parts.concat())
    }

    #[test]
    fn edit_lists_return_rescaled_with_their_durations() {
        let mdia = boxed(b"mdia", b"");
        // Original: movie timescale 1000; audio delayed 20 ms, 1 s of media,
        // then 500 ms of silence. Video has no list.
        let original = [
            mvhd(1000, 1520),
            trak(&[tkhd(1000), mdia.clone()]),
            trak(&[tkhd(1520), elst_v0(&[(20, -1), (1000, 0), (500, -1)]), mdia.clone()]),
        ]
        .concat();
        // Copy: movie timescale 600, no edit lists, its own durations.
        let copy = [mvhd(600, 600), trak(&[tkhd(600), mdia.clone()]), trak(&[tkhd(600), mdia.clone()])].concat();
        let restored = restore_in_moov(&original, &copy).unwrap();

        let traks: Vec<_> = boxes(&restored).unwrap().into_iter().filter(|item| &item.kind == b"trak").collect();
        assert_eq!(edits_of(traks[0].body).unwrap(), None, "a track without a list stays without one");
        let audio = edits_of(traks[1].body).unwrap().unwrap();
        assert_eq!(audio, vec![
            Edit { duration: 12, media_time: -1, rate: 0x0001_0000 },
            Edit { duration: 600, media_time: 0, rate: 0x0001_0000 },
            Edit { duration: 300, media_time: -1, rate: 0x0001_0000 },
        ]);
        let tkhd = super::super::mp4_box::find(traks[1].body, b"tkhd").unwrap();
        assert_eq!(read_duration(tkhd, TKHD_DURATION), Some(912), "the track lasts as long as its edits");
        let mvhd = super::super::mp4_box::find(&restored, b"mvhd").unwrap();
        assert_eq!(read_duration(mvhd, MVHD_DURATION), Some(912), "the movie lasts as long as its longest track");
    }

    #[test]
    fn mismatched_track_counts_are_refused() {
        let mdia = boxed(b"mdia", b"");
        let original = [mvhd(1000, 1000), trak(&[tkhd(1000), mdia.clone()])].concat();
        let copy = [mvhd(1000, 1000), trak(&[tkhd(1000), mdia.clone()]), trak(&[tkhd(1000), mdia])].concat();
        assert!(restore_in_moov(&original, &copy).is_none());
    }
}
