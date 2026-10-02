//! Read-only MP4 index inspection: where each track's samples sit in the file.
//! Only box headers and the `moov` index are read, never media payloads.
use std::{
    fs::File,
    io::{Read, Seek, SeekFrom},
    path::Path,
};

use crate::error::{AppError, AppResult};

const MAXIMUM_INDEX_BYTES: u64 = 64 * 1024 * 1024;
// Old clips kept most audio hundreds of MB from its video, so players seek
// across the file for every audio packet. New in-place clips stay well under.
const FAR_AUDIO_BYTES: u64 = 16 * 1024 * 1024;
const SCATTERED_AUDIO_BYTES: u64 = 64 * 1024 * 1024;
/// A sample stored this far before the one played just ahead of it counts as
/// out of time order (normal interleaving moves back far less).
const BACKWARD_BYTES: u64 = 1024 * 1024;
/// Indexes can come from a friend's PC, so counts are bounded before any
/// allocation (a 10-minute 120 fps clip has well under a million samples).
const MAXIMUM_SAMPLES: usize = 10_000_000;

#[derive(Clone, Debug, Default, PartialEq)]
pub struct TrackLayout {
    pub handler: [u8; 4],
    pub offsets: Vec<u64>,
    pub sizes: Vec<u32>,
    /// Decode time of each sample in seconds.
    pub times: Vec<f64>,
    pub duration_seconds: f64,
    /// How many samples each chunk of the offset table holds, in table order.
    pub chunk_samples: Vec<u32>,
}

impl TrackLayout {
    pub fn is_video(&self) -> bool {
        &self.handler == b"vide"
    }
    pub fn is_audio(&self) -> bool {
        &self.handler == b"soun"
    }
    pub fn payload_bytes(&self) -> u64 {
        self.sizes.iter().map(|size| u64::from(*size)).sum()
    }
}

pub fn read_layout(path: &Path) -> AppResult<Vec<TrackLayout>> {
    let io = |source| AppError::Io {
        action: "inspect clip layout",
        path: path.to_owned(),
        source,
    };
    let mut file = File::open(path).map_err(io)?;
    let end = file.metadata().map_err(io)?.len();
    let (start, stop) = top_level_box(&mut file, end, b"moov")
        .map_err(io)?
        .ok_or_else(|| AppError::Integration("clip has no MP4 index".into()))?;
    if stop - start > MAXIMUM_INDEX_BYTES {
        return Err(AppError::Integration("clip index is too large".into()));
    }
    let mut moov = vec![0_u8; (stop - start) as usize];
    file.seek(SeekFrom::Start(start)).map_err(io)?;
    file.read_exact(&mut moov).map_err(io)?;
    parse_moov(&moov).ok_or_else(|| AppError::Integration("clip index is malformed".into()))
}

/// True when some audio track is stored far from the video it plays with.
/// The tracks described by an in-memory `moov` box (header included).
pub fn layout_from_moov(moov: &[u8]) -> Option<Vec<TrackLayout>> {
    if moov.get(4..8)? != b"moov" {
        return None;
    }
    let header = if u32_at(moov, 0)? == 1 { 16 } else { 8 };
    parse_moov(moov.get(header..)?)
}

/// Whether much of the file is not media: in-place recordings saved after
/// the replay buffer wrapped around can be half zero padding. A lossless
/// remux keeps every sample and drops the padding.
pub fn is_padded(tracks: &[TrackLayout], file_bytes: u64) -> bool {
    let payload: u64 = tracks.iter().map(TrackLayout::payload_bytes).sum();
    payload > 0 && file_bytes > payload + payload / 8 + 4 * 1024 * 1024
}

/// Whether a clip streams badly as stored: zero padding between its samples
/// (in-place recordings can be half padding), or samples not in time order
/// (players then read all over the file). A remuxed copy fixes both.
pub fn needs_stream_layout(tracks: &[TrackLayout], file_bytes: u64) -> bool {
    if tracks.iter().all(|track| track.offsets.is_empty()) {
        return false;
    }
    let padded = is_padded(tracks, file_bytes);
    // Within one track, later samples sit later in a normal file; across
    // tracks, interleaving legitimately places audio a second or so after
    // its video, so only per-track order is checked.
    let scrambled = tracks.iter().any(|track| {
        let backward = track
            .offsets
            .windows(2)
            .filter(|pair| pair[1] + BACKWARD_BYTES < pair[0])
            .count();
        backward * 100 > track.offsets.len()
    });
    padded || scrambled || needs_interleave_repair(tracks)
}

pub fn needs_interleave_repair(tracks: &[TrackLayout]) -> bool {
    let Some(video) = tracks.iter().find(|track| track.is_video()) else {
        return false;
    };
    tracks.iter().filter(|track| track.is_audio()).any(|audio| {
        let (worst, far) = audio_distance(video, audio);
        worst > SCATTERED_AUDIO_BYTES && far * 4 > audio.offsets.len()
    })
}

/// Worst file distance from an audio sample to the video sample playing at
/// the same time, and how many audio samples are "far".
fn audio_distance(video: &TrackLayout, audio: &TrackLayout) -> (u64, usize) {
    let (mut worst, mut far, mut index) = (0, 0, 0);
    for (time, offset) in audio.times.iter().zip(&audio.offsets) {
        while index + 1 < video.times.len() && video.times[index + 1] <= *time {
            index += 1;
        }
        let Some(video_offset) = video.offsets.get(index) else { break };
        let distance = offset.abs_diff(*video_offset);
        worst = worst.max(distance);
        far += usize::from(distance > FAR_AUDIO_BYTES);
    }
    (worst, far)
}

fn top_level_box(file: &mut File, end: u64, kind: &[u8; 4]) -> std::io::Result<Option<(u64, u64)>> {
    let mut position = 0;
    while position + 8 <= end {
        let mut header = [0_u8; 16];
        file.seek(SeekFrom::Start(position))?;
        file.read_exact(&mut header[..8])?;
        let mut size = u64::from(u32::from_be_bytes(header[..4].try_into().unwrap()));
        let mut header_size = 8;
        if size == 1 {
            file.read_exact(&mut header[8..])?;
            size = u64::from_be_bytes(header[8..].try_into().unwrap());
            header_size = 16;
        } else if size == 0 {
            size = end - position;
        }
        if size < header_size || position + size > end {
            return Ok(None);
        }
        if &header[4..8] == kind {
            return Ok(Some((position + header_size, position + size)));
        }
        position += size;
    }
    Ok(None)
}

fn children(data: &[u8]) -> impl Iterator<Item = (&[u8], &[u8])> {
    let mut rest = data;
    std::iter::from_fn(move || {
        if rest.len() < 8 {
            return None;
        }
        let mut size = u64::from(u32::from_be_bytes(rest[..4].try_into().ok()?));
        let mut header = 8;
        if size == 1 {
            size = u64::from_be_bytes(rest.get(8..16)?.try_into().ok()?);
            header = 16;
        } else if size == 0 {
            size = rest.len() as u64;
        }
        let size = usize::try_from(size).ok()?;
        if size < header || size > rest.len() {
            return None;
        }
        let (current, next) = rest.split_at(size);
        rest = next;
        Some((&current[4..8], &current[header..]))
    })
}

fn child<'a>(data: &'a [u8], path: &[&[u8; 4]]) -> Option<&'a [u8]> {
    let (first, remaining) = path.split_first()?;
    let (_, body) = children(data).find(|(kind, _)| kind == first)?;
    if remaining.is_empty() {
        Some(body)
    } else {
        child(body, remaining)
    }
}

fn u32_at(data: &[u8], at: usize) -> Option<u32> {
    Some(u32::from_be_bytes(data.get(at..at + 4)?.try_into().ok()?))
}

fn u64_at(data: &[u8], at: usize) -> Option<u64> {
    Some(u64::from_be_bytes(data.get(at..at + 8)?.try_into().ok()?))
}

fn parse_moov(moov: &[u8]) -> Option<Vec<TrackLayout>> {
    children(moov)
        .filter(|(kind, _)| *kind == b"trak")
        .map(|(_, trak)| parse_track(trak))
        .collect()
}

fn parse_track(trak: &[u8]) -> Option<TrackLayout> {
    let mdia = child(trak, &[b"mdia"])?;
    let handler: [u8; 4] = child(mdia, &[b"hdlr"])?.get(8..12)?.try_into().ok()?;
    let mdhd = child(mdia, &[b"mdhd"])?;
    let timescale = f64::from(u32_at(mdhd, if mdhd.first()? == &1 { 20 } else { 12 })?.max(1));
    let stbl = child(mdia, &[b"minf", b"stbl"])?;

    let stsz = child(stbl, &[b"stsz"])?;
    let fixed = u32_at(stsz, 4)?;
    let count = u32_at(stsz, 8)? as usize;
    if count > MAXIMUM_SAMPLES || (fixed == 0 && count > stsz.len().saturating_sub(12) / 4) {
        return None;
    }
    let sizes = (0..count)
        .map(|i| if fixed != 0 { Some(fixed) } else { u32_at(stsz, 12 + 4 * i) })
        .collect::<Option<Vec<_>>>()?;

    let chunks: Vec<u64> = if let Some(stco) = child(stbl, &[b"stco"]) {
        (0..u32_at(stco, 4)? as usize).map(|i| u32_at(stco, 8 + 4 * i).map(u64::from)).collect::<Option<_>>()?
    } else {
        let co64 = child(stbl, &[b"co64"])?;
        (0..u32_at(co64, 4)? as usize).map(|i| u64_at(co64, 8 + 8 * i)).collect::<Option<_>>()?
    };

    let stsc = child(stbl, &[b"stsc"])?;
    let runs = u32_at(stsc, 4)? as usize;
    let mut offsets = Vec::with_capacity(count);
    let mut chunk_samples = Vec::with_capacity(chunks.len());
    for run in 0..runs {
        let first = u32_at(stsc, 8 + 12 * run)? as usize;
        let per_chunk = u32_at(stsc, 12 + 12 * run)? as usize;
        let last = if run + 1 < runs { u32_at(stsc, 8 + 12 * (run + 1))? as usize - 1 } else { chunks.len() };
        for chunk in first..=last {
            let mut offset = *chunks.get(chunk.checked_sub(1)?)?;
            let before = offsets.len();
            for _ in 0..per_chunk {
                if offsets.len() >= count {
                    break;
                }
                offsets.push(offset);
                offset += u64::from(sizes[offsets.len() - 1]);
            }
            chunk_samples.push((offsets.len() - before) as u32);
        }
    }
    if offsets.len() != count {
        return None;
    }

    let stts = child(stbl, &[b"stts"])?;
    let mut times = Vec::with_capacity(count);
    let mut ticks = 0_u64;
    for entry in 0..u32_at(stts, 4)? as usize {
        let (run, delta) = (u32_at(stts, 8 + 8 * entry)?, u32_at(stts, 12 + 8 * entry)?);
        let (run, delta) = (u64::from(run), u64::from(delta));
        // Only the first `count` times are kept; a huge run is not walked.
        let kept = run.min((count - times.len()) as u64);
        times.extend((0..kept).map(|i| (ticks + i * delta) as f64 / timescale));
        ticks = ticks.saturating_add(run.saturating_mul(delta));
    }
    times.resize(count, ticks as f64 / timescale);
    Some(TrackLayout {
        handler,
        offsets,
        sizes,
        times,
        duration_seconds: ticks as f64 / timescale,
        chunk_samples,
    })
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    fn boxed(kind: &[u8; 4], body: &[u8]) -> Vec<u8> {
        let mut out = ((body.len() + 8) as u32).to_be_bytes().to_vec();
        out.extend_from_slice(kind);
        out.extend_from_slice(body);
        out
    }

    fn words(values: &[u32]) -> Vec<u8> {
        values.iter().flat_map(|value| value.to_be_bytes()).collect()
    }

    /// One sample per chunk at the given offsets, `delta` ticks apart at 1 kHz.
    pub(crate) fn track_box(handler: &[u8; 4], offsets: &[u32], size: u32, delta: u32) -> Vec<u8> {
        let count = offsets.len() as u32;
        let mut stco = words(&[0, count]);
        stco.extend(words(offsets));
        let stbl = [
            boxed(b"stsz", &words(&[0, size, count])),
            boxed(b"stco", &stco),
            boxed(b"stsc", &words(&[0, 1, 1, 1, 1])),
            boxed(b"stts", &words(&[0, 1, count, delta])),
        ]
        .concat();
        let mut hdlr = words(&[0, 0]);
        hdlr.extend_from_slice(handler);
        let mdia = [
            boxed(b"mdhd", &words(&[0, 0, 0, 1000, 0])),
            boxed(b"hdlr", &hdlr),
            boxed(b"minf", &boxed(b"stbl", &stbl)),
        ]
        .concat();
        boxed(b"trak", &boxed(b"mdia", &mdia))
    }

    pub(crate) fn movie(tracks: &[Vec<u8>]) -> Vec<u8> {
        boxed(b"moov", &tracks.concat())
    }

    #[test]
    fn audio_stored_after_all_video_needs_repair_but_interleaved_does_not() {
        let video: Vec<u32> = (0..100).map(|i| i * 4_000_000).collect();
        let split = parse_moov(&movie(&[
            track_box(b"vide", &video, 1000, 100),
            track_box(b"soun", &(0..100).map(|i| 400_000_000 + i * 10).collect::<Vec<_>>(), 10, 100),
        ])[8..])
        .unwrap();
        assert!(needs_interleave_repair(&split));
        assert_eq!(split[0].offsets[1], 4_000_000);
        assert!((split[1].duration_seconds - 10.0).abs() < 1e-9);

        let mixed = parse_moov(&movie(&[
            track_box(b"vide", &video, 1000, 100),
            track_box(b"soun", &video.iter().map(|offset| offset + 1000).collect::<Vec<_>>(), 10, 100),
        ])[8..])
        .unwrap();
        assert!(!needs_interleave_repair(&mixed));
    }

    #[test]
    fn padded_or_scrambled_clips_need_a_stream_copy_and_clean_ones_do_not() {
        let ordered: Vec<u32> = (0..100).map(|i| i * 1000).collect();
        let clean = layout_from_moov(&movie(&[track_box(b"vide", &ordered, 1000, 100)])).unwrap();
        assert!(!needs_stream_layout(&clean, 100_000));
        // Half the file is padding.
        assert!(needs_stream_layout(&clean, 100_000 + 10 * 1024 * 1024));
        // Frames land wherever a ring buffer had room.
        let scrambled: Vec<u32> = (0..100).map(|i| ((i * 37) % 100) * 3_000_000).collect();
        let ring = layout_from_moov(&movie(&[track_box(b"vide", &scrambled, 1000, 100)])).unwrap();
        assert!(needs_stream_layout(&ring, 300_000_000));
    }

    #[test]
    fn hostile_sample_counts_are_refused_before_allocating() {
        // stsz claims four billion samples in a few bytes.
        let mut stbl = boxed(b"stsz", &words(&[0, 0, u32::MAX]));
        stbl.extend(boxed(b"stco", &words(&[0, 0])));
        stbl.extend(boxed(b"stsc", &words(&[0, 0])));
        stbl.extend(boxed(b"stts", &words(&[0, 1, u32::MAX, 1])));
        let mut hdlr = words(&[0, 0]);
        hdlr.extend_from_slice(b"vide");
        let mdia = [
            boxed(b"mdhd", &words(&[0, 0, 0, 1000, 0])),
            boxed(b"hdlr", &hdlr),
            boxed(b"minf", &boxed(b"stbl", &stbl)),
        ]
        .concat();
        let hostile = movie(&[boxed(b"trak", &boxed(b"mdia", &mdia))]);
        assert!(layout_from_moov(&hostile).is_none());
        assert!(layout_from_moov(b"   free").is_none());
    }

    #[test]
    fn reads_only_the_index_of_a_file() {
        let root = tempfile::tempdir().unwrap();
        let path = root.path().join("clip.mp4");
        let mut file = boxed(b"ftyp", b"isom");
        file.extend(boxed(b"mdat", &[0; 64]));
        file.extend(movie(&[track_box(b"vide", &[16, 40], 8, 10)]));
        std::fs::write(&path, file).unwrap();
        let tracks = read_layout(&path).unwrap();
        assert_eq!(tracks.len(), 1);
        assert_eq!(tracks[0].offsets, vec![16, 40]);
        assert_eq!(tracks[0].payload_bytes(), 16);
    }
}
