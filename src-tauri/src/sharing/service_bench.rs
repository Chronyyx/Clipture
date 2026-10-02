//! Streaming benchmark against a real browser. Not part of the normal suite:
//!
//! ```text
//! $env:CLIPTURE_BENCH_CLIP = "C:\path\to\clip.mp4"
//! $env:CLIPTURE_BENCH_MBIT = "56"        # the sender's upload
//! $env:CLIPTURE_BENCH_SECONDS = "60"     # how long to watch
//! cargo test --lib streaming_bench -- --ignored --nocapture
//! ```
//!
//! Two real sharing nodes talk over QUIC on this PC; the sender's upload is
//! throttled to the given speed. The receiver's stream is served over local
//! HTTP, range for range as Clipture's player would get it, to headless Edge
//! (the engine Clipture's window uses), which plays it and reports how long
//! the first frame took and how often and how long it stalled.
use std::{
    io::{BufRead, BufReader, Read, Write},
    net::{TcpListener, TcpStream},
    path::PathBuf,
    sync::{atomic::Ordering, mpsc, Arc, Mutex},
    time::{Duration, Instant},
};

use super::tests::{befriend, online_pair, share, until};
use super::SharingService;
use crate::media::RemoteMediaSource;
use crate::sharing::server::test_link;

const EDGE: &str = r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe";

fn env_or(name: &str, default: &str) -> String {
    std::env::var(name).unwrap_or_else(|_| default.into())
}

#[tokio::test(flavor = "multi_thread", worker_threads = 8)]
#[ignore = "benchmark: needs CLIPTURE_BENCH_CLIP and Microsoft Edge"]
async fn streaming_bench() {
    let clip = PathBuf::from(std::env::var("CLIPTURE_BENCH_CLIP").expect("set CLIPTURE_BENCH_CLIP"));
    let mbit: f64 = env_or("CLIPTURE_BENCH_MBIT", "56").parse().unwrap();
    let watch: u64 = env_or("CLIPTURE_BENCH_SECONDS", "60").parse().unwrap();
    let size = std::fs::metadata(&clip).unwrap().len();
    test_link::BYTES_PER_SECOND.store((mbit * 1_000_000.0 / 8.0) as u64, Ordering::Relaxed);

    let (alice, bob, _lookup) = online_pair().await;
    befriend(&alice, &bob).await;
    let share_id = share(&alice, &bob.code(), &clip).await;
    let has_offer = |id: &str| bob.service.snapshot().inbox.iter().any(|c| c.share_id == id);
    until("bob has the offer", || has_offer(&share_id)).await;
    bob.service.answer_shared_clip(&share_id, true).unwrap();

    let service = bob.service.clone();
    let id = share_id.clone();
    let stream = tokio::task::spawn_blocking(move || service.open_stream("main", &id).unwrap())
        .await
        .unwrap();

    let started = Instant::now();
    let requests = Arc::new(Mutex::new(Vec::<(u128, u64, u64)>::new()));
    let (reports, report) = mpsc::channel::<String>();
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = listener.local_addr().unwrap().port();
    {
        let service = bob.service.clone();
        let requests = requests.clone();
        std::thread::spawn(move || {
            for connection in listener.incoming().flatten() {
                let service = service.clone();
                let stream = stream.clone();
                let requests = requests.clone();
                let reports = reports.clone();
                std::thread::spawn(move || {
                    handle(connection, &service, &stream, watch, started, &requests, &reports)
                });
            }
        });
    }

    let profile = tempfile::tempdir().unwrap();
    let mut edge = std::process::Command::new(EDGE)
        .args([
            "--headless=new",
            "--autoplay-policy=no-user-gesture-required",
            "--mute-audio",
            "--no-first-run",
            &format!("--user-data-dir={}", profile.path().display()),
            &format!("http://127.0.0.1:{port}/"),
        ])
        .spawn()
        .expect("start Microsoft Edge");
    let outcome = tokio::task::spawn_blocking(move || {
        report.recv_timeout(Duration::from_secs(watch + 90))
    })
    .await
    .unwrap();
    let _ = edge.kill();
    let _ = edge.wait();

    let arrived: u64 = bob
        .service
        .snapshot()
        .inbox
        .iter()
        .find(|c| c.share_id == share_id)
        .map(|c| c.streamed.iter().map(|[a, b]| b - a).sum())
        .unwrap_or(0);
    let requests = requests.lock().unwrap();
    println!("\n=== streaming bench ===");
    println!("clip: {} ({} MB), upload {mbit} Mbit/s, watched {watch} s", clip.display(), size / 1_000_000);
    println!("player report: {}", outcome.as_deref().unwrap_or("NONE (timed out)"));
    println!("range requests: {}", requests.len());
    println!(
        "streamed into memory: {} MB of {} MB",
        arrived / 1_000_000,
        size / 1_000_000
    );
    if let Ok(path) = std::env::var("CLIPTURE_BENCH_LOG") {
        let lines: String = requests
            .iter()
            .map(|(at, start, length)| format!("{at}	{start}	{length}
"))
            .collect();
        std::fs::write(path, lines).unwrap();
    }
    test_link::BYTES_PER_SECOND.store(0, Ordering::Relaxed);
    assert!(outcome.is_ok(), "the player never reported");
}

fn handle(
    mut connection: TcpStream,
    service: &Arc<SharingService>,
    stream: &str,
    watch: u64,
    started: Instant,
    requests: &Mutex<Vec<(u128, u64, u64)>>,
    reports: &mpsc::Sender<String>,
) {
    let mut reader = BufReader::new(connection.try_clone().unwrap());
    let mut line = String::new();
    if reader.read_line(&mut line).is_err() {
        return;
    }
    let mut parts = line.split_whitespace();
    let (method, path) = (parts.next().unwrap_or(""), parts.next().unwrap_or(""));
    let (mut range, mut length) = (None::<String>, 0_usize);
    loop {
        let mut header = String::new();
        if reader.read_line(&mut header).is_err() || header.trim().is_empty() {
            break;
        }
        let lower = header.to_ascii_lowercase();
        if lower.starts_with("range:") {
            range = Some(header[6..].trim().to_owned());
        } else if let Some(value) = lower.strip_prefix("content-length:") {
            length = value.trim().parse().unwrap_or(0);
        }
    }
    let respond = |connection: &mut TcpStream, status: &str, headers: &str, body: &[u8]| {
        let head = format!(
            "HTTP/1.1 {status}\r\n{headers}Content-Length: {}\r\nConnection: close\r\n\r\n",
            body.len()
        );
        let _ = connection.write_all(head.as_bytes());
        let _ = connection.write_all(body);
    };
    match (method, path) {
        ("GET", "/") => respond(&mut connection, "200 OK", "Content-Type: text/html\r\n", page(watch).as_bytes()),
        ("POST", "/report") => {
            let mut body = vec![0; length];
            let _ = reader.read_exact(&mut body);
            let _ = reports.send(String::from_utf8_lossy(&body).into_owned());
            respond(&mut connection, "204 No Content", "", b"");
        }
        ("GET" | "HEAD", "/clip") => {
            let header = range.unwrap_or_else(|| "bytes=0-".into());
            match service.read_video(stream, "main", Some(&header)) {
                Ok(chunk) => {
                    requests.lock().unwrap().push((
                        started.elapsed().as_millis(),
                        chunk.range.start,
                        chunk.range.len(),
                    ));
                    let headers = format!(
                        "Content-Type: video/mp4\r\nAccept-Ranges: bytes\r\nContent-Range: {}\r\n",
                        chunk.range.content_range()
                    );
                    let body = if method == "HEAD" { Vec::new() } else { chunk.bytes };
                    respond(&mut connection, "206 Partial Content", &headers, &body);
                }
                Err(error) => respond(&mut connection, "503 Service Unavailable", "", error.to_string().as_bytes()),
            }
        }
        _ => respond(&mut connection, "404 Not Found", "", b""),
    }
}

/// Plays the clip and posts what a viewer would have experienced.
fn page(watch: u64) -> String {
    format!(
        r#"<!doctype html><video src="/clip" autoplay muted preload="auto"></video><script>
const v = document.querySelector('video'), t0 = performance.now();
let first = null, stalls = 0, stalled = 0, since = null, sent = false, log = [];
const now = () => performance.now() - t0;
v.addEventListener('playing', () => {{
  if (first === null) first = now();
  if (since !== null) {{ stalled += now() - since; log.push([Math.round(since), Math.round(now() - since), +v.currentTime.toFixed(2)]); since = null; }}
}});
v.addEventListener('waiting', () => {{ if (first !== null && since === null) {{ stalls++; since = now(); }} }});
v.addEventListener('error', () => report('error ' + (v.error && v.error.code)));
v.addEventListener('ended', () => report('ended'));
setTimeout(() => report('watched'), {watch} * 1000);
function report(reason) {{
  if (sent) return; sent = true;
  if (since !== null) stalled += now() - since;
  fetch('/report', {{ method: 'POST', body: JSON.stringify({{
    reason, firstFrameMs: first === null ? null : Math.round(first), stalls,
    stalledMs: Math.round(stalled), playedSeconds: +v.currentTime.toFixed(1),
    wallSeconds: +(now() / 1000).toFixed(1), duration: v.duration,
    stallLog: log
  }}) }});
}}
</script>"#
    )
}
