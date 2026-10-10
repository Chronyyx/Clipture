//! Where each kept byte goes. Kept samples slide down over the removed ones
//! in their on-disk order, so every move goes to a lower offset and never
//! onto a kept byte that has not moved yet. The file then ends with the new
//! index and is cut to length.
use super::index::Selection;
use super::super::mp4_box::TopBox;
use super::TrimError;

/// Bytes moved per journaled step.
pub(super) const BLOCK_BYTES: u64 = 16 * 1024 * 1024;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) struct Run {
    pub(super) src: u64,
    pub(super) dst: u64,
    pub(super) len: u64,
}

/// Everything needed to finish a trim, also after a crash.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(super) struct Plan {
    pub(super) clip: std::path::PathBuf,
    pub(super) clip_len: u64,
    pub(super) block_bytes: u64,
    pub(super) runs: Vec<Run>,
    pub(super) mdat_header_at: u64,
    pub(super) mdat_header: Vec<u8>,
    pub(super) moov_at: u64,
    pub(super) moov: Vec<u8>,
    pub(super) final_len: u64,
    /// The trimmed length, in milliseconds, for the library.
    pub(super) duration_ms: u64,
}

impl Plan {
    /// The moves in execution order, each at most `block_bytes` long.
    pub(super) fn blocks(&self) -> Vec<Run> {
        let mut blocks = Vec::new();
        for run in self.runs.iter().filter(|run| run.src != run.dst) {
            let mut done = 0;
            while done < run.len {
                let len = self.block_bytes.min(run.len - done);
                blocks.push(Run { src: run.src + done, dst: run.dst + done, len });
                done += len;
            }
        }
        blocks
    }
}

/// The compaction plan for a clip whose top-level boxes are `top`.
pub(super) fn plan(
    clip: &std::path::Path,
    clip_len: u64,
    top: &[TopBox],
    selection: &Selection,
    movie_scale: u32,
) -> Result<Plan, TrimError> {
    let unsupported = TrimError::Unsupported("this clip's layout cannot be trimmed in place");
    let position = |kind: &[u8; 4]| top.iter().position(|item| &item.kind == kind);
    let count = |kind: &[u8; 4]| top.iter().filter(|item| &item.kind == kind).count();
    let (Some(mdat_index), Some(moov_index)) = (position(b"mdat"), position(b"moov")) else {
        return Err(unsupported);
    };
    // One media box, the index after it, and only padding after the media.
    let tail_is_padding = top[mdat_index + 1..]
        .iter()
        .all(|item| matches!(&item.kind, b"moov" | b"free" | b"skip"));
    if count(b"mdat") != 1 || count(b"moov") != 1 || count(b"moof") != 0 || moov_index < mdat_index || !tail_is_padding {
        return Err(unsupported);
    }
    let mdat = top[mdat_index];
    let payload_start = mdat.at + mdat.header_len;
    let payload_end = mdat.at + mdat.size;

    let mut samples: Vec<Kept> = selection
        .samples()
        .map(|(track, index, offset, size)| Kept { offset, size: u64::from(size), track, index })
        .collect();
    samples.sort_unstable_by_key(|sample| (sample.offset, sample.track, sample.index));
    let mut previous_end = payload_start;
    for sample in &samples {
        let end = sample.offset.checked_add(sample.size).ok_or(TrimError::Unsupported("a sample lies outside the clip"))?;
        if sample.offset < payload_start || end > payload_end {
            return Err(TrimError::Unsupported("a sample lies outside the clip's media"));
        }
        if sample.offset < previous_end {
            return Err(TrimError::Unsupported("samples overlap in the clip"));
        }
        previous_end = end;
    }
    let (runs, offsets, kept) = slide_down(&samples, &selection.track_sample_counts(), payload_start);
    let mdat_header = if mdat.header_len == 16 {
        let mut header = 1_u32.to_be_bytes().to_vec();
        header.extend_from_slice(b"mdat");
        header.extend((16 + kept).to_be_bytes());
        header
    } else {
        let size = u32::try_from(8 + kept).map_err(|_| TrimError::Unsupported("the media box is too large"))?;
        [size.to_be_bytes().as_slice(), b"mdat"].concat()
    };
    let moov = selection.moov(&offsets)?;
    let moov_at = payload_start + kept;
    Ok(Plan {
        clip: clip.to_owned(),
        clip_len,
        block_bytes: BLOCK_BYTES,
        runs,
        mdat_header_at: mdat.at,
        mdat_header,
        final_len: moov_at + moov.len() as u64,
        moov_at,
        moov,
        duration_ms: (selection.seconds(movie_scale) * 1000.0).round() as u64,
    })
}

/// One kept sample where it is now.
#[derive(Clone, Copy)]
struct Kept {
    offset: u64,
    size: u64,
    track: usize,
    index: usize,
}

/// Kept samples in their current order, slid down over the removed ones:
/// the moves in order, each sample's new offset, and the media bytes kept.
/// Every move goes to a lower offset, so the file never grows. The recorder
/// stores footage in recording order, so the trimmed clip keeps it too.
fn slide_down(samples: &[Kept], counts: &[usize], payload_start: u64) -> (Vec<Run>, Vec<Vec<u64>>, u64) {
    let mut offsets: Vec<Vec<u64>> = counts.iter().map(|count| vec![0; *count]).collect();
    let mut runs: Vec<Run> = Vec::new();
    let mut cursor = payload_start;
    for sample in samples {
        match runs.last_mut() {
            Some(run) if run.src + run.len == sample.offset => run.len += sample.size,
            _ if sample.size == 0 => {}
            _ => runs.push(Run { src: sample.offset, dst: cursor, len: sample.size }),
        }
        offsets[sample.track][sample.index] = cursor;
        cursor += sample.size;
    }
    (runs, offsets, cursor - payload_start)
}
