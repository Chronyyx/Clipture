//! Playback order: which blocks of an MP4 the player needs, in the order it
//! needs them. Clipture's in-place recordings are not stored in time order
//! (frames land in reused slots, with zero padding between), so fetching
//! front to back makes the player wait for bytes far into the file. Once the
//! index (`moov`) has arrived, its sample tables say where every moment of
//! every track lives. Clips shared by current builds are sent as a remuxed,
//! time-ordered copy, so this mostly matters for clips from older senders.
use std::collections::HashMap;

/// Blocks in the order playback first needs them, and each block's place.
#[derive(Debug, Default)]
pub(super) struct PlaybackOrder {
    pub(super) blocks: Vec<u64>,
    pub(super) rank: HashMap<u64, usize>,
}

/// Index sizes beyond this are not parsed.
const MAXIMUM_MOOV_BYTES: u64 = 64 * 1024 * 1024;

#[derive(Debug, PartialEq)]
pub(super) enum Lookup<T> {
    Found(T),
    /// Needs bytes that have not arrived yet; try again later.
    Pending,
    /// Not an MP4 this module understands.
    Absent,
}

/// Where the top-level `moov` box is, walking box headers with `read`
/// (which returns `None` for bytes that have not arrived yet).
pub(super) fn find_moov(
    size: u64,
    mut read: impl FnMut(u64, usize) -> Option<Vec<u8>>,
) -> Lookup<(u64, u64)> {
    let mut offset = 0_u64;
    for _ in 0..64 {
        if offset + 8 > size {
            return Lookup::Absent;
        }
        let Some(header) = read(offset, (size - offset).min(16) as usize) else {
            return Lookup::Pending;
        };
        let small = u32::from_be_bytes(header[0..4].try_into().expect("four bytes")) as u64;
        let length = match small {
            0 => size - offset,
            1 if header.len() >= 16 => u64::from_be_bytes(header[8..16].try_into().expect("eight bytes")),
            1 => return Lookup::Absent,
            length => length,
        };
        if length < 8 || offset.saturating_add(length) > size {
            return Lookup::Absent;
        }
        if &header[4..8] == b"moov" {
            return if length <= MAXIMUM_MOOV_BYTES {
                Lookup::Found((offset, length))
            } else {
                Lookup::Absent
            };
        }
        offset += length;
    }
    Lookup::Absent
}

/// The playback order of a whole `moov` box over `block_bytes` blocks.
pub(super) fn playback_order(moov: &[u8], block_bytes: u64) -> Option<PlaybackOrder> {
    let tracks = crate::media::layout_from_moov(moov)?;
    let mut samples: Vec<(f64, u64, u64)> = tracks
        .iter()
        .flat_map(|track| {
            track
                .times
                .iter()
                .zip(&track.offsets)
                .zip(&track.sizes)
                .map(|((time, offset), size)| (*time, *offset, u64::from(*size)))
        })
        .filter(|(_, _, size)| *size > 0)
        .collect();
    if samples.is_empty() {
        return None;
    }
    samples.sort_by(|a, b| a.0.total_cmp(&b.0).then(a.1.cmp(&b.1)));
    let mut order = PlaybackOrder::default();
    for (_, offset, size) in samples {
        let first = offset / block_bytes;
        let last = offset.saturating_add(size - 1) / block_bytes;
        for block in first..=last {
            order.rank.entry(block).or_insert_with(|| {
                order.blocks.push(block);
                order.blocks.len() - 1
            });
        }
    }
    Some(order)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn wrap(kind: &[u8; 4], body: &[u8]) -> Vec<u8> {
        let mut out = ((body.len() + 8) as u32).to_be_bytes().to_vec();
        out.extend_from_slice(kind);
        out.extend_from_slice(body);
        out
    }

    fn words(values: &[u32]) -> Vec<u8> {
        values.iter().flat_map(|value| value.to_be_bytes()).collect()
    }

    /// One sample per chunk, `size` bytes each, one per second, at `offsets`.
    fn track(handler: &[u8; 4], offsets: &[u32], size: u32) -> Vec<u8> {
        let n = offsets.len() as u32;
        let mut stco = words(&[0, n]);
        stco.extend(words(offsets));
        let stbl = [
            wrap(b"stts", &words(&[0, 1, n, 1000])),
            wrap(b"stsz", &words(&[0, size, n])),
            wrap(b"stsc", &words(&[0, 1, 1, 1, 1])),
            wrap(b"stco", &stco),
        ]
        .concat();
        let mut hdlr = words(&[0, 0]);
        hdlr.extend_from_slice(handler);
        let mdia = [
            wrap(b"mdhd", &words(&[0, 0, 0, 1000, n * 1000])),
            wrap(b"hdlr", &hdlr),
            wrap(b"minf", &wrap(b"stbl", &stbl)),
        ]
        .concat();
        wrap(b"trak", &wrap(b"mdia", &mdia))
    }

    #[test]
    fn out_of_order_frames_are_fetched_in_time_order() {
        // Video seconds 0, 1, 2 live at blocks 5, 2, 9; audio at block 7.
        let index = wrap(
            b"moov",
            &[track(b"vide", &[5000, 2000, 9000], 500), track(b"soun", &[7000, 7500, 7600], 100)].concat(),
        );
        let order = playback_order(&index, 1000).unwrap();
        assert_eq!(order.blocks, [5, 7, 2, 9]);
        assert_eq!(order.rank[&2], 2);
    }

    #[test]
    fn the_index_is_found_after_the_media() {
        let mut file = wrap(b"ftyp", b"isomxxxx");
        file.extend(wrap(b"mdat", &[1; 50]));
        let at = file.len() as u64;
        file.extend(wrap(b"moov", &track(b"vide", &[20], 10)));
        let size = file.len() as u64;
        let read = |offset: u64, length: usize| Some(file[offset as usize..offset as usize + length].to_vec());
        assert_eq!(find_moov(size, read), Lookup::Found((at, size - at)));
        assert_eq!(find_moov(size, |_, _| None), Lookup::Pending);
        assert_eq!(find_moov(size, |_, length| Some(vec![0; length])), Lookup::Absent);
    }

    #[test]
    fn garbage_is_rejected_not_trusted() {
        assert!(playback_order(&wrap(b"free", &[0; 32]), 1000).is_none());
        let mut truncated = wrap(b"moov", &track(b"vide", &[1, 2, 3], 4));
        truncated.truncate(truncated.len() - 6);
        assert!(playback_order(&truncated, 1000).is_none());
    }
}
