//! Media that is played from somewhere other than a local, authorized file
//! (for example a clip a friend is streaming). The media domain only sees an
//! opaque session id; the source owns authorization and fetching.
use crate::error::AppResult;

use super::ByteRange;

pub struct RemoteVideoChunk {
    pub range: ByteRange,
    pub bytes: Vec<u8>,
}

/// A complete local copy of remote media, for work that needs a seekable
/// file (mixing every audio track with FFmpeg).
pub struct RemoteMediaFile {
    pub path: std::path::PathBuf,
    pub audio_tracks: Vec<String>,
}

pub trait RemoteMediaSource: Send + Sync {
    /// Called from a protocol worker thread (never an async runtime thread).
    /// Invalid ranges must be reported as `AppError::Path` starting with
    /// "invalid media byte range" so the protocol answers 416.
    fn read_video(
        &self,
        stream_id: &str,
        owner: &str,
        range_header: Option<&str>,
    ) -> AppResult<RemoteVideoChunk>;

    /// The whole stream as a local file, once every byte has arrived.
    fn complete_file(&self, stream_id: &str, owner: &str) -> AppResult<RemoteMediaFile>;

    fn release_owner(&self, owner: &str) -> usize;
}
