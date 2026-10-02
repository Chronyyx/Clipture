//! A start-to-end view of a clip whose samples are stored out of time order.
//!
//! Save in place turns the replay buffer itself into the clip, so frames sit
//! wherever the buffer had room and moments a second apart can be hundreds of
//! MB apart in the file. Players then jump across the whole file and stall
//! while their cache fills. The view presents the same clip as a normal file:
//! the original `ftyp`, the original `moov` with only its chunk offset tables
//! rewritten, an `mdat` header, then every chunk in playing order. Chunks move
//! whole, so sample sizes, timing, keyframes and codec configuration are the
//! recorded ones. Bytes are read from the clip when asked for; nothing is
//! copied or written.
use std::{
    collections::VecDeque,
    fs::File,
    io::{self, Read, Seek, SeekFrom},
    path::{Path, PathBuf},
    sync::{Arc, Mutex},
    time::SystemTime,
};

use crate::error::{AppError, AppResult};

use super::{
    apply_patches,
    layout_probe::{layout_from_moov, needs_stream_layout},
    PlaybackPatch,
};

const MAXIMUM_INDEX_BYTES: u64 = 64 * 1024 * 1024;
/// Views the sharing server keeps ready; each is the clip's index plus a
/// small extent table.
const CACHED_VIEWS: usize = 4;

pub struct LinearView {
    path: PathBuf,
    stamp: Stamp,
    /// `ftyp`, the rewritten `moov` and the `mdat` header.
    header: Vec<u8>,
    /// Where each run of the view's media bytes lives in the clip, in view order.
    extents: Vec<Extent>,
    total: u64,
}

#[derive(Clone, Copy, Debug, PartialEq)]
struct Stamp {
    len: u64,
    modified: Option<SystemTime>,
}

#[derive(Clone, Copy, Debug)]
struct Extent {
    start: u64,
    source: u64,
    len: u64,
}

impl std::fmt::Debug for LinearView {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("LinearView")
            .field("path", &self.path)
            .field("total", &self.total)
            .field("extents", &self.extents.len())
            .finish()
    }
}

impl LinearView {
    /// The view of `path` with `patches` (absolute offsets into the clip's
    /// own index) applied. `None` when the clip already plays well as
    /// stored or is not an MP4 this understands; the clip is then served as is.
    pub fn open(path: &Path, patches: &[PlaybackPatch]) -> AppResult<Option<Self>> {
        let io = |source| AppError::Io {
            action: "inspect clip layout",
            path: path.to_owned(),
            source,
        };
        let mut file = File::open(path).map_err(io)?;
        let stamp = stamp_of(&file).map_err(io)?;
        let Some(top) = top_level(&mut file, stamp.len).map_err(io)? else {
            return Ok(None);
        };
        let Some((moov_at, moov_len)) = top.moov else {
            return Ok(None);
        };
        if top.fragmented || moov_len > MAXIMUM_INDEX_BYTES {
            return Ok(None);
        }
        let mut moov = vec![0_u8; moov_len as usize];
        file.seek(SeekFrom::Start(moov_at)).map_err(io)?;
        file.read_exact(&mut moov).map_err(io)?;
        apply_patches(moov_at, &mut moov, patches);
        let ftyp = match top.ftyp {
            Some((at, len)) if len <= 4096 => {
                let mut bytes = vec![0_u8; len as usize];
                file.seek(SeekFrom::Start(at)).map_err(io)?;
                file.read_exact(&mut bytes).map_err(io)?;
                bytes
            }
            _ => boxed(b"ftyp", b"isom\0\0\x02\0isomiso2avc1mp41"),
        };
        Ok(build(path, stamp, &ftyp, &moov))
    }

    /// Bytes in the view.
    pub fn len(&self) -> u64 {
        self.total
    }

    pub fn path(&self) -> &Path {
        &self.path
    }

    /// The clip has not been changed or replaced since the view was made.
    pub fn is_current(&self) -> bool {
        File::open(&self.path)
            .and_then(|file| stamp_of(&file))
            .is_ok_and(|stamp| stamp == self.stamp)
    }

    pub fn open_source(&self) -> io::Result<File> {
        File::open(&self.path)
    }

    /// Fills `buffer` with the view's bytes from `offset`, reading the clip
    /// through `file` (from [`Self::open_source`]).
    pub fn read_at(&self, file: &mut File, offset: u64, buffer: &mut [u8]) -> io::Result<()> {
        let end = offset
            .checked_add(buffer.len() as u64)
            .filter(|end| *end <= self.total)
            .ok_or_else(|| io::Error::new(io::ErrorKind::UnexpectedEof, "read past the end of the view"))?;
        let header = self.header.len() as u64;
        let mut position = offset;
        let mut filled = 0;
        while position < end {
            let out = &mut buffer[filled..];
            let count = if position < header {
                let count = ((header - position) as usize).min(out.len());
                out[..count].copy_from_slice(&self.header[position as usize..position as usize + count]);
                count
            } else {
                let index = self.extents.partition_point(|extent| extent.start + extent.len <= position);
                let extent = self.extents[index];
                let skip = position - extent.start;
                let count = ((extent.len - skip) as usize).min(out.len());
                file.seek(SeekFrom::Start(extent.source + skip))?;
                file.read_exact(&mut out[..count])?;
                count
            };
            position += count as u64;
            filled += count;
        }
        Ok(())
    }
}

/// Recently used views, for a server answering many ranges of the same clips.
#[derive(Default)]
pub struct LinearViews {
    recent: Mutex<VecDeque<Arc<LinearView>>>,
}

impl LinearViews {
    /// The current view of `path`, or `None` when the clip needs none.
    pub fn get(&self, path: &Path) -> AppResult<Option<Arc<LinearView>>> {
        {
            let mut recent = self.recent.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
            if let Some(index) = recent.iter().position(|view| view.path == path) {
                let view = recent.remove(index).expect("index is in range");
                if view.is_current() {
                    recent.push_front(view.clone());
                    return Ok(Some(view));
                }
            }
        }
        let Some(view) = LinearView::open(path, &[])?.map(Arc::new) else {
            return Ok(None);
        };
        let mut recent = self.recent.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        recent.retain(|cached| cached.path != path);
        recent.push_front(view.clone());
        recent.truncate(CACHED_VIEWS);
        Ok(Some(view))
    }
}

fn build(path: &Path, stamp: Stamp, ftyp: &[u8], moov: &[u8]) -> Option<LinearView> {
    let tracks = layout_from_moov(moov)?;
    if !needs_stream_layout(&tracks, stamp.len) {
        return None;
    }
    let body = moov.get(box_header_len(moov)?..)?;
    let shifts = edit_shifts(body, tracks.len())?;

    // Every chunk of every track, keyed by when it plays.
    let mut chunks = Vec::new();
    for (track_index, track) in tracks.iter().enumerate() {
        let mut sample = 0_usize;
        for (chunk_index, count) in track.chunk_samples.iter().enumerate() {
            let count = *count as usize;
            let samples = sample..sample + count;
            let source = *track.offsets.get(sample).unwrap_or(&0);
            let len: u64 = track.sizes.get(samples.clone())?.iter().map(|size| u64::from(*size)).sum();
            let time = track.times.get(sample).map_or(f64::MAX, |time| time + shifts[track_index]);
            if source.checked_add(len)? > stamp.len {
                return None;
            }
            chunks.push(Chunk { time, track: track_index, index: chunk_index, source, len });
            sample = samples.end;
        }
        if sample != track.offsets.len() {
            return None;
        }
    }
    chunks.sort_by(|a, b| a.time.total_cmp(&b.time).then(a.track.cmp(&b.track)).then(a.index.cmp(&b.index)));

    let mut offsets: Vec<Vec<u64>> = tracks.iter().map(|track| vec![0; track.chunk_samples.len()]).collect();
    let header_len = (ftyp.len() + rewrite_moov(body, &offsets)?.len() + 16) as u64;
    let mut extents: Vec<Extent> = Vec::new();
    let mut position = header_len;
    for chunk in &chunks {
        offsets[chunk.track][chunk.index] = position;
        match extents.last_mut() {
            Some(last) if last.source + last.len == chunk.source && chunk.len > 0 => last.len += chunk.len,
            _ if chunk.len == 0 => {}
            _ => extents.push(Extent { start: position, source: chunk.source, len: chunk.len }),
        }
        position += chunk.len;
    }
    let moov = rewrite_moov(body, &offsets)?;
    let mut header = Vec::with_capacity(header_len as usize);
    header.extend_from_slice(ftyp);
    header.extend_from_slice(&moov);
    header.extend_from_slice(&1_u32.to_be_bytes());
    header.extend_from_slice(b"mdat");
    header.extend_from_slice(&(16 + position - header_len).to_be_bytes());
    (header.len() as u64 == header_len).then(|| LinearView {
        path: path.to_owned(),
        stamp,
        header,
        extents,
        total: position,
    })
}

struct Chunk {
    time: f64,
    track: usize,
    index: usize,
    source: u64,
    len: u64,
}

/// The `moov` body with each track's chunk offset table replaced by `offsets`.
fn rewrite_moov(body: &[u8], offsets: &[Vec<u64>]) -> Option<Vec<u8>> {
    let mut tracks = offsets.iter();
    let mut out = Vec::with_capacity(body.len() + 64);
    for item in boxes(body)? {
        match &item.kind {
            b"trak" => {
                let table = tracks.next()?;
                out.extend(boxed(b"trak", &rewrite_path(item.body, &[b"mdia", b"minf", b"stbl"], table)?));
            }
            // Fragmented movies keep samples outside this index.
            b"mvex" => return None,
            _ => out.extend_from_slice(item.full),
        }
    }
    tracks.next().is_none().then(|| boxed(b"moov", &out))
}

fn rewrite_path(body: &[u8], path: &[&[u8; 4]], offsets: &[u64]) -> Option<Vec<u8>> {
    let mut out = Vec::with_capacity(body.len() + offsets.len() * 4);
    let mut replaced = false;
    for item in boxes(body)? {
        match path.split_first() {
            Some((next, rest)) if item.kind == **next && !replaced => {
                out.extend(boxed(next, &rewrite_path(item.body, rest, offsets)?));
                replaced = true;
            }
            None if &item.kind == b"stco" || &item.kind == b"co64" => {
                let count = u32_at(item.body, 4)? as usize;
                if replaced || count != offsets.len() {
                    return None;
                }
                let mut table = Vec::with_capacity(8 + offsets.len() * 8);
                table.extend_from_slice(&[0; 4]);
                table.extend_from_slice(&(offsets.len() as u32).to_be_bytes());
                table.extend(offsets.iter().flat_map(|offset| offset.to_be_bytes()));
                out.extend(boxed(b"co64", &table));
                replaced = true;
            }
            _ => out.extend_from_slice(item.full),
        }
    }
    replaced.then_some(out)
}

/// Seconds to add to each track's decode times to get when they play:
/// leading empty edits delay a track, a media start time skips into it.
fn edit_shifts(moov_body: &[u8], track_count: usize) -> Option<Vec<f64>> {
    let items = boxes(moov_body)?;
    let movie_scale = items
        .iter()
        .find(|item| &item.kind == b"mvhd")
        .and_then(|mvhd| u32_at(mvhd.body, if mvhd.body.first() == Some(&1) { 20 } else { 12 }))
        .unwrap_or(1000)
        .max(1) as f64;
    let shifts: Vec<f64> = items
        .iter()
        .filter(|item| &item.kind == b"trak")
        .map(|trak| track_shift(trak.body, movie_scale).unwrap_or(0.0))
        .collect();
    (shifts.len() == track_count).then_some(shifts)
}

fn track_shift(trak: &[u8], movie_scale: f64) -> Option<f64> {
    let mdhd = find(find(trak, b"mdia")?, b"mdhd")?;
    let media_scale = u32_at(mdhd, if mdhd.first() == Some(&1) { 20 } else { 12 })?.max(1) as f64;
    let elst = find(find(trak, b"edts")?, b"elst")?;
    let wide = elst.first() == Some(&1);
    let entry = if wide { 20 } else { 12 };
    let mut delay = 0.0;
    for index in 0..(u32_at(elst, 4)? as usize).min(16) {
        let at = 8 + index * entry;
        let (duration, media_time) = if wide {
            (u64_at(elst, at)? as f64, u64_at(elst, at + 8)? as i64)
        } else {
            (f64::from(u32_at(elst, at)?), i64::from(u32_at(elst, at + 4)? as i32))
        };
        if media_time == -1 {
            delay += duration / movie_scale;
        } else {
            return Some(delay - media_time as f64 / media_scale);
        }
    }
    Some(delay)
}

struct Item<'a> {
    kind: [u8; 4],
    full: &'a [u8],
    body: &'a [u8],
}

/// Every child box of `data`, or `None` when the boxes do not exactly fill it.
fn boxes(data: &[u8]) -> Option<Vec<Item<'_>>> {
    let mut items = Vec::new();
    let mut rest = data;
    while !rest.is_empty() {
        let header = box_header_len(rest)?;
        let size = match u32_at(rest, 0)? {
            1 => usize::try_from(u64_at(rest, 8)?).ok()?,
            0 => rest.len(),
            size => size as usize,
        };
        if size < header || size > rest.len() {
            return None;
        }
        let (full, next) = rest.split_at(size);
        items.push(Item {
            kind: full[4..8].try_into().ok()?,
            full,
            body: &full[header..],
        });
        rest = next;
    }
    Some(items)
}

fn find<'a>(data: &'a [u8], kind: &[u8; 4]) -> Option<&'a [u8]> {
    boxes(data)?.into_iter().find(|item| &item.kind == kind).map(|item| item.body)
}

fn box_header_len(data: &[u8]) -> Option<usize> {
    data.get(4..8)?;
    Some(if u32_at(data, 0)? == 1 { 16 } else { 8 })
}

fn boxed(kind: &[u8; 4], body: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(body.len() + 16);
    match u32::try_from(body.len() + 8) {
        Ok(size) => out.extend_from_slice(&size.to_be_bytes()),
        Err(_) => {
            out.extend_from_slice(&1_u32.to_be_bytes());
            out.extend_from_slice(kind);
            out.extend_from_slice(&(body.len() as u64 + 16).to_be_bytes());
            out.extend_from_slice(body);
            return out;
        }
    }
    out.extend_from_slice(kind);
    out.extend_from_slice(body);
    out
}

fn u32_at(data: &[u8], at: usize) -> Option<u32> {
    Some(u32::from_be_bytes(data.get(at..at + 4)?.try_into().ok()?))
}

fn u64_at(data: &[u8], at: usize) -> Option<u64> {
    Some(u64::from_be_bytes(data.get(at..at + 8)?.try_into().ok()?))
}

fn stamp_of(file: &File) -> io::Result<Stamp> {
    let metadata = file.metadata()?;
    Ok(Stamp {
        len: metadata.len(),
        modified: metadata.modified().ok(),
    })
}

struct TopLevel {
    ftyp: Option<(u64, u64)>,
    moov: Option<(u64, u64)>,
    fragmented: bool,
}

/// Where the top-level `ftyp` and `moov` boxes are; `None` for a file whose
/// box headers do not add up.
fn top_level(file: &mut File, end: u64) -> io::Result<Option<TopLevel>> {
    let mut top = TopLevel { ftyp: None, moov: None, fragmented: false };
    let mut position = 0_u64;
    while position + 8 <= end {
        let mut header = [0_u8; 16];
        file.seek(SeekFrom::Start(position))?;
        file.read_exact(&mut header[..8])?;
        let mut size = u64::from(u32::from_be_bytes(header[..4].try_into().unwrap()));
        let mut header_len = 8;
        if size == 1 {
            file.read_exact(&mut header[8..])?;
            size = u64::from_be_bytes(header[8..].try_into().unwrap());
            header_len = 16;
        } else if size == 0 {
            size = end - position;
        }
        if size < header_len || position + size > end {
            return Ok(None);
        }
        match &header[4..8] {
            b"ftyp" => top.ftyp = Some((position, size)),
            b"moov" => top.moov = Some((position, size)),
            b"moof" => top.fragmented = true,
            _ => {}
        }
        position += size;
    }
    Ok(Some(top))
}

/// The tracks the view's own index describes.
#[cfg(test)]
pub(super) fn layout_of_view(view: &LinearView) -> Option<Vec<super::layout_probe::TrackLayout>> {
    let moov_at = u32_at(&view.header, 0)? as usize;
    let moov_len = u32_at(&view.header, moov_at)? as usize;
    layout_from_moov(view.header.get(moov_at..moov_at + moov_len)?)
}
