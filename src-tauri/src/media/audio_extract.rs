//! A clip's audio alone, as a small MP4, gathered with the clip's own index.
//! Audio is a few percent of a clip, and in Save in Place recordings it is
//! spread among the video; reading just its packets is much less work than
//! having FFmpeg read through the whole file to find them.
use std::{
    fs::File,
    io::{Read, Seek, SeekFrom, Write},
    path::Path,
};

use crate::error::{AppError, AppResult};

use super::layout_probe::layout_from_moov;
use super::linear_view::rewrite_path;
use super::mp4_box::{boxed, boxes, top_level_boxes};

const MAXIMUM_INDEX_BYTES: u64 = 64 * 1024 * 1024;
/// Audio of up to about three hours of a busy clip.
const MAXIMUM_AUDIO_BYTES: u64 = 512 * 1024 * 1024;
/// Packets closer than this are read together, video between them and all.
const MERGE_GAP: u64 = 32 * 1024;

/// One run of samples to copy: where it is in the clip, where it goes.
struct Copy {
    source: u64,
    len: u64,
    at: u64,
}

/// Writes every audio track of the clip at `path` (in order, edit lists and
/// all) to `out` as an MP4 holding nothing else. Returns how many there are.
pub(super) fn extract_audio(path: &Path, out: &Path) -> AppResult<usize> {
    let unsupported = |what: &str| AppError::Path(format!("the clip's audio cannot be read: {what}"));
    let mut clip = File::open(path).map_err(|source| io_error(path, source))?;
    let clip_len = clip.metadata().map_err(|source| io_error(path, source))?.len();
    let top = top_level_boxes(&mut clip, clip_len)
        .map_err(|source| io_error(path, source))?
        .ok_or_else(|| unsupported("its boxes are malformed"))?;
    let moov_box = top.iter().find(|item| &item.kind == b"moov").ok_or_else(|| unsupported("it has no index"))?;
    if moov_box.size > MAXIMUM_INDEX_BYTES {
        return Err(unsupported("its index is too large"));
    }
    let mut moov = vec![0_u8; moov_box.size as usize];
    clip.seek(SeekFrom::Start(moov_box.at)).and_then(|_| clip.read_exact(&mut moov)).map_err(|source| io_error(path, source))?;
    let layouts = layout_from_moov(&moov).ok_or_else(|| unsupported("its index is malformed"))?;
    let body = &moov[moov_box.header_len as usize..];
    let items = boxes(body).ok_or_else(|| unsupported("its index is malformed"))?;

    // Each audio chunk, laid out track after track.
    let mut copies = Vec::new();
    let mut tables: Vec<Vec<u64>> = Vec::new();
    let mut cursor = 0_u64;
    for layout in layouts.iter().filter(|layout| layout.is_audio()) {
        let mut table = Vec::with_capacity(layout.chunk_samples.len());
        let mut sample = 0_usize;
        for count in &layout.chunk_samples {
            let count = *count as usize;
            let len: u64 = layout.sizes.get(sample..sample + count).ok_or_else(|| unsupported("its index is malformed"))?.iter().map(|size| u64::from(*size)).sum();
            let source = layout.offsets.get(sample).copied().unwrap_or(0);
            if source.checked_add(len).is_none_or(|end| end > clip_len) {
                return Err(unsupported("a sample lies outside the file"));
            }
            table.push(cursor);
            if len > 0 {
                copies.push(Copy { source, len, at: cursor });
            }
            cursor += len;
            sample += count;
        }
        tables.push(table);
    }
    if cursor > MAXIMUM_AUDIO_BYTES {
        return Err(unsupported("there is too much of it"));
    }

    // The index: the movie header and the audio tracks, chunk offsets moved.
    let ftyp = boxed(b"ftyp", b"isom\0\0\x02\0isommp41");
    let index = |base: u64| -> Option<Vec<u8>> {
        let mut out = Vec::new();
        let mut tables = tables.iter();
        for (item, layout) in items.iter().filter(|item| &item.kind == b"trak").zip(&layouts) {
            if layout.is_audio() {
                let moved: Vec<u64> = tables.next()?.iter().map(|offset| base + offset).collect();
                out.extend(boxed(b"trak", &rewrite_path(item.body, &[b"mdia", b"minf", b"stbl"], &moved)?));
            }
        }
        let header = items.iter().find(|item| &item.kind == b"mvhd")?;
        Some(boxed(b"moov", &[header.full, &out].concat()))
    };
    let index_len = index(0).ok_or_else(|| unsupported("its index is malformed"))?.len() as u64;
    let base = ftyp.len() as u64 + index_len + 16;
    let index = index(base).ok_or_else(|| unsupported("its index is malformed"))?;

    // The audio, read in clip order with near neighbours merged.
    let mut data = vec![0_u8; cursor as usize];
    copies.sort_unstable_by_key(|copy| copy.source);
    let mut span = Vec::new();
    let mut first = 0;
    while first < copies.len() {
        let mut last = first;
        while last + 1 < copies.len() && copies[last + 1].source <= copies[last].source + copies[last].len + MERGE_GAP {
            last += 1;
        }
        let start = copies[first].source;
        let end = copies[first..=last].iter().map(|copy| copy.source + copy.len).max().unwrap_or(start);
        span.resize((end - start) as usize, 0);
        clip.seek(SeekFrom::Start(start)).and_then(|_| clip.read_exact(&mut span)).map_err(|source| io_error(path, source))?;
        for copy in &copies[first..=last] {
            let from = (copy.source - start) as usize;
            data[copy.at as usize..(copy.at + copy.len) as usize].copy_from_slice(&span[from..from + copy.len as usize]);
        }
        first = last + 1;
    }

    let mut file = File::create(out).map_err(|source| io_error(out, source))?;
    let mdat_header = [&1_u32.to_be_bytes()[..], b"mdat", &(16 + cursor).to_be_bytes()].concat();
    for part in [&ftyp[..], &index, &mdat_header, &data] {
        file.write_all(part).map_err(|source| io_error(out, source))?;
    }
    Ok(tables.len())
}

fn io_error(path: &Path, source: std::io::Error) -> AppError {
    AppError::Io { action: "read a clip's audio", path: path.to_owned(), source }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::media::layout_probe::read_layout;
    use crate::media::trim::tests::fixture;

    #[test]
    fn the_audio_comes_out_alone_byte_for_byte() {
        let dir = tempfile::tempdir().unwrap();
        let clip = fixture(dir.path());
        let out = dir.path().join("audio.m4a");
        assert_eq!(extract_audio(&clip.path, &out).unwrap(), 1);
        let written = std::fs::read(&out).unwrap();
        let tracks = read_layout(&out).unwrap();
        assert_eq!(tracks.len(), 1, "only the audio track is left");
        assert!(tracks[0].is_audio());
        assert_eq!(tracks[0].sizes.len(), clip.audio.len());
        for (index, packet) in clip.audio.iter().enumerate() {
            let at = tracks[0].offsets[index] as usize;
            assert_eq!(&written[at..at + packet.len()], &packet[..], "packet {index}");
        }
        assert!(written.len() < clip.original.len() / 2, "a fraction of the clip");
    }
}
