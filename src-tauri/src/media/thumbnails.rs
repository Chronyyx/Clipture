use std::{
    collections::{HashMap, VecDeque},
    ffi::OsString,
    fs,
    path::{Path, PathBuf},
    sync::{Arc, Mutex},
    time::{Duration, UNIX_EPOCH},
};

use crate::{
    clips::PathAuthorizer,
    error::{AppError, AppResult},
};

use super::{base64, FfmpegExecutor, FfmpegJob};

const THUMBNAIL_WIDTH: u32 = 480;
const THUMBNAIL_HEIGHT: u32 = 270;
const MAXIMUM_JPEG_BYTES: usize = 2 * 1024 * 1024;

#[derive(Clone)]
struct ThumbnailEntry {
    signature: String,
    data_url: String,
}

#[derive(Default)]
struct ThumbnailCache {
    entries: HashMap<PathBuf, ThumbnailEntry>,
    order: VecDeque<PathBuf>,
}

pub struct ThumbnailService {
    executor: Arc<dyn FfmpegExecutor>,
    cache: Mutex<ThumbnailCache>,
    extraction_gate: Mutex<()>,
    capacity: usize,
}

impl ThumbnailService {
    pub fn new(executor: Arc<dyn FfmpegExecutor>) -> Self {
        Self::with_capacity(executor, 64)
    }

    pub fn with_capacity(executor: Arc<dyn FfmpegExecutor>, capacity: usize) -> Self {
        Self {
            executor,
            cache: Mutex::new(ThumbnailCache::default()),
            extraction_gate: Mutex::new(()),
            capacity: capacity.max(1),
        }
    }

    pub fn thumbnail(&self, authority: &PathAuthorizer, clip_id: &str) -> AppResult<String> {
        let path = authority
            .primary(clip_id)
            .ok_or_else(|| AppError::Path("clip is not authorized for thumbnail access".into()))?;
        let signature = file_signature(path)?;
        if let Some(value) = self.cached(path, &signature) {
            return Ok(value);
        }

        // Serialize large-file seeks so thumbnail generation cannot become a
        // random-I/O workload while capture is writing the replay archive.
        let _guard = self
            .extraction_gate
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        if let Some(value) = self.cached(path, &signature) {
            return Ok(value);
        }
        let output = self.executor.run(thumbnail_job(path))?;
        if !output.success || output.stdout.is_empty() {
            return Err(AppError::Integration(if output.stderr.is_empty() {
                format!("thumbnail FFmpeg exited with code {:?}", output.exit_code)
            } else {
                output.stderr
            }));
        }
        let data_url = format!("data:image/jpeg;base64,{}", base64::encode(&output.stdout));
        self.insert(path.to_owned(), signature, data_url.clone());
        Ok(data_url)
    }

    pub fn invalidate(&self, path: &Path) {
        let mut cache = self
            .cache
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        cache.entries.remove(path);
        cache.order.retain(|candidate| candidate != path);
    }

    pub fn clear(&self) {
        *self
            .cache
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner()) = ThumbnailCache::default();
    }

    fn cached(&self, path: &Path, signature: &str) -> Option<String> {
        let mut cache = self
            .cache
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        let entry = cache.entries.get(path).cloned();
        match entry {
            Some(entry) if entry.signature == signature => {
                cache.order.retain(|candidate| candidate != path);
                cache.order.push_back(path.to_owned());
                Some(entry.data_url)
            }
            Some(_) => {
                cache.entries.remove(path);
                cache.order.retain(|candidate| candidate != path);
                None
            }
            None => None,
        }
    }

    fn insert(&self, path: PathBuf, signature: String, data_url: String) {
        let mut cache = self
            .cache
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        cache.order.retain(|candidate| candidate != &path);
        cache.order.push_back(path.clone());
        cache.entries.insert(
            path,
            ThumbnailEntry {
                signature,
                data_url,
            },
        );
        while cache.entries.len() > self.capacity {
            if let Some(oldest) = cache.order.pop_front() {
                cache.entries.remove(&oldest);
            } else {
                break;
            }
        }
    }
}

fn thumbnail_job(path: &Path) -> FfmpegJob {
    let mut job = FfmpegJob::new(
        "thumbnail extraction",
        [
            OsString::from("-nostdin"),
            OsString::from("-hide_banner"),
            OsString::from("-loglevel"),
            OsString::from("error"),
            OsString::from("-threads"),
            OsString::from("1"),
            OsString::from("-ss"),
            OsString::from("0.1"),
            OsString::from("-i"),
            path.as_os_str().to_owned(),
            OsString::from("-map"),
            OsString::from("0:v:0"),
            OsString::from("-frames:v"),
            OsString::from("1"),
            OsString::from("-an"),
            OsString::from("-sn"),
            OsString::from("-dn"),
            OsString::from("-vf"),
            OsString::from(format!(
                "scale={THUMBNAIL_WIDTH}:{THUMBNAIL_HEIGHT}:force_original_aspect_ratio=decrease,pad={THUMBNAIL_WIDTH}:{THUMBNAIL_HEIGHT}:(ow-iw)/2:(oh-ih)/2:color=0x171a20"
            )),
            OsString::from("-q:v"),
            OsString::from("5"),
            OsString::from("-c:v"),
            OsString::from("mjpeg"),
            OsString::from("-f"),
            OsString::from("image2pipe"),
            OsString::from("pipe:1"),
        ],
    );
    job.timeout = Duration::from_secs(15);
    job.maximum_stdout_bytes = MAXIMUM_JPEG_BYTES;
    job
}

fn file_signature(path: &Path) -> AppResult<String> {
    let metadata = fs::metadata(path).map_err(|source| AppError::Io {
        action: "inspect media for thumbnail",
        path: path.to_owned(),
        source,
    })?;
    let modified = metadata
        .modified()
        .unwrap_or(UNIX_EPOCH)
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_nanos();
    Ok(format!("{}:{modified}", metadata.len()))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::contracts::ClipRecord;
    use crate::media::FfmpegOutput;
    use std::sync::atomic::{AtomicUsize, Ordering};

    struct FakeFfmpeg(AtomicUsize);
    impl FfmpegExecutor for FakeFfmpeg {
        fn run(&self, _: FfmpegJob) -> AppResult<FfmpegOutput> {
            self.0.fetch_add(1, Ordering::Relaxed);
            Ok(FfmpegOutput {
                success: true,
                exit_code: Some(0),
                stdout: vec![0xff, 0xd8, 0xff],
                stderr: String::new(),
            })
        }
    }

    #[test]
    fn caches_by_authorized_path_signature() {
        let root = tempfile::tempdir().unwrap();
        let video = root.path().join("clip.mp4");
        fs::write(&video, b"video").unwrap();
        let authority = PathAuthorizer::from_records([ClipRecord {
            id: "clip".into(),
            file_path: video.to_string_lossy().into(),
            ..ClipRecord::default()
        }]);
        let executor = Arc::new(FakeFfmpeg(AtomicUsize::new(0)));
        let service = ThumbnailService::new(executor.clone());
        let first = service.thumbnail(&authority, "clip").unwrap();
        let second = service.thumbnail(&authority, "clip").unwrap();
        assert_eq!(first, second);
        assert_eq!(executor.0.load(Ordering::Relaxed), 1);
    }
}
