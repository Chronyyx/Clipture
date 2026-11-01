use super::*;
use crate::contracts::ClipRecord;
use crate::media::FfmpegOutput;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::thread;

const ENDPOINT: &str = "http://clipture-media.localhost";
const JPEG: &[u8] = &[0xff, 0xd8, 0xff, 0xe0, 1, 2, 3];

struct FakeFfmpeg {
    runs: AtomicUsize,
    delay: Duration,
}

impl FakeFfmpeg {
    fn new() -> Arc<Self> {
        Self::slow(Duration::ZERO)
    }

    fn slow(delay: Duration) -> Arc<Self> {
        Arc::new(Self { runs: AtomicUsize::new(0), delay })
    }

    fn runs(&self) -> usize {
        self.runs.load(Ordering::SeqCst)
    }
}

impl FfmpegExecutor for FakeFfmpeg {
    fn run(&self, _: FfmpegJob) -> AppResult<FfmpegOutput> {
        self.runs.fetch_add(1, Ordering::SeqCst);
        thread::sleep(self.delay);
        Ok(FfmpegOutput { success: true, exit_code: Some(0), stdout: JPEG.to_vec(), stderr: String::new() })
    }
}

struct Library {
    _root: tempfile::TempDir,
    video: PathBuf,
    cache: PathBuf,
    authority: PathAuthorizer,
}

fn library() -> Library {
    let root = tempfile::tempdir().unwrap();
    let video = root.path().join("clip.mp4");
    fs::write(&video, b"video").unwrap();
    let authority = PathAuthorizer::from_records([ClipRecord {
        id: "clip".into(),
        file_path: video.to_string_lossy().into(),
        ..ClipRecord::default()
    }]);
    let cache = root.path().join("thumbnails");
    Library { _root: root, video, cache, authority }
}

fn token_of(url: &str) -> &str {
    url.strip_prefix(ENDPOINT).unwrap().strip_prefix("/v1/thumbnail/").unwrap().strip_suffix("/image").unwrap()
}

fn rewrite(path: &Path, bytes: &[u8]) {
    // A different size changes the signature even within the clock's resolution.
    fs::write(path, bytes).unwrap();
}

#[test]
fn a_url_is_handed_out_without_running_ffmpeg() {
    let library = library();
    let ffmpeg = FakeFfmpeg::new();
    let service = ThumbnailService::new(ffmpeg.clone(), Some(library.cache.clone()));
    let url = service.url(&library.authority, "clip", ENDPOINT).unwrap();
    assert!(url.starts_with("http://clipture-media.localhost/v1/thumbnail/"));
    assert_eq!(token_of(&url).len(), 48);
    assert_eq!(service.url(&library.authority, "clip", ENDPOINT).unwrap(), url, "an unchanged clip keeps its URL");
    assert_eq!(ffmpeg.runs(), 0);
    assert!(!url.contains("clip.mp4"), "the URL never carries the clip path");
}

#[test]
fn an_image_is_made_once_and_survives_a_restart() {
    let library = library();
    let ffmpeg = FakeFfmpeg::new();
    let service = ThumbnailService::new(ffmpeg.clone(), Some(library.cache.clone()));
    let url = service.url(&library.authority, "clip", ENDPOINT).unwrap();
    assert_eq!(service.image(token_of(&url)).unwrap().as_slice(), JPEG);
    assert_eq!(service.image(token_of(&url)).unwrap().as_slice(), JPEG);
    assert_eq!(ffmpeg.runs(), 1, "the second request is served from memory");
    let files: Vec<_> = fs::read_dir(&library.cache).unwrap().filter_map(Result::ok).collect();
    assert_eq!(files.len(), 1);
    let name = files[0].file_name().to_string_lossy().into_owned();
    assert!(name.ends_with(".jpg") && !name.contains("clip"), "cache files are named by key, not by clip");

    let restarted = FakeFfmpeg::new();
    let after_restart = ThumbnailService::new(restarted.clone(), Some(library.cache.clone()));
    let url = after_restart.url(&library.authority, "clip", ENDPOINT).unwrap();
    assert_eq!(after_restart.image(token_of(&url)).unwrap().as_slice(), JPEG);
    assert_eq!(restarted.runs(), 0, "the next launch reads the thumbnail from disk");
}

#[test]
fn a_changed_clip_gets_a_new_url_and_a_new_image() {
    let library = library();
    let ffmpeg = FakeFfmpeg::new();
    let service = ThumbnailService::new(ffmpeg.clone(), Some(library.cache.clone()));
    let before = service.url(&library.authority, "clip", ENDPOINT).unwrap();
    service.image(token_of(&before)).unwrap();
    rewrite(&library.video, b"trimmed video");
    assert!(service.image(token_of(&before)).is_err(), "an outdated URL is refused, not shown");
    let after = service.url(&library.authority, "clip", ENDPOINT).unwrap();
    assert_ne!(after, before);
    service.image(token_of(&after)).unwrap();
    assert_eq!(ffmpeg.runs(), 2);
    service.invalidate(&library.video);
    assert!(service.image(token_of(&after)).is_err(), "an invalidated clip's URL is forgotten");
}

#[test]
fn unknown_tokens_and_unauthorized_clips_are_refused() {
    let library = library();
    let service = ThumbnailService::new(FakeFfmpeg::new(), Some(library.cache.clone()));
    assert!(service.url(&library.authority, "someone-else", ENDPOINT).is_err());
    assert!(service.image(&"a".repeat(48)).is_err());
}

#[test]
fn concurrent_requests_for_one_clip_run_ffmpeg_once() {
    let library = library();
    let ffmpeg = FakeFfmpeg::slow(Duration::from_millis(150));
    let service = Arc::new(ThumbnailService::new(ffmpeg.clone(), Some(library.cache.clone())));
    let url = service.url(&library.authority, "clip", ENDPOINT).unwrap();
    let token = token_of(&url).to_owned();
    let readers: Vec<_> = (0..8)
        .map(|_| {
            let service = service.clone();
            let token = token.clone();
            thread::spawn(move || service.image(&token).unwrap())
        })
        .collect();
    for reader in readers {
        assert_eq!(reader.join().unwrap().as_slice(), JPEG);
    }
    assert_eq!(ffmpeg.runs(), 1);
}

#[test]
fn a_damaged_cache_file_is_made_again() {
    let library = library();
    let first = ThumbnailService::new(FakeFfmpeg::new(), Some(library.cache.clone()));
    let url = first.url(&library.authority, "clip", ENDPOINT).unwrap();
    first.image(token_of(&url)).unwrap();
    let file = fs::read_dir(&library.cache).unwrap().next().unwrap().unwrap().path();
    fs::write(&file, b"not a jpeg").unwrap();

    let ffmpeg = FakeFfmpeg::new();
    let restarted = ThumbnailService::new(ffmpeg.clone(), Some(library.cache.clone()));
    let url = restarted.url(&library.authority, "clip", ENDPOINT).unwrap();
    assert_eq!(restarted.image(token_of(&url)).unwrap().as_slice(), JPEG);
    assert_eq!(ffmpeg.runs(), 1);
    assert_eq!(fs::read(&file).unwrap(), JPEG, "the damaged file is replaced");
}

#[test]
fn prewarming_a_saved_clip_means_the_library_never_waits() {
    let library = library();
    let ffmpeg = FakeFfmpeg::new();
    ThumbnailService::new(ffmpeg.clone(), Some(library.cache.clone())).prewarm(&library.video).unwrap();
    assert_eq!(ffmpeg.runs(), 1);

    let later = FakeFfmpeg::new();
    let service = ThumbnailService::new(later.clone(), Some(library.cache.clone()));
    let url = service.url(&library.authority, "clip", ENDPOINT).unwrap();
    service.image(token_of(&url)).unwrap();
    assert_eq!(later.runs(), 0);
}

fn cached_files(cache: &Path) -> Vec<String> {
    let mut names: Vec<_> = fs::read_dir(cache)
        .map(|entries| entries.filter_map(Result::ok).map(|entry| entry.file_name().to_string_lossy().into_owned()).collect())
        .unwrap_or_default();
    names.sort();
    names
}

#[test]
fn deleting_a_clip_deletes_its_thumbnail() {
    let library = library();
    let other = library.video.with_file_name("other.mp4");
    fs::write(&other, b"other video").unwrap();
    let ffmpeg = FakeFfmpeg::new();
    let service = ThumbnailService::new(ffmpeg.clone(), Some(library.cache.clone()));
    let url = service.url(&library.authority, "clip", ENDPOINT).unwrap();
    service.image(token_of(&url)).unwrap();
    service.prewarm(&other).unwrap();
    assert_eq!(cached_files(&library.cache).len(), 2);

    fs::remove_file(&library.video).unwrap();
    service.delete(&library.video);
    assert_eq!(cached_files(&library.cache).len(), 1, "only the deleted clip's file goes");
    assert!(service.image(token_of(&url)).is_err(), "its URL is gone");

    let other_service = ThumbnailService::new(FakeFfmpeg::new(), Some(library.cache.clone()));
    other_service.prewarm(&other).unwrap();
    assert_eq!(cached_files(&library.cache).len(), 1, "the other clip's thumbnail is still cached");
}

#[test]
fn a_rewritten_clip_keeps_one_thumbnail_file() {
    let library = library();
    let service = ThumbnailService::new(FakeFfmpeg::new(), Some(library.cache.clone()));
    service.prewarm(&library.video).unwrap();
    let before = cached_files(&library.cache);
    rewrite(&library.video, b"trimmed video");
    service.prewarm(&library.video).unwrap();
    let after = cached_files(&library.cache);
    assert_eq!(after.len(), 1, "the old version's file is replaced, not kept");
    assert_ne!(after, before);
    assert_eq!(after[0].split('-').next(), before[0].split('-').next(), "same clip, new version");
}

#[test]
fn the_disk_cache_keeps_the_newest_files() {
    let root = tempfile::tempdir().unwrap();
    for index in 0..6 {
        fs::write(root.path().join(format!("{index}.jpg")), JPEG).unwrap();
        thread::sleep(Duration::from_millis(15));
    }
    fs::write(root.path().join("keep.txt"), b"other").unwrap();
    prune(root.path(), 3);
    let mut left: Vec<_> = fs::read_dir(root.path())
        .unwrap()
        .filter_map(Result::ok)
        .map(|entry| entry.file_name().to_string_lossy().into_owned())
        .collect();
    left.sort();
    assert_eq!(left, ["3.jpg", "4.jpg", "5.jpg", "keep.txt"]);
}

#[test]
fn without_a_cache_folder_images_stay_in_memory() {
    let library = library();
    let ffmpeg = FakeFfmpeg::new();
    let service = ThumbnailService::new(ffmpeg.clone(), None);
    let url = service.url(&library.authority, "clip", ENDPOINT).unwrap();
    service.image(token_of(&url)).unwrap();
    service.image(token_of(&url)).unwrap();
    assert_eq!(ffmpeg.runs(), 1);
}

/// Real FFmpeg on a folder of clips: one thumbnail at a time (the old
/// service), the first launch with the disk cache (two FFmpeg slots, four
/// requests in flight like the window), and the next launch from disk.
#[test]
#[ignore = "requires CLIPTURE_TEST_FFMPEG and CLIPTURE_THUMBNAIL_BENCH (a folder of clips)"]
fn benchmark_cold_and_warm_library_thumbnails() {
    use crate::media::{AlwaysReady, CommandFfmpeg};
    use std::time::Instant;
    let ffmpeg_path = PathBuf::from(std::env::var_os("CLIPTURE_TEST_FFMPEG").expect("set CLIPTURE_TEST_FFMPEG"));
    let folder = PathBuf::from(std::env::var_os("CLIPTURE_THUMBNAIL_BENCH").expect("set CLIPTURE_THUMBNAIL_BENCH"));
    let mut records = Vec::new();
    for (index, entry) in fs::read_dir(&folder).unwrap().filter_map(Result::ok).enumerate() {
        if entry.path().extension().is_some_and(|extension| extension == "mp4") {
            records.push(ClipRecord {
                id: format!("clip-{index}"),
                file_path: entry.path().to_string_lossy().into(),
                ..ClipRecord::default()
            });
        }
    }
    assert!(!records.is_empty(), "no clips in the bench folder");
    let ids: Vec<String> = records.iter().map(|record| record.id.clone()).collect();
    let authority = Arc::new(PathAuthorizer::from_records(records));
    let cache = tempfile::tempdir().unwrap();

    let serial = ThumbnailService::new(Arc::new(CommandFfmpeg::with_concurrency(ffmpeg_path.clone(), Arc::new(AlwaysReady), 1)), None);
    let started = Instant::now();
    for id in &ids {
        let url = serial.url(&authority, id, ENDPOINT).unwrap();
        serial.image(token_of(&url)).unwrap();
    }
    let serial_ms = started.elapsed().as_millis();

    let load_all = |service: Arc<ThumbnailService>| {
        let started = Instant::now();
        let urls: Vec<String> = ids.iter().map(|id| service.url(&authority, id, ENDPOINT).unwrap()).collect();
        let url_ms = started.elapsed().as_secs_f64() * 1000.0;
        let queue = Arc::new(Mutex::new(urls));
        let first_image_ms = Arc::new(Mutex::new(None::<f64>));
        let workers: Vec<_> = (0..4)
            .map(|_| {
                let (service, queue, first) = (service.clone(), queue.clone(), first_image_ms.clone());
                thread::spawn(move || loop {
                    let Some(url) = queue.lock().unwrap().pop() else { break };
                    let image = service.image(token_of(&url)).unwrap();
                    assert!(image.starts_with(&[0xff, 0xd8]), "a JPEG");
                    first.lock().unwrap().get_or_insert(started.elapsed().as_secs_f64() * 1000.0);
                })
            })
            .collect();
        for worker in workers {
            worker.join().unwrap();
        }
        let first = first_image_ms.lock().unwrap().unwrap();
        (url_ms, first, started.elapsed().as_secs_f64() * 1000.0)
    };
    let executor = || Arc::new(CommandFfmpeg::with_concurrency(ffmpeg_path.clone(), Arc::new(AlwaysReady), 2));
    let (cold_urls, cold_first, cold_all) = load_all(Arc::new(ThumbnailService::new(executor(), Some(cache.path().to_owned()))));
    let (warm_urls, warm_first, warm_all) = load_all(Arc::new(ThumbnailService::new(executor(), Some(cache.path().to_owned()))));
    println!(
        "THUMBNAIL BENCH clips={} | old one-at-a-time: {serial_ms} ms every launch | first launch: urls {cold_urls:.1} ms, first image {cold_first:.0} ms, all {cold_all:.0} ms | next launch: urls {warm_urls:.1} ms, first image {warm_first:.1} ms, all {warm_all:.1} ms",
        ids.len()
    );
    assert!(warm_all < cold_all / 5.0, "the next launch must come from disk");
}
