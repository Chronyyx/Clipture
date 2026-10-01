mod base64;
mod ffmpeg;
mod layout_probe;
mod layout_repair;
mod playback;
mod protocol;
mod range;
mod remote;
mod save_audio;
mod save_inputs;
mod save_jobs;
#[cfg(test)]
mod save_native_tests;
mod save_processor;
#[cfg(test)]
mod save_processor_tests;
mod save_resolution;
mod segment_audio;
mod sessions;
#[cfg(test)]
mod sparse_audio_native_tests;
mod thumbnails;
mod timeline;

pub(crate) use base64::encode as encode_base64;
pub use ffmpeg::{
    AlwaysReady, CommandFfmpeg, FfmpegExecutor, FfmpegJob, FfmpegOutput, WorkScheduler,
};
pub(crate) use ffmpeg::library_input;
pub use layout_probe::layout_from_moov;
pub use layout_repair::{ClipLayoutRepair, RepairCandidate};
pub use playback::{MediaService, VideoStreamPlan};
pub(crate) use protocol::handle_request as handle_protocol_request;
pub(crate) use protocol::MediaAdmission;
pub use protocol::register as register_protocol;
pub use range::{resolve_range, ByteRange, RangeError};
pub use remote::{RemoteMediaFile, RemoteMediaSource, RemoteVideoChunk};
pub use save_processor::MediaSaveProcessor;
pub(crate) use sessions::random_token;
pub use sessions::{MediaSessionRegistry, PlaybackDescriptor, ResolvedSession};
pub use thumbnails::ThumbnailService;
pub use timeline::{apply_patches, audio_edit_list_patches, audio_track_count, PlaybackPatch};
