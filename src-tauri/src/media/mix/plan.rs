//! Where the re-encoded audio goes, and the index that describes it.
//!
//! Each changed track's new packets fill that track's old packet slots in
//! time order: a slot takes as many new packets as fit, and the rest of it is
//! zeroed. Packets that no slot can take go after the media, which grows to
//! hold them, followed by the new index.
use std::path::{Path, PathBuf};

use super::super::layout_probe::{layout_from_moov, TrackLayout};
use super::super::mp4_box::{boxed, boxes, find, u32_at, u64_at, Item, TopBox};
use super::encode::{edit_entries, media_header, EncodedTrack};
use super::MixError;

/// One of the clip's audio tracks.
#[derive(Clone, Debug)]
pub(super) struct AudioTrack {
    /// Position among the index's tracks.
    pub(super) trak: usize,
    pub(super) layout: TrackLayout,
}

impl AudioTrack {
    pub(super) fn bits_per_second(&self) -> f64 {
        if self.layout.duration_seconds <= 0.0 {
            return 0.0;
        }
        self.layout.payload_bytes() as f64 * 8.0 / self.layout.duration_seconds
    }
}

/// Bytes to put at `at`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(super) struct Write {
    pub(super) at: u64,
    pub(super) bytes: Vec<u8>,
}

/// Everything needed to finish a mix, also after a crash.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(super) struct Plan {
    pub(super) clip: PathBuf,
    pub(super) clip_len: u64,
    pub(super) writes: Vec<Write>,
    pub(super) final_len: u64,
    pub(super) tracks_changed: usize,
}

/// The audio tracks of `moov` (a whole box), in order.
pub(super) fn audio_tracks(moov: &[u8]) -> Result<Vec<AudioTrack>, MixError> {
    let layouts = layout_from_moov(moov).ok_or(MixError::Unsupported("the clip's index is malformed"))?;
    Ok(layouts
        .into_iter()
        .enumerate()
        .filter(|(_, layout)| layout.is_audio())
        .map(|(trak, layout)| AudioTrack { trak, layout })
        .collect())
}

/// The mix plan for a clip whose top-level boxes are `top` and index `moov`;
/// `encoded[n]` is the new audio of track `changed[n].0`.
pub(super) fn plan(
    clip: &Path,
    clip_len: u64,
    top: &[TopBox],
    moov: &[u8],
    tracks: &[AudioTrack],
    changed: &[(usize, f64)],
    encoded: &[EncodedTrack],
) -> Result<Plan, MixError> {
    let unsupported = MixError::Unsupported("this clip's layout cannot be changed in place");
    let position = |kind: &[u8; 4]| top.iter().position(|item| &item.kind == kind);
    let count = |kind: &[u8; 4]| top.iter().filter(|item| &item.kind == kind).count();
    let (Some(mdat_index), Some(moov_index)) = (position(b"mdat"), position(b"moov")) else {
        return Err(unsupported);
    };
    let tail_is_padding = top[mdat_index + 1..]
        .iter()
        .all(|item| matches!(&item.kind, b"moov" | b"free" | b"skip"));
    if count(b"mdat") != 1 || count(b"moov") != 1 || count(b"moof") != 0 || moov_index < mdat_index || !tail_is_padding {
        return Err(unsupported);
    }
    let mdat = top[mdat_index];
    let payload_start = mdat.at + mdat.header_len;
    let payload_end = mdat.at + mdat.size;

    let mut writes = Vec::new();
    let mut overflow = Vec::new();
    let mut new_offsets: Vec<(usize, Vec<u64>)> = Vec::new();
    for ((track, _), new) in changed.iter().zip(encoded) {
        let old = &tracks[*track].layout;
        let mut slots: Vec<(u64, u64)> = Vec::with_capacity(old.offsets.len());
        for (offset, size) in old.offsets.iter().zip(&old.sizes) {
            let end = offset.checked_add(u64::from(*size)).ok_or(MixError::Unsupported("a sample lies outside the clip"))?;
            if *offset < payload_start || end > payload_end {
                return Err(MixError::Unsupported("a sample lies outside the clip's media"));
            }
            if *size > 0 {
                slots.push((*offset, u64::from(*size)));
            }
        }
        let mut contents: Vec<Vec<u8>> = slots.iter().map(|(_, size)| vec![0_u8; *size as usize]).collect();
        let mut offsets = Vec::with_capacity(new.packets.len());
        let (mut slot, mut used) = (0_usize, 0_u64);
        for packet in &new.packets {
            let len = packet.len() as u64;
            while slot < slots.len() && used + len > slots[slot].1 {
                slot += 1;
                used = 0;
            }
            if slot < slots.len() {
                contents[slot][used as usize..(used + len) as usize].copy_from_slice(packet);
                offsets.push(slots[slot].0 + used);
                used += len;
            } else {
                offsets.push(payload_end + overflow.len() as u64);
                overflow.extend_from_slice(packet);
            }
        }
        writes.extend(slots.iter().zip(contents).map(|((at, _), bytes)| Write { at: *at, bytes }));
        new_offsets.push((tracks[*track].trak, offsets));
    }
    // Old packets of different tracks never share bytes; slots must not either.
    let mut spans: Vec<(u64, u64)> = writes.iter().map(|write| (write.at, write.bytes.len() as u64)).collect();
    spans.sort_unstable();
    if spans.windows(2).any(|pair| pair[0].0 + pair[0].1 > pair[1].0) {
        return Err(MixError::Unsupported("samples overlap in the clip"));
    }

    let moov_at = payload_end + overflow.len() as u64;
    if !overflow.is_empty() {
        writes.push(Write { at: payload_end, bytes: overflow });
        let size = moov_at - mdat.at;
        let header = if mdat.header_len == 16 {
            [&1_u32.to_be_bytes()[..], b"mdat", &size.to_be_bytes()].concat()
        } else {
            let size = u32::try_from(size).map_err(|_| MixError::Unsupported("the media box is too large"))?;
            [&size.to_be_bytes()[..], b"mdat"].concat()
        };
        writes.push(Write { at: mdat.at, bytes: header });
    }
    let moov = rebuild_moov(moov, changed, encoded, &new_offsets)?;
    let final_len = moov_at + moov.len() as u64;
    writes.push(Write { at: moov_at, bytes: moov });
    Ok(Plan { clip: clip.to_owned(), clip_len, writes, final_len, tracks_changed: changed.len() })
}

fn rebuild_moov(
    moov: &[u8],
    changed: &[(usize, f64)],
    encoded: &[EncodedTrack],
    new_offsets: &[(usize, Vec<u64>)],
) -> Result<Vec<u8>, MixError> {
    let malformed = || MixError::Unsupported("the clip's index is malformed");
    let header = if u32_at(moov, 0).ok_or_else(malformed)? == 1 { 16 } else { 8 };
    let mut out = Vec::with_capacity(moov.len());
    let mut trak = 0;
    for item in boxes(moov.get(header..).ok_or_else(malformed)?).ok_or_else(malformed)? {
        if &item.kind != b"trak" {
            out.extend_from_slice(item.full);
            continue;
        }
        let index = new_offsets.iter().position(|(position, _)| *position == trak);
        trak += 1;
        match index {
            Some(index) => {
                debug_assert!(index < changed.len());
                out.extend(boxed(b"trak", &rebuild_trak(item.body, &encoded[index], &new_offsets[index].1)?));
            }
            None => out.extend_from_slice(item.full),
        }
    }
    Ok(boxed(b"moov", &out))
}

fn rebuild_trak(trak: &[u8], new: &EncodedTrack, offsets: &[u64]) -> Result<Vec<u8>, MixError> {
    let malformed = || MixError::Unsupported("a track's index is malformed");
    let items = boxes(trak).ok_or_else(malformed)?;
    let presented = items
        .iter()
        .find(|item| &item.kind == b"tkhd")
        .and_then(|tkhd| if tkhd.body.first() == Some(&1) { u64_at(tkhd.body, 28) } else { u32_at(tkhd.body, 20).map(u64::from) })
        .ok_or_else(malformed)?;
    let has_edits = items.iter().any(|item| &item.kind == b"edts");
    let mut out = Vec::with_capacity(trak.len());
    for item in items {
        match &item.kind {
            b"edts" => {
                let elst = find(item.body, b"elst").ok_or_else(malformed)?;
                let entries = edit_entries(elst).ok_or_else(malformed)?;
                let shifted = entries
                    .into_iter()
                    .map(|(duration, time, rate, fraction)| (duration, if time >= 0 { time + new.priming } else { time }, rate, fraction))
                    .collect::<Vec<_>>();
                out.extend(boxed(b"edts", &edit_list(&shifted)));
            }
            b"mdia" => out.extend(boxed(b"mdia", &rebuild_mdia(item.body, new, offsets)?)),
            _ => {
                out.extend_from_slice(item.full);
                // Without an edit list the encoder's priming would play.
                if &item.kind == b"tkhd" && !has_edits && new.priming != 0 {
                    out.extend(boxed(b"edts", &edit_list(&[(presented, new.priming, 1, 0)])));
                }
            }
        }
    }
    Ok(out)
}

fn edit_list(entries: &[(u64, i64, i16, i16)]) -> Vec<u8> {
    let mut body = vec![1_u8, 0, 0, 0];
    body.extend((entries.len() as u32).to_be_bytes());
    for (duration, time, rate, fraction) in entries {
        body.extend(duration.to_be_bytes());
        body.extend(time.to_be_bytes());
        body.extend(rate.to_be_bytes());
        body.extend(fraction.to_be_bytes());
    }
    boxed(b"elst", &body)
}

fn rebuild_mdia(mdia: &[u8], new: &EncodedTrack, offsets: &[u64]) -> Result<Vec<u8>, MixError> {
    let malformed = || MixError::Unsupported("a track's index is malformed");
    let mut out = Vec::with_capacity(mdia.len());
    for item in boxes(mdia).ok_or_else(malformed)? {
        match &item.kind {
            b"mdhd" => out.extend(media_header_with(&item, new)?),
            b"minf" => {
                let mut minf = Vec::with_capacity(item.body.len());
                for child in boxes(item.body).ok_or_else(malformed)? {
                    if &child.kind == b"stbl" {
                        minf.extend(boxed(b"stbl", &sample_table(new, offsets)));
                    } else {
                        minf.extend_from_slice(child.full);
                    }
                }
                out.extend(boxed(b"minf", &minf));
            }
            _ => out.extend_from_slice(item.full),
        }
    }
    Ok(out)
}

/// The `mdhd` box with the new media duration; the timescale must not change.
fn media_header_with(item: &Item<'_>, new: &EncodedTrack) -> Result<Vec<u8>, MixError> {
    let (timescale, _) = media_header(item.body).ok_or(MixError::Unsupported("a track's index is malformed"))?;
    if timescale != new.timescale {
        return Err(MixError::Encode("the re-encoded audio has a different sample rate".into()));
    }
    let mut body = item.body.to_vec();
    if body[0] == 1 {
        body[24..32].copy_from_slice(&new.media_duration.to_be_bytes());
    } else {
        let duration = u32::try_from(new.media_duration).map_err(|_| MixError::Unsupported("the track is too long"))?;
        body[16..20].copy_from_slice(&duration.to_be_bytes());
    }
    Ok(boxed(b"mdhd", &body))
}

/// Description and timing from the encoder; one packet per chunk at `offsets`.
fn sample_table(new: &EncodedTrack, offsets: &[u64]) -> Vec<u8> {
    let mut out = new.description.clone();
    out.extend(boxed(b"stsc", &[0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1]));
    let mut stsz = vec![0_u8; 8];
    stsz.extend((new.packets.len() as u32).to_be_bytes());
    for packet in &new.packets {
        stsz.extend((packet.len() as u32).to_be_bytes());
    }
    out.extend(boxed(b"stsz", &stsz));
    let mut co64 = vec![0_u8; 4];
    co64.extend((offsets.len() as u32).to_be_bytes());
    for offset in offsets {
        co64.extend(offset.to_be_bytes());
    }
    out.extend(boxed(b"co64", &co64));
    out.extend_from_slice(&new.groups);
    out
}
