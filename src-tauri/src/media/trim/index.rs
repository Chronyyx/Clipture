//! The trimmed clip's index: which samples stay, and the `moov` that
//! describes them. Pure (bytes in, bytes out) so it can be checked without
//! touching a file.
//!
//! Video keeps every frame from the keyframe at or before the cut, so it
//! decodes, and audio keeps one packet before the cut to prime the decoder.
//! Each track's new edit list then starts presentation exactly at the cut, so
//! the result is frame accurate without re-encoding.
use super::super::mp4_box::{box_header_len, boxed, boxes, find, u32_at, u64_at, Item};
use super::TrimError;

/// Sample counts beyond this are refused before allocating (a 10-minute
/// 240 fps clip has well under a million).
const MAXIMUM_SAMPLES: usize = 10_000_000;
/// The shortest result worth keeping.
const MINIMUM_KEPT_SECONDS: f64 = 0.1;

/// What survives a trim, and how to describe it.
pub(super) struct Selection {
    moov_body: Vec<u8>,
    movie_duration: u64,
    tracks: Vec<TrackCut>,
}

/// One track after the cut. Tables hold only kept samples, in decode order.
struct TrackCut {
    /// Source offset of each kept sample in the clip.
    offsets: Vec<u64>,
    sizes: Vec<u32>,
    fixed_size: Option<u32>,
    durations: Vec<u32>,
    /// Composition offsets and the `ctts` version they were written with.
    composition: Option<(u8, Vec<i64>)>,
    /// Kept sync samples, numbered from 1 in the new table.
    sync: Option<Vec<u32>>,
    dependencies: Option<Vec<u8>>,
    descriptions: Vec<u32>,
    /// Movie ticks of nothing before the media starts.
    delay: u64,
    /// Where presentation starts in the new media timeline (media ticks).
    media_time: i64,
    /// Movie ticks of media presented.
    presented: u64,
    media_duration: u64,
}

impl Selection {
    /// Movie length after the trim, in seconds.
    pub(super) fn seconds(&self, movie_scale: u32) -> f64 {
        self.movie_duration as f64 / f64::from(movie_scale.max(1))
    }

    /// Source offset and size of every kept sample, per track.
    pub(super) fn samples(&self) -> impl Iterator<Item = (usize, usize, u64, u32)> + '_ {
        self.tracks.iter().enumerate().flat_map(|(track, cut)| {
            cut.offsets
                .iter()
                .zip(&cut.sizes)
                .enumerate()
                .map(move |(index, (offset, size))| (track, index, *offset, *size))
        })
    }

    pub(super) fn track_sample_counts(&self) -> Vec<usize> {
        self.tracks.iter().map(|track| track.offsets.len()).collect()
    }

    /// The trimmed `moov` box, with each kept sample at `offsets[track][index]`.
    pub(super) fn moov(&self, offsets: &[Vec<u64>]) -> Result<Vec<u8>, TrimError> {
        let mut traks = self.tracks.iter().zip(offsets);
        let mut out = Vec::with_capacity(self.moov_body.len());
        for item in boxes(&self.moov_body).ok_or(TrimError::Unsupported("the clip index is malformed"))? {
            match &item.kind {
                b"mvhd" => out.extend(with_duration(&item, self.movie_duration)?),
                b"trak" => {
                    let (cut, offsets) = traks.next().ok_or(TrimError::Unsupported("track count changed"))?;
                    out.extend(boxed(b"trak", &rebuild_trak(item.body, cut, offsets)?));
                }
                _ => out.extend_from_slice(item.full),
            }
        }
        Ok(boxed(b"moov", &out))
    }
}

/// The samples of `moov` (a whole box) that present `[start, end)` seconds.
pub(super) fn select(moov: &[u8], start: f64, end: f64) -> Result<(Selection, u32), TrimError> {
    let malformed = TrimError::Unsupported("the clip index is malformed");
    let body = moov.get(box_header_len(moov).ok_or(TrimError::Unsupported("the clip index is malformed"))?..).ok_or(malformed)?;
    let items = boxes(body).ok_or(TrimError::Unsupported("the clip index is malformed"))?;
    if items.iter().any(|item| &item.kind == b"mvex") {
        return Err(TrimError::Unsupported("fragmented clips cannot be trimmed in place"));
    }
    let mvhd = items.iter().find(|item| &item.kind == b"mvhd").ok_or(TrimError::Unsupported("the clip has no movie header"))?;
    let (movie_scale, movie_duration) = scale_and_duration(mvhd.body).ok_or(TrimError::Unsupported("the movie header is malformed"))?;
    let ticks = |seconds: f64| (seconds.max(0.0) * f64::from(movie_scale)).round() as u64;
    let (a, b) = (ticks(start).min(movie_duration), ticks(end).min(movie_duration));
    if b <= a || ((b - a) as f64) < MINIMUM_KEPT_SECONDS * f64::from(movie_scale) {
        return Err(TrimError::Range("keep at least a tenth of a second"));
    }
    if a == 0 && b == movie_duration {
        return Err(TrimError::Range("nothing would be removed"));
    }
    let tracks = items
        .iter()
        .filter(|item| &item.kind == b"trak")
        .map(|trak| cut_track(trak.body, movie_scale, a, b))
        .collect::<Result<Vec<_>, _>>()?;
    if tracks.iter().all(|track| track.offsets.is_empty()) {
        return Err(TrimError::Range("nothing plays in that part of the clip"));
    }
    Ok((
        Selection {
            moov_body: body.to_vec(),
            movie_duration: b - a,
            tracks,
        },
        movie_scale,
    ))
}

/// A track as recorded, expanded to one entry per sample.
struct Track {
    handler: [u8; 4],
    media_scale: u64,
    offsets: Vec<u64>,
    sizes: Vec<u32>,
    fixed_size: Option<u32>,
    durations: Vec<u32>,
    composition: Option<(u8, Vec<i64>)>,
    sync: Option<Vec<u32>>,
    dependencies: Option<Vec<u8>>,
    descriptions: Vec<u32>,
    edit: Edit,
}

/// The one shape of edit list this understands: optional leading empty
/// edits (a delay), then one media edit at normal speed; trailing empty
/// edits only pad the track and are dropped.
struct Edit {
    delay: u64,
    /// `None` when the track presents nothing at all.
    media: Option<(i64, u64)>,
}

fn cut_track(trak: &[u8], movie_scale: u32, a: u64, b: u64) -> Result<TrackCut, TrimError> {
    let track = parse_track(trak, movie_scale)?;
    let movie_scale = u64::from(movie_scale);
    let empty = |delay: u64| TrackCut {
        offsets: Vec::new(),
        sizes: Vec::new(),
        fixed_size: track.fixed_size,
        durations: Vec::new(),
        composition: track.composition.as_ref().map(|(version, _)| (*version, Vec::new())),
        sync: track.sync.as_ref().map(|_| Vec::new()),
        dependencies: track.dependencies.as_ref().map(|_| Vec::new()),
        descriptions: Vec::new(),
        delay,
        media_time: -1,
        presented: 0,
        media_duration: 0,
    };
    let Some((media_start, presented)) = track.edit.media else {
        return Ok(empty(b - a));
    };
    let shown_from = a.max(track.edit.delay);
    let shown_to = b.min(track.edit.delay.saturating_add(presented));
    if shown_to <= shown_from {
        return Ok(empty(b - a));
    }
    // Presentation ticks to this track's media ticks.
    let to_media = |movie: u64, round_up: bool| -> i64 {
        let numerator = u128::from(movie - track.edit.delay) * u128::from(track.media_scale);
        let ticks = if round_up { numerator.div_ceil(u128::from(movie_scale)) } else { numerator / u128::from(movie_scale) };
        media_start.saturating_add(ticks as i64)
    };
    let (from, to) = (to_media(shown_from, false), to_media(shown_to, true));

    let mut decode = 0_i64;
    let mut needed = None::<(usize, usize)>;
    for (index, duration) in track.durations.iter().enumerate() {
        let shift = track.composition.as_ref().map_or(0, |(_, offsets)| offsets[index]);
        let presented_at = decode + shift;
        if presented_at < to && presented_at + i64::from(*duration) > from {
            needed = Some(needed.map_or((index, index), |(first, last)| (first.min(index), last.max(index))));
        }
        decode += i64::from(*duration);
    }
    let Some((first_needed, last_needed)) = needed else {
        return Ok(empty(b - a));
    };
    let first = match &track.sync {
        Some(sync) => sync
            .iter()
            .map(|number| *number as usize - 1)
            .filter(|index| *index <= first_needed)
            .max()
            .ok_or(TrimError::Unsupported("no keyframe before the cut"))?,
        // Audio: one packet before the cut primes the decoder (AAC frames
        // overlap), and the edit list hides it.
        None if &track.handler == b"soun" => first_needed.saturating_sub(1),
        None => first_needed,
    };
    let kept = first..last_needed + 1;
    let first_decode: i64 = track.durations[..first].iter().map(|duration| i64::from(*duration)).sum();
    let mut delay = shown_from - a;
    let mut presented = shown_to - shown_from;
    let mut media_time = from - first_decode;
    if media_time < 0 {
        // The kept samples start after the cut: wait for them.
        let wait = (u128::from(media_time.unsigned_abs()) * u128::from(movie_scale)).div_ceil(u128::from(track.media_scale)) as u64;
        delay += wait;
        presented = presented.saturating_sub(wait);
        media_time = 0;
    }
    let durations = track.durations[kept.clone()].to_vec();
    Ok(TrackCut {
        offsets: track.offsets[kept.clone()].to_vec(),
        sizes: track.sizes[kept.clone()].to_vec(),
        fixed_size: track.fixed_size,
        media_duration: durations.iter().map(|duration| u64::from(*duration)).sum(),
        durations,
        composition: track.composition.as_ref().map(|(version, offsets)| (*version, offsets[kept.clone()].to_vec())),
        sync: track.sync.as_ref().map(|sync| {
            sync.iter()
                .map(|number| *number as usize - 1)
                .filter(|index| kept.contains(index))
                .map(|index| (index - first + 1) as u32)
                .collect()
        }),
        dependencies: track.dependencies.as_ref().map(|flags| flags[kept.clone()].to_vec()),
        descriptions: track.descriptions[kept].to_vec(),
        delay,
        media_time,
        presented,
    })
}

fn parse_track(trak: &[u8], movie_scale: u32) -> Result<Track, TrimError> {
    let malformed = || TrimError::Unsupported("a track's index is malformed");
    let mdia = find(trak, b"mdia").ok_or_else(malformed)?;
    let handler: [u8; 4] = find(mdia, b"hdlr").and_then(|hdlr| hdlr.get(8..12)).ok_or_else(malformed)?.try_into().map_err(|_| malformed())?;
    let (media_scale, media_duration) = find(mdia, b"mdhd").and_then(scale_and_duration).ok_or_else(malformed)?;
    let stbl = find(find(mdia, b"minf").ok_or_else(malformed)?, b"stbl").ok_or_else(malformed)?;
    let tables = boxes(stbl).ok_or_else(malformed)?;
    for table in &tables {
        let allowed = matches!(&table.kind, b"stsd" | b"stts" | b"ctts" | b"stss" | b"stsc" | b"stsz" | b"stco" | b"co64" | b"sdtp" | b"cslg" | b"stps")
            || (matches!(&table.kind, b"sgpd" | b"sbgp") && table.body.get(4..8) == Some(b"roll"));
        if !allowed {
            return Err(TrimError::Unsupported("the clip has sample tables this cannot rewrite"));
        }
    }
    let table = |kind: &[u8; 4]| tables.iter().find(|item| &item.kind == kind).map(|item| item.body);

    let stsz = table(b"stsz").ok_or_else(malformed)?;
    let fixed = u32_at(stsz, 4).ok_or_else(malformed)?;
    let count = u32_at(stsz, 8).ok_or_else(malformed)? as usize;
    if count > MAXIMUM_SAMPLES || (fixed == 0 && count > stsz.len().saturating_sub(12) / 4) {
        return Err(malformed());
    }
    let sizes = (0..count)
        .map(|index| if fixed != 0 { Some(fixed) } else { u32_at(stsz, 12 + 4 * index) })
        .collect::<Option<Vec<_>>>()
        .ok_or_else(malformed)?;

    let durations = expand_runs(table(b"stts").ok_or_else(malformed)?, count).ok_or_else(malformed)?;
    let composition = match tables.iter().find(|item| &item.kind == b"ctts") {
        Some(ctts) => {
            let version = *ctts.body.first().ok_or_else(malformed)?;
            let raw = expand_runs(ctts.body, count).ok_or_else(malformed)?;
            Some((version, raw.into_iter().map(|value| if version == 0 { i64::from(value) } else { i64::from(value as i32) }).collect()))
        }
        None => None,
    };
    let sync = match table(b"stss") {
        Some(stss) => {
            let entries = u32_at(stss, 4).ok_or_else(malformed)? as usize;
            if entries > count {
                return Err(malformed());
            }
            let numbers = (0..entries).map(|index| u32_at(stss, 8 + 4 * index)).collect::<Option<Vec<_>>>().ok_or_else(malformed)?;
            if numbers.iter().any(|number| *number == 0 || *number as usize > count) {
                return Err(malformed());
            }
            Some(numbers)
        }
        None => None,
    };
    let dependencies = match table(b"sdtp") {
        Some(sdtp) => Some(sdtp.get(4..4 + count).ok_or_else(malformed)?.to_vec()),
        None => None,
    };
    let chunks: Vec<u64> = if let Some(stco) = table(b"stco") {
        let entries = u32_at(stco, 4).ok_or_else(malformed)? as usize;
        (0..entries.min(MAXIMUM_SAMPLES)).map(|index| u32_at(stco, 8 + 4 * index).map(u64::from)).collect::<Option<_>>().ok_or_else(malformed)?
    } else {
        let co64 = table(b"co64").ok_or_else(malformed)?;
        let entries = u32_at(co64, 4).ok_or_else(malformed)? as usize;
        (0..entries.min(MAXIMUM_SAMPLES)).map(|index| u64_at(co64, 8 + 8 * index)).collect::<Option<_>>().ok_or_else(malformed)?
    };
    let (offsets, descriptions) = sample_places(table(b"stsc").ok_or_else(malformed)?, &chunks, &sizes).ok_or_else(malformed)?;
    let edit = parse_edit(trak, movie_scale, media_scale, media_duration)?;
    Ok(Track {
        handler,
        media_scale: u64::from(media_scale.max(1)),
        offsets,
        sizes,
        fixed_size: (fixed != 0).then_some(fixed),
        durations,
        composition,
        sync,
        dependencies,
        descriptions,
        edit,
    })
}

/// `stts`/`ctts` runs (count, value) expanded to exactly `count` values.
fn expand_runs(table: &[u8], count: usize) -> Option<Vec<u32>> {
    let entries = u32_at(table, 4)? as usize;
    let mut values = Vec::with_capacity(count);
    for entry in 0..entries {
        let run = u32_at(table, 8 + 8 * entry)? as usize;
        let value = u32_at(table, 12 + 8 * entry)?;
        if run > count - values.len() {
            return None;
        }
        values.extend(std::iter::repeat_n(value, run));
    }
    (values.len() == count).then_some(values)
}

/// Each sample's file offset and sample description, from the chunk tables.
fn sample_places(stsc: &[u8], chunks: &[u64], sizes: &[u32]) -> Option<(Vec<u64>, Vec<u32>)> {
    let runs = u32_at(stsc, 4)? as usize;
    let mut offsets = Vec::with_capacity(sizes.len());
    let mut descriptions = Vec::with_capacity(sizes.len());
    for run in 0..runs {
        let first = u32_at(stsc, 8 + 12 * run)? as usize;
        let per_chunk = u32_at(stsc, 12 + 12 * run)? as usize;
        let description = u32_at(stsc, 16 + 12 * run)?;
        let last = if run + 1 < runs { (u32_at(stsc, 8 + 12 * (run + 1))? as usize).checked_sub(1)? } else { chunks.len() };
        for chunk in first..=last {
            let mut offset = *chunks.get(chunk.checked_sub(1)?)?;
            for _ in 0..per_chunk {
                if offsets.len() >= sizes.len() {
                    break;
                }
                offsets.push(offset);
                descriptions.push(description);
                offset = offset.checked_add(u64::from(sizes[offsets.len() - 1]))?;
            }
        }
    }
    (offsets.len() == sizes.len()).then_some((offsets, descriptions))
}

fn parse_edit(trak: &[u8], movie_scale: u32, media_scale: u32, media_duration: u64) -> Result<Edit, TrimError> {
    let unsupported = TrimError::Unsupported("the clip's edit list cannot be trimmed in place");
    let Some(elst) = find(trak, b"edts").and_then(|edts| find(edts, b"elst")) else {
        let presented = (u128::from(media_duration) * u128::from(movie_scale) / u128::from(media_scale.max(1))) as u64;
        return Ok(Edit { delay: 0, media: Some((0, presented)) });
    };
    let wide = elst.first() == Some(&1);
    let entry_len = if wide { 20 } else { 12 };
    let entries = u32_at(elst, 4).ok_or(TrimError::Unsupported("the clip's edit list is malformed"))? as usize;
    let mut delay = 0_u64;
    let mut media = None;
    for index in 0..entries.min(64) {
        let at = 8 + index * entry_len;
        let read = || -> Option<(u64, i64, u32)> {
            Some(if wide {
                (u64_at(elst, at)?, u64_at(elst, at + 8)? as i64, u32_at(elst, at + 16)?)
            } else {
                (u64::from(u32_at(elst, at)?), i64::from(u32_at(elst, at + 4)? as i32), u32_at(elst, at + 8)?)
            })
        };
        let (duration, media_time, rate) = read().ok_or(TrimError::Unsupported("the clip's edit list is malformed"))?;
        match (media_time, media.is_some()) {
            (-1, false) => delay = delay.saturating_add(duration),
            // Trailing empty edits only pad the end.
            (-1, true) => {}
            (time, false) if time >= 0 && rate == 0x0001_0000 => {
                let duration = if duration == 0 {
                    (u128::from(media_duration.saturating_sub(time as u64)) * u128::from(movie_scale) / u128::from(media_scale.max(1))) as u64
                } else {
                    duration
                };
                media = Some((time, duration));
            }
            _ => return Err(unsupported),
        }
    }
    Ok(Edit { delay, media })
}

fn scale_and_duration(header: &[u8]) -> Option<(u32, u64)> {
    if header.first() == Some(&1) {
        Some((u32_at(header, 20)?, u64_at(header, 24)?))
    } else {
        Some((u32_at(header, 12)?, u64::from(u32_at(header, 16)?)))
    }
}

/// `mvhd`/`mdhd`/`tkhd` with a new duration. Durations only shrink, so a
/// 32-bit field always has room.
fn with_duration(item: &Item<'_>, duration: u64) -> Result<Vec<u8>, TrimError> {
    let mut body = item.body.to_vec();
    let wide = body.first() == Some(&1);
    let at = match (&item.kind, wide) {
        (b"tkhd", true) => 28,
        (b"tkhd", false) => 20,
        (_, true) => 24,
        (_, false) => 16,
    };
    let field = body.get_mut(at..at + if wide { 8 } else { 4 }).ok_or(TrimError::Unsupported("a header is malformed"))?;
    if wide {
        field.copy_from_slice(&duration.to_be_bytes());
    } else {
        field.copy_from_slice(&u32::try_from(duration).unwrap_or(u32::MAX).to_be_bytes());
    }
    Ok(boxed(&item.kind, &body))
}

fn rebuild_trak(trak: &[u8], cut: &TrackCut, offsets: &[u64]) -> Result<Vec<u8>, TrimError> {
    let mut out = Vec::with_capacity(trak.len());
    let edits = new_edits(cut);
    let mut wrote_edits = false;
    for item in boxes(trak).ok_or(TrimError::Unsupported("a track's index is malformed"))? {
        match &item.kind {
            b"tkhd" => {
                out.extend(with_duration(&item, cut.delay + cut.presented)?);
                // A track without edits gets them right after its header.
                out.extend(&edits);
                wrote_edits = true;
            }
            b"edts" => {}
            b"mdia" => out.extend(boxed(b"mdia", &rebuild_mdia(item.body, cut, offsets)?)),
            _ => out.extend_from_slice(item.full),
        }
    }
    if !wrote_edits {
        return Err(TrimError::Unsupported("a track has no header"));
    }
    Ok(out)
}

fn new_edits(cut: &TrackCut) -> Vec<u8> {
    let mut entries: Vec<(u64, i64)> = Vec::new();
    if cut.delay > 0 || cut.offsets.is_empty() {
        entries.push((cut.delay, -1));
    }
    if !cut.offsets.is_empty() && cut.presented > 0 {
        entries.push((cut.presented, cut.media_time));
    }
    let mut elst = vec![1, 0, 0, 0];
    elst.extend((entries.len() as u32).to_be_bytes());
    for (duration, media_time) in entries {
        elst.extend(duration.to_be_bytes());
        elst.extend(media_time.to_be_bytes());
        elst.extend(0x0001_0000_u32.to_be_bytes());
    }
    boxed(b"edts", &boxed(b"elst", &elst))
}

fn rebuild_mdia(mdia: &[u8], cut: &TrackCut, offsets: &[u64]) -> Result<Vec<u8>, TrimError> {
    let malformed = || TrimError::Unsupported("a track's index is malformed");
    let mut out = Vec::with_capacity(mdia.len());
    for item in boxes(mdia).ok_or_else(malformed)? {
        match &item.kind {
            b"mdhd" => out.extend(with_duration(&item, cut.media_duration)?),
            b"minf" => {
                let mut minf = Vec::with_capacity(item.body.len());
                for inner in boxes(item.body).ok_or_else(malformed)? {
                    if &inner.kind == b"stbl" {
                        minf.extend(boxed(b"stbl", &rebuild_stbl(inner.body, cut, offsets)?));
                    } else {
                        minf.extend_from_slice(inner.full);
                    }
                }
                out.extend(boxed(b"minf", &minf));
            }
            _ => out.extend_from_slice(item.full),
        }
    }
    Ok(out)
}

fn rebuild_stbl(stbl: &[u8], cut: &TrackCut, offsets: &[u64]) -> Result<Vec<u8>, TrimError> {
    let stsd = find(stbl, b"stsd").ok_or(TrimError::Unsupported("a track has no sample descriptions"))?;
    let full = |values: &[u8]| [&[0_u8, 0, 0, 0][..], values].concat();
    let mut out = boxed(b"stsd", stsd);
    out.extend(boxed(b"stts", &full(&run_table(cut.durations.iter().map(|value| *value)))));
    if let Some((version, composition)) = &cut.composition {
        let mut ctts = vec![*version, 0, 0, 0];
        ctts.extend(run_table(composition.iter().map(|value| *value as u32)));
        out.extend(boxed(b"ctts", &ctts));
    }
    if let Some(sync) = &cut.sync {
        let mut stss = (sync.len() as u32).to_be_bytes().to_vec();
        stss.extend(sync.iter().flat_map(|number| number.to_be_bytes()));
        out.extend(boxed(b"stss", &full(&stss)));
    }
    if let Some(dependencies) = &cut.dependencies {
        out.extend(boxed(b"sdtp", &full(dependencies)));
    }
    // One sample per chunk; a new run starts wherever the description changes.
    let mut stsc = Vec::new();
    let mut runs = 0_u32;
    for (index, description) in cut.descriptions.iter().enumerate() {
        if index == 0 || cut.descriptions[index - 1] != *description {
            stsc.extend(((index + 1) as u32).to_be_bytes());
            stsc.extend(1_u32.to_be_bytes());
            stsc.extend(description.to_be_bytes());
            runs += 1;
        }
    }
    out.extend(boxed(b"stsc", &full(&[runs.to_be_bytes().to_vec(), stsc].concat())));
    let mut stsz = cut.fixed_size.unwrap_or(0).to_be_bytes().to_vec();
    stsz.extend((cut.sizes.len() as u32).to_be_bytes());
    if cut.fixed_size.is_none() {
        stsz.extend(cut.sizes.iter().flat_map(|size| size.to_be_bytes()));
    }
    out.extend(boxed(b"stsz", &full(&stsz)));
    if offsets.len() != cut.sizes.len() {
        return Err(TrimError::Unsupported("sample count changed"));
    }
    let mut co64 = (offsets.len() as u32).to_be_bytes().to_vec();
    co64.extend(offsets.iter().flat_map(|offset| offset.to_be_bytes()));
    out.extend(boxed(b"co64", &full(&co64)));
    Ok(out)
}

/// Run-length (count, value) pairs, prefixed with the number of runs.
fn run_table(values: impl Iterator<Item = u32>) -> Vec<u8> {
    let mut runs: Vec<(u32, u32)> = Vec::new();
    for value in values {
        match runs.last_mut() {
            Some((count, last)) if *last == value => *count += 1,
            _ => runs.push((1, value)),
        }
    }
    let mut out = (runs.len() as u32).to_be_bytes().to_vec();
    for (count, value) in runs {
        out.extend(count.to_be_bytes());
        out.extend(value.to_be_bytes());
    }
    out
}
