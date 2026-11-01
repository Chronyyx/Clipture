//! Clip thumbnails as cached JPEG files served by URL.
//!
//! A thumbnail is made once per clip version (path, size and modification
//! time) and kept in `thumbnails/` in the data folder (about 12 KiB each), so
//! a library opens from disk instead of running FFmpeg for every card on every
//! launch. A clip keeps one file: a new version replaces it, and deleting the
//! clip deletes it. The window
//! gets an opaque URL at once and loads the image through the media protocol
//! like any other picture, so lazy loading, decoding and caching are the
//! browser's, and no command slot waits on FFmpeg. Saved clips get theirs right
//! after the save, before the library asks.
use std::{
    collections::{HashMap, HashSet, VecDeque},
    ffi::OsString,
    fs,
    path::{Path, PathBuf},
    sync::{Arc, Condvar, Mutex},
    time::{Duration, UNIX_EPOCH},
};

use crate::{
    clips::PathAuthorizer,
    error::{AppError, AppResult},
};

use super::{library_input, sessions::random_token, FfmpegExecutor, FfmpegJob};

const THUMBNAIL_WIDTH: u32 = 480;
const THUMBNAIL_HEIGHT: u32 = 270;
const MAXIMUM_JPEG_BYTES: usize = 2 * 1024 * 1024;
/// Bumped when the image format changes, so old files are not served.
const FORMAT_VERSION: &str = "v1";
/// Recently served images kept in memory (about 15 KiB each).
const MEMORY_ENTRIES: usize = 192;
/// Thumbnail files kept on disk (about 6 MiB); the oldest go first. Clips
/// deleted outside Clipture leave theirs until then.
const DISK_ENTRIES: usize = 500;
/// Tokens handed out this run; the oldest are forgotten past this.
const TOKEN_ENTRIES: usize = 8192;

#[derive(Clone)]
struct Source {
    path: PathBuf,
    signature: String,
}

#[derive(Default)]
struct Tokens {
    by_token: HashMap<String, Source>,
    by_path: HashMap<PathBuf, String>,
    order: VecDeque<String>,
}

#[derive(Default)]
struct MemoryCache {
    images: HashMap<String, Arc<Vec<u8>>>,
    order: VecDeque<String>,
}

pub struct ThumbnailService {
    executor: Arc<dyn FfmpegExecutor>,
    directory: Option<PathBuf>,
    tokens: Mutex<Tokens>,
    memory: Mutex<MemoryCache>,
    /// Keys being made now: a second request for one waits for the first.
    making: Mutex<HashSet<String>>,
    made: Condvar,
}

impl ThumbnailService {
    /// Without a directory images are kept in memory only (tests, no data folder).
    pub fn new(executor: Arc<dyn FfmpegExecutor>, directory: Option<PathBuf>) -> Self {
        Self {
            executor,
            directory,
            tokens: Mutex::new(Tokens::default()),
            memory: Mutex::new(MemoryCache::default()),
            making: Mutex::new(HashSet::new()),
            made: Condvar::new(),
        }
    }

    /// The image URL for an authorized clip under `endpoint`. Cheap: no FFmpeg
    /// and no file read beyond the clip's metadata. The token changes when the
    /// clip does, so a URL never shows an outdated picture.
    pub fn url(&self, authority: &PathAuthorizer, clip_id: &str, endpoint: &str) -> AppResult<String> {
        let path = canonical(
            authority
                .primary(clip_id)
                .ok_or_else(|| AppError::Path("clip is not authorized for thumbnail access".into()))?,
        );
        let signature = file_signature(&path)?;
        let mut tokens = lock(&self.tokens);
        if let Some(token) = tokens.by_path.get(&path).cloned() {
            if tokens.by_token.get(&token).is_some_and(|source| source.signature == signature) {
                return Ok(image_url(endpoint, &token));
            }
            tokens.by_token.remove(&token);
        }
        let token = random_token();
        tokens.by_path.insert(path.clone(), token.clone());
        tokens.by_token.insert(token.clone(), Source { path, signature });
        tokens.order.push_back(token.clone());
        while tokens.order.len() > TOKEN_ENTRIES {
            if let Some(oldest) = tokens.order.pop_front() {
                if let Some(source) = tokens.by_token.remove(&oldest) {
                    if tokens.by_path.get(&source.path) == Some(&oldest) {
                        tokens.by_path.remove(&source.path);
                    }
                }
            }
        }
        Ok(image_url(endpoint, &token))
    }

    /// The JPEG for a token from `url`, made now if no cache has it.
    pub fn image(&self, token: &str) -> AppResult<Arc<Vec<u8>>> {
        let source = lock(&self.tokens)
            .by_token
            .get(token)
            .cloned()
            .ok_or_else(|| AppError::Path("thumbnail is unavailable".into()))?;
        if file_signature(&source.path)? != source.signature {
            return Err(AppError::Path("the clip changed; its thumbnail is outdated".into()));
        }
        self.load_or_make(&source)
    }

    /// Makes a saved clip's thumbnail ahead of the library asking for it.
    pub fn prewarm(&self, path: &Path) -> AppResult<()> {
        let path = canonical(path);
        let signature = file_signature(&path)?;
        self.load_or_make(&Source { path, signature }).map(|_| ())
    }

    /// The clip at `path` was rewritten (trimmed, mixed): forget its token.
    pub fn invalidate(&self, path: &Path) {
        let path = canonical(path);
        let mut tokens = lock(&self.tokens);
        if let Some(token) = tokens.by_path.remove(&path) {
            tokens.by_token.remove(&token);
        }
    }

    /// The clip at `path` was deleted: its URL and every file it had go.
    pub fn delete(&self, path: &Path) {
        self.invalidate(path);
        let prefix = path_key(&canonical(path));
        lock(&self.memory).images.retain(|key, _| !key.starts_with(&prefix));
        lock(&self.memory).order.retain(|key| !key.starts_with(&prefix));
        if let Some(directory) = &self.directory {
            remove_versions(directory, &prefix, None);
        }
    }

    fn load_or_make(&self, source: &Source) -> AppResult<Arc<Vec<u8>>> {
        let key = cache_key(source);
        if let Some(image) = self.remembered(&key) {
            return Ok(image);
        }
        // One maker per key; others wait for its file instead of running FFmpeg.
        {
            let mut making = lock(&self.making);
            while making.contains(&key) {
                making = self.made.wait(making).unwrap_or_else(|poisoned| poisoned.into_inner());
            }
            if let Some(image) = self.remembered(&key).or_else(|| self.read_disk(&key)) {
                self.remember(&key, image.clone());
                return Ok(image);
            }
            making.insert(key.clone());
        }
        let result = self.make(&source.path).map(Arc::new);
        if let Ok(image) = &result {
            self.write_disk(&key, image);
            self.remember(&key, image.clone());
        }
        lock(&self.making).remove(&key);
        self.made.notify_all();
        result
    }

    fn make(&self, path: &Path) -> AppResult<Vec<u8>> {
        let output = self.executor.run(thumbnail_job(path))?;
        if !output.success || output.stdout.is_empty() {
            return Err(AppError::Integration(if output.stderr.is_empty() {
                format!("thumbnail FFmpeg exited with code {:?}", output.exit_code)
            } else {
                output.stderr
            }));
        }
        Ok(output.stdout)
    }

    fn remembered(&self, key: &str) -> Option<Arc<Vec<u8>>> {
        let mut memory = lock(&self.memory);
        let image = memory.images.get(key).cloned()?;
        memory.order.retain(|candidate| candidate != key);
        memory.order.push_back(key.to_owned());
        Some(image)
    }

    fn remember(&self, key: &str, image: Arc<Vec<u8>>) {
        let mut memory = lock(&self.memory);
        memory.order.retain(|candidate| candidate != key);
        memory.order.push_back(key.to_owned());
        memory.images.insert(key.to_owned(), image);
        while memory.images.len() > MEMORY_ENTRIES {
            let Some(oldest) = memory.order.pop_front() else { break };
            memory.images.remove(&oldest);
        }
    }

    fn read_disk(&self, key: &str) -> Option<Arc<Vec<u8>>> {
        let bytes = fs::read(self.directory.as_ref()?.join(format!("{key}.jpg"))).ok()?;
        // A torn or foreign file is made again rather than shown.
        (bytes.starts_with(&[0xff, 0xd8]) && bytes.len() <= MAXIMUM_JPEG_BYTES).then(|| Arc::new(bytes))
    }

    fn write_disk(&self, key: &str, image: &[u8]) {
        let Some(directory) = &self.directory else { return };
        if let Err(error) = write_atomically(directory, key, image) {
            tracing::warn!(%error, "could not cache a thumbnail on disk");
            return;
        }
        // The clip's earlier versions (before a trim) are not shown again.
        if let Some((prefix, _)) = key.split_once('-') {
            remove_versions(directory, prefix, Some(key));
        }
        prune(directory, DISK_ENTRIES);
    }
}

/// One spelling per clip: the library hands over canonical paths, a save or a
/// trim may not, and both must reach the same token and cache file. A deleted
/// clip is spelled through its folder, which still exists.
fn canonical(path: &Path) -> PathBuf {
    if let Ok(path) = fs::canonicalize(path) {
        return path;
    }
    match (path.parent().and_then(|parent| fs::canonicalize(parent).ok()), path.file_name()) {
        (Some(parent), Some(name)) => parent.join(name),
        _ => path.to_owned(),
    }
}

fn lock<T>(mutex: &Mutex<T>) -> std::sync::MutexGuard<'_, T> {
    mutex.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
}

fn image_url(endpoint: &str, token: &str) -> String {
    format!("{endpoint}/v1/thumbnail/{token}/image")
}

/// The file name for one version of one clip, `<clip>-<version>`: the clip's
/// part finds all its files, and the clip path never appears.
fn cache_key(source: &Source) -> String {
    let version = format!("{FORMAT_VERSION}|{}", source.signature);
    format!("{}-{}", path_key(&source.path), &blake3::hash(version.as_bytes()).to_hex()[..16])
}

fn path_key(path: &Path) -> String {
    blake3::hash(path.to_string_lossy().to_lowercase().as_bytes()).to_hex()[..16].to_owned()
}

/// Removes the clip's thumbnail files, all but `keep` (a file name stem).
fn remove_versions(directory: &Path, prefix: &str, keep: Option<&str>) {
    let Ok(entries) = fs::read_dir(directory) else { return };
    for entry in entries.filter_map(Result::ok) {
        let path = entry.path();
        let Some(stem) = path.file_stem().and_then(|stem| stem.to_str()) else { continue };
        let jpeg = path.extension().is_some_and(|extension| extension == "jpg");
        if jpeg && stem.starts_with(&format!("{prefix}-")) && Some(stem) != keep {
            let _ = fs::remove_file(&path);
        }
    }
}

fn write_atomically(directory: &Path, key: &str, image: &[u8]) -> std::io::Result<()> {
    fs::create_dir_all(directory)?;
    let temporary = directory.join(format!("{key}.{}.tmp", std::process::id()));
    fs::write(&temporary, image)?;
    fs::rename(&temporary, directory.join(format!("{key}.jpg"))).inspect_err(|_| {
        let _ = fs::remove_file(&temporary);
    })
}

/// Keeps the newest `keep` thumbnails; an old clip's image is made again if
/// it is ever shown.
fn prune(directory: &Path, keep: usize) {
    let Ok(entries) = fs::read_dir(directory) else { return };
    let mut files: Vec<_> = entries
        .filter_map(Result::ok)
        .filter(|entry| entry.path().extension().is_some_and(|extension| extension == "jpg"))
        .filter_map(|entry| Some((entry.metadata().ok()?.modified().ok()?, entry.path())))
        .collect();
    if files.len() <= keep {
        return;
    }
    files.sort_by_key(|(modified, _)| *modified);
    for (_, path) in files.iter().take(files.len() - keep) {
        let _ = fs::remove_file(path);
    }
}

fn thumbnail_job(path: &Path) -> FfmpegJob {
    let head = [
        OsString::from("-nostdin"),
        OsString::from("-hide_banner"),
        OsString::from("-loglevel"),
        OsString::from("error"),
        OsString::from("-threads"),
        OsString::from("1"),
        OsString::from("-ss"),
        OsString::from("0.1"),
    ];
    let mut job = FfmpegJob::new(
        "thumbnail extraction",
        head.into_iter().chain(library_input(path)).chain([
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
        ]),
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
#[path = "thumbnails_tests.rs"]
mod tests;
