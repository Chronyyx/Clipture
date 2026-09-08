use std::{ffi::OsString, path::Path};

use crate::{
    error::{AppError, AppResult},
    media::{FfmpegExecutor, FfmpegJob},
};

const MAX_PCM_BYTES: usize = 16 * 1024 * 1024;

pub struct DecodedWave(Vec<u8>);
impl DecodedWave {
    pub fn as_bytes(&self) -> &[u8] {
        &self.0
    }
}

/// Decode every supported sound through the bundled codec implementation rather
/// than optional Windows codecs. No decoder process or PCM cache stays resident.
pub(super) fn decode_wave(ffmpeg: &dyn FfmpegExecutor, path: &Path) -> AppResult<DecodedWave> {
    let format = path
        .extension()
        .and_then(|s| s.to_str())
        .unwrap_or("")
        .to_ascii_lowercase();
    if !matches!(format.as_str(), "mp3" | "wav" | "ogg") {
        return Err(AppError::Path("Unsupported clip sound format".into()));
    }
    let mut args: Vec<OsString> = [
        "-nostdin",
        "-hide_banner",
        "-loglevel",
        "error",
        "-protocol_whitelist",
        "file,pipe",
        "-f",
        &format,
        "-i",
    ]
    .into_iter()
    .map(Into::into)
    .collect();
    args.push(path.as_os_str().to_owned());
    args.extend(
        [
            "-map",
            "0:a:0",
            "-vn",
            "-sn",
            "-dn",
            "-ac",
            "2",
            "-ar",
            "48000",
            "-c:a",
            "pcm_s16le",
            "-f",
            "s16le",
            "pipe:1",
        ]
        .into_iter()
        .map(OsString::from),
    );
    let mut job = FfmpegJob::new("decode clip sound", args);
    job.maximum_stdout_bytes = MAX_PCM_BYTES;
    let output = ffmpeg.run(job)?;
    if !output.success {
        return Err(AppError::Integration(format!(
            "Could not decode clip sound: {}",
            output.stderr
        )));
    }
    wave_from_pcm(output.stdout)
}

fn wave_from_pcm(mut pcm: Vec<u8>) -> AppResult<DecodedWave> {
    if pcm.is_empty() || pcm.len() % 4 != 0 || pcm.len() > MAX_PCM_BYTES {
        return Err(AppError::Integration(
            "Clip sound PCM is empty, unaligned, or exceeds 16 MiB".into(),
        ));
    }
    // Build a known-size PCM header ourselves. FFmpeg's streaming WAV header
    // has unknown lengths and is unsuitable for a memory-only native API.
    let mut header = Vec::with_capacity(44);
    header.extend_from_slice(b"RIFF");
    header.extend_from_slice(&(pcm.len() as u32 + 36).to_le_bytes());
    header.extend_from_slice(b"WAVEfmt ");
    header.extend_from_slice(&16_u32.to_le_bytes());
    header.extend_from_slice(&1_u16.to_le_bytes());
    header.extend_from_slice(&2_u16.to_le_bytes());
    header.extend_from_slice(&48000_u32.to_le_bytes());
    header.extend_from_slice(&192000_u32.to_le_bytes());
    header.extend_from_slice(&4_u16.to_le_bytes());
    header.extend_from_slice(&16_u16.to_le_bytes());
    header.extend_from_slice(b"data");
    header.extend_from_slice(&(pcm.len() as u32).to_le_bytes());
    pcm.splice(0..0, header);
    Ok(DecodedWave(pcm))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn native_wave_has_exact_bounded_lengths() {
        let wave = wave_from_pcm(vec![0; 19200]).unwrap();
        let wave = wave.as_bytes();
        assert_eq!(wave.len(), 19244);
        assert_eq!(&wave[..4], b"RIFF");
        assert_eq!(u32::from_le_bytes(wave[4..8].try_into().unwrap()), 19236);
        assert_eq!(u32::from_le_bytes(wave[40..44].try_into().unwrap()), 19200);
        assert!(wave_from_pcm(vec![]).is_err());
        assert!(wave_from_pcm(vec![0; 3]).is_err());
        assert!(wave_from_pcm(vec![0; MAX_PCM_BYTES + 4]).is_err());
    }
}
