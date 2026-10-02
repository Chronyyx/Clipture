use std::{
    fs::File,
    io::{Read, Seek, SeekFrom},
    sync::{Arc, Condvar, Mutex},
    thread,
    time::{Duration, Instant},
};

use tauri::{
    http::{
        header::{
            ACCEPT_RANGES, ACCESS_CONTROL_ALLOW_HEADERS, ACCESS_CONTROL_ALLOW_METHODS,
            ACCESS_CONTROL_ALLOW_ORIGIN, ALLOW, CACHE_CONTROL, CONTENT_LENGTH, CONTENT_RANGE,
            CONTENT_TYPE, RANGE,
        },
        Method, Request, Response, StatusCode, Uri,
    },
    AppHandle, Runtime,
};

use crate::error::{AppError, AppResult};

use super::{apply_patches, ByteRange, MediaService, RemoteVideoChunk, VideoStreamPlan};

pub const URI_SCHEME: &str = "clipture-media";

const MAXIMUM_REQUEST_URI_BYTES: usize = 1_024;
const MAXIMUM_RANGE_HEADER_BYTES: usize = 128;
const MAXIMUM_VIDEO_RESPONSE_BYTES: u64 = 4 * 1024 * 1024;
const MAXIMUM_AUDIO_START_SECONDS: f64 = 24.0 * 60.0 * 60.0;
// Video byte ranges and mixed-audio FFmpeg jobs get separate budgets: slow
// audio extraction must never occupy the slots the video element needs, or
// the player keeps playing audio over a black, stalled picture.
const MAXIMUM_IN_FLIGHT_VIDEO_REQUESTS: usize = 6;
const MAXIMUM_IN_FLIGHT_AUDIO_REQUESTS: usize = 2;
// A media element treats a failed range request as a stall; briefly queueing
// behind in-flight reads (each at most 4 MiB) is far cheaper than a 503.
const VIDEO_REQUEST_QUEUE_TIMEOUT: Duration = Duration::from_secs(10);

/// Registers the opaque media transport without coupling the media domain to
/// the application's composition state. The resolver is called only after
/// Tauri setup has installed `MediaService` in the host state.
pub fn register<R, F>(builder: tauri::Builder<R>, resolve: F) -> tauri::Builder<R>
where
    R: Runtime,
    F: Fn(&AppHandle<R>) -> Option<Arc<MediaService>> + Send + Sync + 'static,
{
    let admission = Arc::new(MediaAdmission::new());
    builder.register_asynchronous_uri_scheme_protocol(
        URI_SCHEME,
        move |context, request, responder| {
            let owner = context.webview_label().to_owned();
            let media = resolve(context.app_handle());
            let admission = Arc::clone(&admission);
            thread::spawn(move || {
                let Some(_permit) = admission.admit(request.uri()) else {
                    responder.respond(busy_response());
                    return;
                };
                responder.respond(handle_request(media, &owner, request));
            });
        },
    )
}

/// Media request budgets, shared by the in-process protocol and the UI-process
/// relay so both transports queue video reads instead of failing them.
pub struct MediaAdmission {
    video: Arc<RequestLimiter>,
    audio: Arc<RequestLimiter>,
}

impl MediaAdmission {
    pub fn new() -> Self {
        Self {
            video: Arc::new(RequestLimiter::new(MAXIMUM_IN_FLIGHT_VIDEO_REQUESTS)),
            audio: Arc::new(RequestLimiter::new(MAXIMUM_IN_FLIGHT_AUDIO_REQUESTS)),
        }
    }

    /// Blocks briefly for a video slot; the mixed-audio loader tolerates a
    /// failed chunk and retries, so audio fails fast.
    pub fn admit(&self, uri: &Uri) -> Option<RequestPermit> {
        if matches!(
            parse_route(uri),
            Ok(Route::Audio { .. } | Route::RemoteAudio { .. })
        ) {
            self.audio.try_acquire()
        } else {
            self.video.acquire_within(VIDEO_REQUEST_QUEUE_TIMEOUT)
        }
    }
}

fn busy_response() -> Response<Vec<u8>> {
    text_response(StatusCode::SERVICE_UNAVAILABLE, "media service is busy")
}

pub(crate) fn handle_request(
    media: Option<Arc<MediaService>>,
    owner: &str,
    request: Request<Vec<u8>>,
) -> Response<Vec<u8>> {
    if request.uri().to_string().len() > MAXIMUM_REQUEST_URI_BYTES
        || !request.body().is_empty()
        || !trusted_protocol_uri(request.uri())
    {
        return text_response(StatusCode::BAD_REQUEST, "invalid media request");
    }
    if request.method() == Method::OPTIONS {
        return preflight_response();
    }
    let route = match parse_route(request.uri()) {
        Ok(route) => route,
        Err(message) => return text_response(StatusCode::BAD_REQUEST, message),
    };
    let Some(media) = media else {
        return text_response(
            StatusCode::SERVICE_UNAVAILABLE,
            "media service is unavailable",
        );
    };

    match route {
        Route::Video { session_id } => {
            if request.method() != Method::GET && request.method() != Method::HEAD {
                return method_not_allowed("GET, HEAD, OPTIONS");
            }
            let range_header = match single_range_header(&request) {
                Ok(value) => value,
                Err(message) => return text_response(StatusCode::BAD_REQUEST, message),
            };
            match media.video_stream_plan(session_id, owner, range_header) {
                Ok(plan) => video_response(
                    plan,
                    request.method() == Method::HEAD,
                    range_header.is_some(),
                ),
                Err(error) => service_error_response(&error),
            }
        }
        Route::Remote { stream_id } => {
            if request.method() != Method::GET && request.method() != Method::HEAD {
                return method_not_allowed("GET, HEAD, OPTIONS");
            }
            let range_header = match single_range_header(&request) {
                Ok(value) => value,
                Err(message) => return text_response(StatusCode::BAD_REQUEST, message),
            };
            match media.remote_video(stream_id, owner, range_header) {
                Ok(chunk) => remote_video_response(chunk, request.method() == Method::HEAD),
                Err(error) => service_error_response(&error),
            }
        }
        Route::RemoteAudio {
            stream_id,
            start_seconds,
            duration_seconds,
        } => {
            if request.method() != Method::GET && request.method() != Method::HEAD {
                return method_not_allowed("GET, HEAD, OPTIONS");
            }
            if request.method() == Method::HEAD {
                return binary_response(StatusCode::OK, "audio/wav", Vec::new());
            }
            match media.remote_mixed_audio_chunk(stream_id, owner, start_seconds, duration_seconds) {
                Ok(bytes) => binary_response(StatusCode::OK, "audio/wav", bytes),
                Err(error) => service_error_response(&error),
            }
        }
        Route::Audio {
            session_id,
            start_seconds,
            duration_seconds,
        } => {
            if request.method() != Method::GET && request.method() != Method::HEAD {
                return method_not_allowed("GET, HEAD, OPTIONS");
            }
            if request.method() == Method::HEAD {
                return binary_response(StatusCode::OK, "audio/wav", Vec::new());
            }
            match media.mixed_audio_chunk(session_id, owner, start_seconds, duration_seconds) {
                Ok(bytes) => binary_response(StatusCode::OK, "audio/wav", bytes),
                Err(error) => service_error_response(&error),
            }
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
enum Route<'a> {
    Video {
        session_id: &'a str,
    },
    /// A clip streamed from a friend; the id is an opaque stream session.
    Remote {
        stream_id: &'a str,
    },
    /// Every audio track of a fully arrived stream, mixed.
    RemoteAudio {
        stream_id: &'a str,
        start_seconds: f64,
        duration_seconds: f64,
    },
    Audio {
        session_id: &'a str,
        start_seconds: f64,
        duration_seconds: f64,
    },
}

fn parse_route(uri: &Uri) -> Result<Route<'_>, &'static str> {
    let segments: Vec<_> = uri.path().split('/').collect();
    if segments.len() != 5
        || segments[0] != ""
        || segments[1] != "v1"
        || !valid_session_id(segments[3])
    {
        return Err("invalid media route");
    }
    if segments[2] == "remote" {
        return match segments[4] {
            "video" if uri.query().is_none() => Ok(Route::Remote {
                stream_id: segments[3],
            }),
            "audio" => {
                let (start_seconds, duration_seconds) = parse_audio_query(uri.query())?;
                Ok(Route::RemoteAudio {
                    stream_id: segments[3],
                    start_seconds,
                    duration_seconds,
                })
            }
            _ => Err("invalid media route"),
        };
    }
    if segments[2] != "session" {
        return Err("invalid media route");
    }
    let session_id = segments[3];
    match segments[4] {
        "video" if uri.query().is_none() => Ok(Route::Video { session_id }),
        "audio" => {
            let (start_seconds, duration_seconds) = parse_audio_query(uri.query())?;
            Ok(Route::Audio {
                session_id,
                start_seconds,
                duration_seconds,
            })
        }
        _ => Err("invalid media route"),
    }
}

fn valid_session_id(value: &str) -> bool {
    value.len() == 48
        && value
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
}

fn parse_audio_query(query: Option<&str>) -> Result<(f64, f64), &'static str> {
    let Some(query) = query else {
        return Err("audio chunk parameters are required");
    };
    if query.len() > 256 || query.is_empty() {
        return Err("invalid audio chunk parameters");
    }
    let mut start = None;
    let mut duration = None;
    for item in query.split('&') {
        let Some((name, value)) = item.split_once('=') else {
            return Err("invalid audio chunk parameters");
        };
        let parsed = value
            .parse::<f64>()
            .map_err(|_| "invalid audio chunk parameters")?;
        if !parsed.is_finite() {
            return Err("invalid audio chunk parameters");
        }
        match name {
            "start" if start.replace(parsed).is_none() => {}
            "duration" if duration.replace(parsed).is_none() => {}
            _ => return Err("invalid audio chunk parameters"),
        }
    }
    let (Some(start), Some(duration)) = (start, duration) else {
        return Err("audio chunk parameters are required");
    };
    if !(0.0..=MAXIMUM_AUDIO_START_SECONDS).contains(&start) || !(0.25..=20.0).contains(&duration) {
        return Err("audio chunk parameters are out of range");
    }
    Ok((start, duration))
}

fn trusted_protocol_uri(uri: &Uri) -> bool {
    match (uri.scheme_str(), uri.host()) {
        (Some(URI_SCHEME), Some("localhost")) => true,
        // WebView2 maps custom protocols to this HTTP origin internally.
        (Some("http" | "https"), Some("clipture-media.localhost")) => true,
        _ => false,
    }
}

fn single_range_header(request: &Request<Vec<u8>>) -> Result<Option<&str>, &'static str> {
    let mut values = request.headers().get_all(RANGE).iter();
    let Some(value) = values.next() else {
        return Ok(None);
    };
    if values.next().is_some() {
        return Err("multiple range headers are not supported");
    }
    let value = value
        .to_str()
        .map_err(|_| "range header is not valid text")?;
    if value.len() > MAXIMUM_RANGE_HEADER_BYTES {
        return Err("range header is too large");
    }
    Ok(Some(value))
}

fn video_response(
    mut plan: VideoStreamPlan,
    head_only: bool,
    range_was_requested: bool,
) -> Response<Vec<u8>> {
    plan.range = capped_range(plan.range, MAXIMUM_VIDEO_RESPONSE_BYTES);
    let partial = range_was_requested || plan.range.len() < plan.range.total;
    let body = if head_only {
        Vec::new()
    } else {
        match read_video_bytes(&plan) {
            Ok(bytes) => bytes,
            Err(_) => {
                return text_response(StatusCode::INTERNAL_SERVER_ERROR, "could not read media")
            }
        }
    };
    let status = if partial {
        StatusCode::PARTIAL_CONTENT
    } else {
        StatusCode::OK
    };
    let mut response = Response::builder()
        .status(status)
        .header(CONTENT_TYPE, plan.content_type)
        .header(CONTENT_LENGTH, plan.range.len().to_string())
        .header(ACCEPT_RANGES, "bytes")
        .header(CACHE_CONTROL, "no-store")
        .header(ACCESS_CONTROL_ALLOW_ORIGIN, "*");
    if partial {
        response = response.header(CONTENT_RANGE, plan.range.content_range());
    }
    response.body(body).unwrap_or_else(|_| fallback_response())
}

/// The source already bounded the chunk; always answer as a partial range so
/// the media element keeps issuing range requests instead of one huge read.
fn remote_video_response(mut chunk: RemoteVideoChunk, head_only: bool) -> Response<Vec<u8>> {
    if chunk.bytes.len() as u64 != chunk.range.len() {
        return text_response(StatusCode::INTERNAL_SERVER_ERROR, "could not read media");
    }
    // Same bound as local video: larger bodies cannot cross the UI pipe.
    let range = capped_range(chunk.range, MAXIMUM_VIDEO_RESPONSE_BYTES);
    chunk.bytes.truncate(range.len() as usize);
    chunk.range = range;
    let length = chunk.range.len();
    Response::builder()
        .status(StatusCode::PARTIAL_CONTENT)
        .header(CONTENT_TYPE, "video/mp4")
        .header(CONTENT_LENGTH, length.to_string())
        .header(CONTENT_RANGE, chunk.range.content_range())
        .header(ACCEPT_RANGES, "bytes")
        .header(CACHE_CONTROL, "no-store")
        .header(ACCESS_CONTROL_ALLOW_ORIGIN, "*")
        .body(if head_only { Vec::new() } else { chunk.bytes })
        .unwrap_or_else(|_| fallback_response())
}

fn capped_range(range: ByteRange, maximum_bytes: u64) -> ByteRange {
    if range.len() <= maximum_bytes {
        return range;
    }
    ByteRange {
        start: range.start,
        end_inclusive: range.start + maximum_bytes.saturating_sub(1),
        total: range.total,
    }
}

fn read_video_bytes(plan: &VideoStreamPlan) -> AppResult<Vec<u8>> {
    let length = usize::try_from(plan.range.len())
        .map_err(|_| AppError::Path("media range is too large".into()))?;
    if let Some(view) = &plan.view {
        let mut bytes = vec![0_u8; length];
        view.open_source()
            .and_then(|mut file| view.read_at(&mut file, plan.range.start, &mut bytes))
            .map_err(|source| AppError::Io {
                action: "read authorized playback media",
                path: plan.path.clone(),
                source,
            })?;
        return Ok(bytes);
    }
    let mut file = File::open(&plan.path).map_err(|source| AppError::Io {
        action: "open authorized playback media",
        path: plan.path.clone(),
        source,
    })?;
    file.seek(SeekFrom::Start(plan.range.start))
        .map_err(|source| AppError::Io {
            action: "seek authorized playback media",
            path: plan.path.clone(),
            source,
        })?;
    let mut bytes = vec![0_u8; length];
    file.read_exact(&mut bytes).map_err(|source| AppError::Io {
        action: "read authorized playback media",
        path: plan.path.clone(),
        source,
    })?;
    apply_patches(plan.range.start, &mut bytes, &plan.patches);
    Ok(bytes)
}

fn service_error_response(error: &AppError) -> Response<Vec<u8>> {
    match error {
        AppError::Path(message) if message.starts_with("invalid media byte range") => {
            text_response(
                StatusCode::RANGE_NOT_SATISFIABLE,
                "invalid media byte range",
            )
        }
        AppError::Path(_) | AppError::Io { .. } => {
            text_response(StatusCode::NOT_FOUND, "media session is unavailable")
        }
        _ => text_response(StatusCode::INTERNAL_SERVER_ERROR, "media processing failed"),
    }
}

fn preflight_response() -> Response<Vec<u8>> {
    Response::builder()
        .status(StatusCode::NO_CONTENT)
        .header(ACCESS_CONTROL_ALLOW_ORIGIN, "*")
        .header(ACCESS_CONTROL_ALLOW_METHODS, "GET, HEAD, OPTIONS")
        .header(ACCESS_CONTROL_ALLOW_HEADERS, "Range")
        .header(CACHE_CONTROL, "no-store")
        .body(Vec::new())
        .unwrap_or_else(|_| fallback_response())
}

fn method_not_allowed(allow: &'static str) -> Response<Vec<u8>> {
    Response::builder()
        .status(StatusCode::METHOD_NOT_ALLOWED)
        .header(ALLOW, allow)
        .header(ACCESS_CONTROL_ALLOW_ORIGIN, "*")
        .header(CACHE_CONTROL, "no-store")
        .body(b"method not allowed".to_vec())
        .unwrap_or_else(|_| fallback_response())
}

fn text_response(status: StatusCode, message: &'static str) -> Response<Vec<u8>> {
    binary_response(
        status,
        "text/plain; charset=utf-8",
        message.as_bytes().to_vec(),
    )
}

fn binary_response(
    status: StatusCode,
    content_type: &'static str,
    body: Vec<u8>,
) -> Response<Vec<u8>> {
    Response::builder()
        .status(status)
        .header(CONTENT_TYPE, content_type)
        .header(CONTENT_LENGTH, body.len().to_string())
        .header(CACHE_CONTROL, "no-store")
        .header(ACCESS_CONTROL_ALLOW_ORIGIN, "*")
        .body(body)
        .unwrap_or_else(|_| fallback_response())
}

fn fallback_response() -> Response<Vec<u8>> {
    Response::new(Vec::new())
}

struct RequestLimiter {
    active: Mutex<usize>,
    freed: Condvar,
    maximum: usize,
}

impl RequestLimiter {
    fn new(maximum: usize) -> Self {
        Self {
            active: Mutex::new(0),
            freed: Condvar::new(),
            maximum,
        }
    }

    fn try_acquire(self: &Arc<Self>) -> Option<RequestPermit> {
        self.acquire_within(Duration::ZERO)
    }

    fn acquire_within(self: &Arc<Self>, timeout: Duration) -> Option<RequestPermit> {
        let deadline = Instant::now() + timeout;
        let mut active = self
            .active
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        while *active >= self.maximum {
            let remaining = deadline.saturating_duration_since(Instant::now());
            if remaining.is_zero() {
                return None;
            }
            active = self
                .freed
                .wait_timeout(active, remaining)
                .unwrap_or_else(|poisoned| poisoned.into_inner())
                .0;
        }
        *active += 1;
        Some(RequestPermit(Arc::clone(self)))
    }
}

pub struct RequestPermit(Arc<RequestLimiter>);

impl Drop for RequestPermit {
    fn drop(&mut self) {
        let mut active = self
            .0
            .active
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        *active = active.saturating_sub(1);
        self.0.freed.notify_one();
    }
}

#[cfg(test)]
mod tests {
    use std::fs;

    use crate::{clips::PathAuthorizer, contracts::ClipRecord};

    use super::*;
    use crate::media::{
        FfmpegExecutor, FfmpegJob, FfmpegOutput, MediaSessionRegistry, PlaybackPatch,
    };

    struct FakeFfmpeg;

    impl FfmpegExecutor for FakeFfmpeg {
        fn run(&self, _: FfmpegJob) -> AppResult<FfmpegOutput> {
            Ok(FfmpegOutput {
                success: true,
                exit_code: Some(0),
                stdout: b"RIFF-test-wave".to_vec(),
                stderr: String::new(),
            })
        }
    }

    fn fixture() -> (tempfile::TempDir, Arc<MediaService>, String) {
        let root = tempfile::tempdir().unwrap();
        let video = root.path().join("clip.mp4");
        fs::write(&video, (0_u8..100).collect::<Vec<_>>()).unwrap();
        let authority = PathAuthorizer::from_records([ClipRecord {
            id: "clip".into(),
            file_path: video.to_string_lossy().into(),
            ..ClipRecord::default()
        }]);
        let sessions = Arc::new(MediaSessionRegistry::new("clipture-media://localhost").unwrap());
        let media = Arc::new(MediaService::new(sessions, Arc::new(FakeFfmpeg)));
        let session = media
            .open_playback(
                &authority,
                "clip",
                &["system".into(), "microphone".into()],
                "main",
            )
            .unwrap();
        (root, media, session.session_id)
    }

    #[test]
    fn busy_video_requests_queue_for_a_slot_instead_of_failing() {
        let limiter = Arc::new(RequestLimiter::new(1));
        let held = limiter.try_acquire().unwrap();
        assert!(limiter.try_acquire().is_none());
        let waiting = {
            let limiter = Arc::clone(&limiter);
            thread::spawn(move || limiter.acquire_within(Duration::from_secs(5)).is_some())
        };
        thread::sleep(Duration::from_millis(50));
        drop(held);
        assert!(waiting.join().unwrap(), "a queued request gets the freed slot");
        assert!(limiter
            .acquire_within(Duration::from_millis(20))
            .is_some());
    }

    #[test]
    fn admission_queues_video_but_fails_busy_audio_fast() {
        let token = "a".repeat(48);
        let video: Uri = format!("clipture-media://localhost/v1/session/{token}/video")
            .parse()
            .unwrap();
        let audio: Uri =
            format!("clipture-media://localhost/v1/session/{token}/audio?start=0&duration=8")
                .parse()
                .unwrap();
        let admission = Arc::new(MediaAdmission::new());
        let held_audio: Vec<_> = (0..MAXIMUM_IN_FLIGHT_AUDIO_REQUESTS)
            .map(|_| admission.admit(&audio).unwrap())
            .collect();
        assert!(admission.admit(&audio).is_none(), "busy audio fails fast");
        let mut held_video: Vec<_> = (0..MAXIMUM_IN_FLIGHT_VIDEO_REQUESTS)
            .map(|_| admission.admit(&video).unwrap())
            .collect();
        let waiting = {
            let admission = Arc::clone(&admission);
            thread::spawn(move || admission.admit(&video).is_some())
        };
        thread::sleep(Duration::from_millis(50));
        held_video.pop();
        assert!(waiting.join().unwrap(), "busy video waits for a freed slot");
        drop(held_audio);
    }

    #[test]
    fn route_requires_an_opaque_token_and_bounded_audio_query() {
        let token = "a".repeat(48);
        let video: Uri = format!("clipture-media://localhost/v1/session/{token}/video")
            .parse()
            .unwrap();
        assert!(matches!(parse_route(&video), Ok(Route::Video { .. })));

        let audio: Uri = format!(
            "clipture-media://localhost/v1/session/{token}/audio?start=8.000&duration=8.000"
        )
        .parse()
        .unwrap();
        assert!(matches!(parse_route(&audio), Ok(Route::Audio { .. })));

        let traversal: Uri = "clipture-media://localhost/v1/session/../../secret/video"
            .parse()
            .unwrap();
        assert!(parse_route(&traversal).is_err());
        assert!(parse_audio_query(Some("start=0&start=1&duration=8")).is_err());
        assert!(parse_audio_query(Some("start=0&duration=21")).is_err());
    }

    #[test]
    fn remote_replies_fit_through_the_ui_process_pipe() {
        // A larger body closes the window process (it rejects the frame).
        let total = 64 * 1024 * 1024;
        let chunk = RemoteVideoChunk {
            range: ByteRange {
                start: 10,
                end_inclusive: 10 + 8 * 1024 * 1024 - 1,
                total,
            },
            bytes: vec![7; 8 * 1024 * 1024],
        };
        let response = remote_video_response(chunk, false);
        assert_eq!(response.status(), StatusCode::PARTIAL_CONTENT);
        assert_eq!(response.body().len() as u64, MAXIMUM_VIDEO_RESPONSE_BYTES);
        // The UI process transport carries at most 4 MiB of body per frame.
        assert!(MAXIMUM_VIDEO_RESPONSE_BYTES <= 4 * 1024 * 1024);
        assert_eq!(
            response.headers()[CONTENT_RANGE],
            format!("bytes 10-{}/{total}", 10 + MAXIMUM_VIDEO_RESPONSE_BYTES - 1)
        );
    }

    #[test]
    fn remote_streams_accept_only_opaque_video_and_audio_routes() {
        let token = "b".repeat(48);
        let remote: Uri = format!("clipture-media://localhost/v1/remote/{token}/video")
            .parse()
            .unwrap();
        assert!(matches!(parse_route(&remote), Ok(Route::Remote { .. })));
        let audio: Uri = format!("clipture-media://localhost/v1/remote/{token}/audio?start=0&duration=8")
            .parse()
            .unwrap();
        assert!(matches!(parse_route(&audio), Ok(Route::RemoteAudio { .. })));
        for rejected in [
            format!("clipture-media://localhost/v1/remote/{token}/audio"),
            format!("clipture-media://localhost/v1/remote/{token}/audio?start=0&duration=8&path=x"),
            format!("clipture-media://localhost/v1/remote/{token}/video?path=C:/secret.mp4"),
            "clipture-media://localhost/v1/remote/short/video".to_owned(),
            format!("clipture-media://localhost/v1/other/{token}/video"),
        ] {
            let uri: Uri = rejected.parse().unwrap();
            assert!(parse_route(&uri).is_err(), "{rejected}");
        }
    }

    #[test]
    fn video_ranges_are_bounded_and_patches_apply_to_the_selected_span() {
        let root = tempfile::tempdir().unwrap();
        let path = root.path().join("clip.bin");
        fs::write(&path, (0_u8..16).collect::<Vec<_>>()).unwrap();
        let plan = VideoStreamPlan {
            path,
            content_type: "application/octet-stream",
            range: ByteRange {
                start: 4,
                end_inclusive: 11,
                total: 16,
            },
            patches: vec![PlaybackPatch {
                offset: 6,
                bytes: vec![90, 91],
            }],
            view: None,
        };
        assert_eq!(
            read_video_bytes(&plan).unwrap(),
            vec![4, 5, 90, 91, 8, 9, 10, 11]
        );

        let capped = capped_range(
            ByteRange {
                start: 5,
                end_inclusive: 100,
                total: 101,
            },
            10,
        );
        assert_eq!((capped.start, capped.end_inclusive), (5, 14));
    }

    #[test]
    fn handler_serves_ranges_and_enforces_the_creating_webview() {
        let (_root, media, session_id) = fixture();
        let uri = format!("clipture-media://localhost/v1/session/{session_id}/video");
        let request = Request::builder()
            .uri(&uri)
            .header(RANGE, "bytes=10-19")
            .body(Vec::new())
            .unwrap();
        let response = handle_request(Some(media.clone()), "main", request);
        assert_eq!(response.status(), StatusCode::PARTIAL_CONTENT);
        assert_eq!(response.headers()[CONTENT_RANGE], "bytes 10-19/100");
        assert_eq!(response.body(), &(10_u8..20).collect::<Vec<_>>());

        let request = Request::builder().uri(uri).body(Vec::new()).unwrap();
        let denied = handle_request(Some(media), "other-window", request);
        assert_eq!(denied.status(), StatusCode::NOT_FOUND);
    }

    #[test]
    fn scrambled_clips_are_served_in_playing_order() {
        let root = tempfile::tempdir().unwrap();
        let video = root.path().join("ring.mp4");
        crate::media::write_scrambled_clip(&video);
        let authority = PathAuthorizer::from_records([ClipRecord {
            id: "ring".into(),
            file_path: video.to_string_lossy().into(),
            ..ClipRecord::default()
        }]);
        let sessions = Arc::new(MediaSessionRegistry::new("clipture-media://localhost").unwrap());
        let media = Arc::new(MediaService::new(sessions, Arc::new(FakeFfmpeg)));
        let session = media.open_playback(&authority, "ring", &[], "main").unwrap();
        let view = crate::media::LinearView::open(&video, &[]).unwrap().unwrap();
        let mut expected = vec![0_u8; view.len() as usize];
        view.read_at(&mut view.open_source().unwrap(), 0, &mut expected).unwrap();

        let uri = format!("clipture-media://localhost/v1/session/{}/video", session.session_id);
        let request = Request::builder().uri(&uri).header(RANGE, "bytes=0-").body(Vec::new()).unwrap();
        let response = handle_request(Some(media.clone()), "main", request);
        assert_eq!(response.status(), StatusCode::PARTIAL_CONTENT);
        assert_eq!(response.headers()[CONTENT_RANGE], format!("bytes 0-{}/{}", view.len() - 1, view.len()));
        assert_eq!(response.body(), &expected);

        // A clip replaced while open is not served from the old layout.
        fs::write(&video, b"replaced").unwrap();
        let request = Request::builder().uri(&uri).body(Vec::new()).unwrap();
        assert_ne!(handle_request(Some(media), "main", request).status(), StatusCode::OK);
    }

    #[test]
    fn handler_serves_mixed_audio_without_exposing_a_path() {
        let (_root, media, session_id) = fixture();
        let uri = format!(
            "clipture-media://localhost/v1/session/{session_id}/audio?start=0.000&duration=8.000"
        );
        let request = Request::builder().uri(uri).body(Vec::new()).unwrap();
        let response = handle_request(Some(media), "main", request);
        assert_eq!(response.status(), StatusCode::OK);
        assert_eq!(response.headers()[CONTENT_TYPE], "audio/wav");
        assert_eq!(response.body(), b"RIFF-test-wave");
    }

    #[test]
    fn rejects_foreign_origins_before_session_lookup() {
        let request = Request::builder()
            .uri(format!(
                "clipture-media://evil.example/v1/session/{}/video",
                "a".repeat(48)
            ))
            .body(Vec::new())
            .unwrap();
        assert_eq!(
            handle_request(None, "main", request).status(),
            StatusCode::BAD_REQUEST
        );
    }
}
