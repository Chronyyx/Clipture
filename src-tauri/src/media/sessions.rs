use std::{
    collections::HashMap,
    fs,
    hash::{Hash, Hasher},
    path::{Path, PathBuf},
    sync::{
        atomic::{AtomicU64, Ordering},
        Mutex,
    },
    time::{Duration, Instant, SystemTime},
};

use serde::Serialize;

use crate::{
    clips::PathAuthorizer,
    error::{AppError, AppResult},
};

use super::PlaybackPatch;

const DEFAULT_SESSION_TTL: Duration = Duration::from_secs(15 * 60);
const DEFAULT_SESSION_LIMIT: usize = 32;
const AUDIO_CHUNK_SECONDS: u32 = 8;

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PlaybackDescriptor {
    pub session_id: String,
    pub url: String,
    pub mixed: bool,
    pub message: String,
    pub audio_chunk_url: Option<String>,
    pub audio_chunk_seconds: Option<u32>,
}

#[derive(Clone, Debug)]
pub struct ResolvedSession {
    pub id: String,
    pub clip_id: String,
    pub path: PathBuf,
    pub content_type: &'static str,
    pub selected_audio_indexes: Vec<u8>,
    pub playback_patches: Vec<PlaybackPatch>,
}

#[derive(Clone, Debug)]
struct SessionEntry {
    clip_id: String,
    path: PathBuf,
    selected_audio_indexes: Vec<u8>,
    playback_patches: Vec<PlaybackPatch>,
    owner: String,
    touched_at: Instant,
}

pub struct MediaSessionRegistry {
    endpoint: String,
    ttl: Duration,
    limit: usize,
    sessions: Mutex<HashMap<String, SessionEntry>>,
}

impl MediaSessionRegistry {
    pub fn new(endpoint: impl Into<String>) -> AppResult<Self> {
        Self::with_policy(endpoint, DEFAULT_SESSION_TTL, DEFAULT_SESSION_LIMIT)
    }

    pub fn with_policy(
        endpoint: impl Into<String>,
        ttl: Duration,
        limit: usize,
    ) -> AppResult<Self> {
        let endpoint = endpoint.into().trim_end_matches('/').to_owned();
        if !safe_endpoint(&endpoint) {
            return Err(AppError::Path(
                "media endpoint must be loopback HTTP or clipture-media://localhost".into(),
            ));
        }
        Ok(Self {
            endpoint,
            ttl: ttl.max(Duration::from_secs(1)),
            limit: limit.max(1),
            sessions: Mutex::new(HashMap::new()),
        })
    }

    pub fn open(
        &self,
        authority: &PathAuthorizer,
        clip_id: &str,
        audio_tracks: &[String],
        playback_patches: Vec<PlaybackPatch>,
        owner: &str,
    ) -> AppResult<PlaybackDescriptor> {
        let path = authority
            .primary(clip_id)
            .ok_or_else(|| AppError::Path("clip is not in the authorized library snapshot".into()))?
            .to_owned();
        validate_unchanged_regular_file(&path, &path)?;
        let indexes = selected_audio_indexes(audio_tracks);
        let mixed = indexes.len() > 1 || (indexes.len() == 1 && !playback_patches.is_empty());
        let id = random_token();
        let mut sessions = self
            .sessions
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        purge_expired(&mut sessions, self.ttl);
        while sessions.len() >= self.limit {
            let Some(oldest) = sessions
                .iter()
                .min_by_key(|(_, session)| session.touched_at)
                .map(|(id, _)| id.clone())
            else {
                break;
            };
            sessions.remove(&oldest);
        }
        sessions.insert(
            id.clone(),
            SessionEntry {
                clip_id: clip_id.into(),
                path,
                selected_audio_indexes: indexes,
                playback_patches,
                owner: owner.into(),
                touched_at: Instant::now(),
            },
        );
        let base = format!("{}/v1/session/{id}", self.endpoint);
        Ok(PlaybackDescriptor {
            session_id: id,
            url: format!("{base}/video"),
            mixed,
            message: if mixed {
                "Streaming video with rolling mixed audio buffer.".into()
            } else {
                "Playing original clip with range buffering.".into()
            },
            audio_chunk_url: mixed.then(|| format!("{base}/audio")),
            audio_chunk_seconds: mixed.then_some(AUDIO_CHUNK_SECONDS),
        })
    }

    pub fn resolve(&self, id: &str, owner: &str) -> AppResult<ResolvedSession> {
        let mut sessions = self
            .sessions
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        purge_expired(&mut sessions, self.ttl);
        let entry = sessions
            .get_mut(id)
            .ok_or_else(|| AppError::Path("media session is missing or expired".into()))?;
        if entry.owner != owner {
            return Err(AppError::Path(
                "media session does not belong to this WebView".into(),
            ));
        }
        validate_unchanged_regular_file(&entry.path, &entry.path)?;
        entry.touched_at = Instant::now();
        Ok(ResolvedSession {
            id: id.into(),
            clip_id: entry.clip_id.clone(),
            content_type: media_content_type(&entry.path),
            path: entry.path.clone(),
            selected_audio_indexes: entry.selected_audio_indexes.clone(),
            playback_patches: entry.playback_patches.clone(),
        })
    }

    pub fn release(&self, id: &str, owner: &str) -> bool {
        let mut sessions = self
            .sessions
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        if sessions
            .get(id)
            .is_some_and(|session| session.owner == owner)
        {
            sessions.remove(id);
            true
        } else {
            false
        }
    }

    pub fn release_owner(&self, owner: &str) -> usize {
        let mut sessions = self
            .sessions
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        let before = sessions.len();
        sessions.retain(|_, session| session.owner != owner);
        before - sessions.len()
    }

    pub fn active_count(&self) -> usize {
        let mut sessions = self
            .sessions
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        purge_expired(&mut sessions, self.ttl);
        sessions.len()
    }
}

fn selected_audio_indexes(tracks: &[String]) -> Vec<u8> {
    tracks
        .iter()
        .enumerate()
        .filter(|(_, track)| {
            track.as_str() != "mixed-preview-pcm" && track.as_str() != "Mixed preview"
        })
        .filter_map(|(index, _)| (index < 32).then_some(index as u8))
        .collect()
}

fn validate_unchanged_regular_file(path: &Path, expected_canonical: &Path) -> AppResult<()> {
    let metadata = fs::symlink_metadata(path).map_err(|source| AppError::Io {
        action: "inspect authorized media",
        path: path.to_owned(),
        source,
    })?;
    if !metadata.file_type().is_file() || metadata.file_type().is_symlink() {
        return Err(AppError::Path(
            "authorized media is no longer a regular file".into(),
        ));
    }
    let canonical = path.canonicalize().map_err(|source| AppError::Io {
        action: "resolve authorized media",
        path: path.to_owned(),
        source,
    })?;
    if canonical != expected_canonical {
        return Err(AppError::Path(
            "authorized media path changed after authorization".into(),
        ));
    }
    Ok(())
}

fn purge_expired(sessions: &mut HashMap<String, SessionEntry>, ttl: Duration) {
    sessions.retain(|_, session| session.touched_at.elapsed() <= ttl);
}

fn safe_endpoint(endpoint: &str) -> bool {
    endpoint == "clipture-media://localhost"
        || endpoint == "http://clipture-media.localhost"
        || endpoint
            .strip_prefix("http://127.0.0.1:")
            .is_some_and(|port| port.parse::<u16>().is_ok_and(|port| port > 0))
}

fn media_content_type(path: &Path) -> &'static str {
    match path
        .extension()
        .and_then(|value| value.to_str())
        .map(str::to_ascii_lowercase)
        .as_deref()
    {
        Some("webm") => "video/webm",
        Some("mov") => "video/quicktime",
        Some("mkv") => "video/x-matroska",
        Some("avi") => "video/x-msvideo",
        _ => "video/mp4",
    }
}

static TOKEN_COUNTER: AtomicU64 = AtomicU64::new(1);

fn random_token() -> String {
    let mut bytes = [0_u8; 24];
    if fill_system_random(&mut bytes) {
        return bytes.iter().map(|byte| format!("{byte:02x}")).collect();
    }
    let counter = TOKEN_COUNTER.fetch_add(1, Ordering::Relaxed);
    for block in 0..3_u64 {
        let mut hasher = std::collections::hash_map::DefaultHasher::new();
        SystemTime::now().hash(&mut hasher);
        std::process::id().hash(&mut hasher);
        counter.hash(&mut hasher);
        block.hash(&mut hasher);
        bytes[(block as usize) * 8..(block as usize + 1) * 8]
            .copy_from_slice(&hasher.finish().to_ne_bytes());
    }
    bytes.iter().map(|byte| format!("{byte:02x}")).collect()
}

#[cfg(windows)]
fn fill_system_random(bytes: &mut [u8]) -> bool {
    #[link(name = "bcrypt")]
    extern "system" {
        fn BCryptGenRandom(
            algorithm: *mut std::ffi::c_void,
            buffer: *mut u8,
            length: u32,
            flags: u32,
        ) -> i32;
    }
    const BCRYPT_USE_SYSTEM_PREFERRED_RNG: u32 = 0x0000_0002;
    // SAFETY: the system RNG writes exactly `bytes.len()` bytes to a valid,
    // uniquely borrowed output buffer. A null algorithm handle is required by
    // BCRYPT_USE_SYSTEM_PREFERRED_RNG.
    unsafe {
        BCryptGenRandom(
            std::ptr::null_mut(),
            bytes.as_mut_ptr(),
            bytes.len() as u32,
            BCRYPT_USE_SYSTEM_PREFERRED_RNG,
        ) >= 0
    }
}

#[cfg(not(windows))]
fn fill_system_random(bytes: &mut [u8]) -> bool {
    use std::io::Read;

    fs::File::open("/dev/urandom")
        .and_then(|mut random| random.read_exact(bytes))
        .is_ok()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::contracts::ClipRecord;

    #[test]
    fn unknown_paths_cannot_create_sessions_and_owners_are_isolated() {
        let root = tempfile::tempdir().unwrap();
        let video = root.path().join("clip.mp4");
        fs::write(&video, b"video").unwrap();
        let authority = PathAuthorizer::from_records([ClipRecord {
            id: "known".into(),
            file_path: video.to_string_lossy().into(),
            ..ClipRecord::default()
        }]);
        let registry = MediaSessionRegistry::new("clipture-media://localhost").unwrap();
        assert!(registry
            .open(&authority, "unknown", &[], vec![], "window-a")
            .is_err());
        let opened = registry
            .open(&authority, "known", &["system".into()], vec![], "window-a")
            .unwrap();
        assert!(registry.resolve(&opened.session_id, "window-b").is_err());
        assert!(registry.resolve(&opened.session_id, "window-a").is_ok());
        assert!(!registry.release(&opened.session_id, "window-b"));
        assert!(registry.release(&opened.session_id, "window-a"));
    }

    #[test]
    fn registry_is_bounded_and_evicts_oldest_sessions() {
        let root = tempfile::tempdir().unwrap();
        let video = root.path().join("clip.mp4");
        fs::write(&video, b"video").unwrap();
        let authority = PathAuthorizer::from_records([ClipRecord {
            id: "known".into(),
            file_path: video.to_string_lossy().into(),
            ..ClipRecord::default()
        }]);
        let registry = MediaSessionRegistry::with_policy(
            "clipture-media://localhost",
            Duration::from_secs(60),
            2,
        )
        .unwrap();
        for _ in 0..3 {
            registry
                .open(&authority, "known", &[], vec![], "owner")
                .unwrap();
        }
        assert_eq!(registry.active_count(), 2);
    }
}
