//! Re-encodes the changed tracks with FFmpeg into a small audio-only MP4 and
//! reads back what the clip needs from it: each packet, the sample
//! description and timing, and the encoder's priming.
use std::{ffi::OsString, path::Path, time::Duration};

use super::super::layout_probe::layout_from_moov;
use super::super::mp4_box::{boxes, find, u32_at, u64_at};
use super::super::{library_input, FfmpegExecutor, FfmpegJob};
use super::plan::AudioTrack;
use super::MixError;

/// The audio of even a long clip is small; anything larger is refused.
const MAXIMUM_ENCODED_BYTES: u64 = 512 * 1024 * 1024;
/// New packets must fit the old ones' slots, so the encode aims a little
/// under the track's own bitrate.
const BITRATE_FRACTION: f64 = 0.9;
const MINIMUM_BITRATE: u32 = 32_000;

/// One re-encoded track.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(super) struct EncodedTrack {
    pub(super) packets: Vec<Vec<u8>>,
    /// `stsd` and `stts`, whole boxes, in that order.
    pub(super) description: Vec<u8>,
    /// Sample groups (`sgpd`/`sbgp`) the encoder wrote, whole boxes.
    pub(super) groups: Vec<u8>,
    pub(super) timescale: u32,
    pub(super) media_duration: u64,
    /// Media ticks of encoder delay before the first real sample.
    pub(super) priming: i64,
}

/// Encodes `changed` (track, gain) pairs of the clip at `clip` into `out`.
pub(super) fn encode(
    ffmpeg: &dyn FfmpegExecutor,
    clip: &Path,
    out: &Path,
    tracks: &[AudioTrack],
    changed: &[(usize, f64)],
) -> Result<Vec<EncodedTrack>, MixError> {
    let filters: Vec<String> = changed
        .iter()
        .enumerate()
        .map(|(output, (track, gain))| format!("[0:a:{track}]volume={gain:.4}:precision=float[m{output}]"))
        .collect();
    let mut args: Vec<OsString> = ["-nostdin", "-hide_banner", "-loglevel", "error", "-ignore_editlist", "1"]
        .into_iter()
        .map(OsString::from)
        .collect();
    // Decoded from the first media sample, edit lists ignored, so the new
    // media timeline is the old one shifted by the encoder's priming.
    args.extend(library_input(clip));
    args.extend([OsString::from("-filter_complex"), OsString::from(filters.join(";"))]);
    for (output, (track, _)) in changed.iter().enumerate() {
        let bitrate = ((tracks[*track].bits_per_second() * BITRATE_FRACTION) as u32).max(MINIMUM_BITRATE);
        args.extend([
            OsString::from("-map"),
            OsString::from(format!("[m{output}]")),
            OsString::from(format!("-c:a:{output}")),
            OsString::from("aac"),
            OsString::from(format!("-b:a:{output}")),
            OsString::from(bitrate.to_string()),
        ]);
    }
    args.extend(["-map_metadata", "-1", "-map_chapters", "-1", "-vn", "-sn", "-dn", "-f", "mp4"].map(OsString::from));
    args.push(out.as_os_str().to_owned());

    let mut job = FfmpegJob::new("audio mix", args);
    job.timeout = Duration::from_secs(180);
    let output = ffmpeg.run(job).map_err(|error| MixError::Encode(error.to_string()))?;
    if !output.success {
        return Err(MixError::Encode(if output.stderr.is_empty() {
            format!("FFmpeg exited with code {:?}", output.exit_code)
        } else {
            output.stderr
        }));
    }
    if std::fs::metadata(out)?.len() > MAXIMUM_ENCODED_BYTES {
        return Err(MixError::Unsupported("the re-encoded audio is too large"));
    }
    let tracks = read_encoded(&std::fs::read(out)?).ok_or(MixError::Encode("FFmpeg wrote an unreadable file".into()))?;
    if tracks.len() != changed.len() {
        return Err(MixError::Encode("FFmpeg wrote the wrong number of tracks".into()));
    }
    Ok(tracks)
}

/// Every track of an encoded audio file, in order.
pub(super) fn read_encoded(file: &[u8]) -> Option<Vec<EncodedTrack>> {
    let top = boxes(file)?;
    let moov = top.iter().find(|item| &item.kind == b"moov")?;
    let layouts = layout_from_moov(moov.full)?;
    let traks: Vec<&[u8]> = boxes(moov.body)?.into_iter().filter(|item| &item.kind == b"trak").map(|item| item.body).collect();
    if traks.len() != layouts.len() {
        return None;
    }
    traks
        .into_iter()
        .zip(layouts)
        .map(|(trak, layout)| {
            if !layout.is_audio() {
                return None;
            }
            let mdia = find(trak, b"mdia")?;
            let (timescale, media_duration) = media_header(find(mdia, b"mdhd")?)?;
            let stbl = find(find(mdia, b"minf")?, b"stbl")?;
            let mut description = Vec::new();
            let mut groups = Vec::new();
            for item in boxes(stbl)? {
                match &item.kind {
                    b"stsd" | b"stts" => description.extend_from_slice(item.full),
                    b"sgpd" | b"sbgp" => groups.extend_from_slice(item.full),
                    // Composition offsets would need a matching edit; AAC has none.
                    b"ctts" => return None,
                    _ => {}
                }
            }
            let packets = layout
                .offsets
                .iter()
                .zip(&layout.sizes)
                .map(|(offset, size)| {
                    let start = usize::try_from(*offset).ok()?;
                    file.get(start..start.checked_add(*size as usize)?).map(<[u8]>::to_vec)
                })
                .collect::<Option<Vec<_>>>()?;
            let priming = find(trak, b"edts")
                .and_then(|edts| find(edts, b"elst"))
                .and_then(|elst| edit_entries(elst))
                .and_then(|entries| entries.into_iter().find(|entry| entry.1 >= 0))
                .map_or(0, |entry| entry.1);
            Some(EncodedTrack { packets, description, groups, timescale, media_duration, priming })
        })
        .collect()
}

/// Timescale and duration from an `mdhd` body.
pub(super) fn media_header(mdhd: &[u8]) -> Option<(u32, u64)> {
    if mdhd.first()? == &1 {
        Some((u32_at(mdhd, 20)?, u64_at(mdhd, 24)?))
    } else {
        Some((u32_at(mdhd, 12)?, u64::from(u32_at(mdhd, 16)?)))
    }
}

/// Edit list entries (duration, media time, rate integer, rate fraction).
pub(super) fn edit_entries(elst: &[u8]) -> Option<Vec<(u64, i64, i16, i16)>> {
    let wide = elst.first()? == &1;
    let count = u32_at(elst, 4)? as usize;
    let width = if wide { 20 } else { 12 };
    if count > elst.len().saturating_sub(8) / width {
        return None;
    }
    (0..count)
        .map(|index| {
            let at = 8 + index * width;
            let entry = elst.get(at..at + width)?;
            let rate = |at: usize| Some(i16::from_be_bytes(entry.get(at..at + 2)?.try_into().ok()?));
            if wide {
                Some((u64_at(entry, 0)?, u64_at(entry, 8)? as i64, rate(16)?, rate(18)?))
            } else {
                Some((u64::from(u32_at(entry, 0)?), i64::from(u32_at(entry, 4)? as i32), rate(8)?, rate(10)?))
            }
        })
        .collect()
}
