//! Sends sealed journal segments, oldest first, one signed request each
//! (ADR 0035, "Transport"). A segment is deleted only once the server has it.
use super::{enrollment::EnrollmentState, journal::SEAL_AGE, now_ms, now_text, TelemetryService};
use clipture_telemetry_protocol::{
    batch_message, body_digest, BatchResponse, EnrollRequest, EnrollResponse, BATCHES_PATH, DEVICE_HEADER, ENROLL_PATH,
    SIGNATURE_HEADER, TIMESTAMP_HEADER,
};
use std::{io::Write, sync::Arc, time::Duration};
use tokio::time::Instant;

pub const LAST_BATCH_FILE: &str = "last-batch.jsonl";
const TICK: Duration = Duration::from_secs(10);
const FIRST_BACKOFF: Duration = Duration::from_secs(30);
const MAXIMUM_BACKOFF: Duration = Duration::from_secs(30 * 60);

pub fn client() -> reqwest::Result<reqwest::Client> {
    if rustls::crypto::CryptoProvider::get_default().is_none() {
        let _ = rustls::crypto::ring::default_provider().install_default();
    }
    reqwest::Client::builder()
        .user_agent(concat!("Clipture-Telemetry/", env!("CARGO_PKG_VERSION")))
        .connect_timeout(Duration::from_secs(15))
        .timeout(Duration::from_secs(60))
        // A redirect would re-send a signed body somewhere else.
        .redirect(reqwest::redirect::Policy::none())
        .build()
}

async fn error_text(response: reqwest::Response) -> String {
    let status = response.status();
    let body: serde_json::Value = response.json().await.unwrap_or_default();
    body["error"].as_str().map(str::to_string).unwrap_or_else(|| format!("The server answered {status}."))
}

pub async fn enroll(client: &reqwest::Client, server: &str, request: &EnrollRequest) -> Result<EnrollResponse, String> {
    let response = client
        .post(format!("{server}{ENROLL_PATH}"))
        .json(request)
        .send()
        .await
        .map_err(|error| format!("Could not reach the telemetry server: {}", without_url(&error)))?;
    if !response.status().is_success() {
        return Err(error_text(response).await);
    }
    response.json().await.map_err(|_| "The telemetry server sent an unexpected answer.".to_string())
}

/// reqwest errors name the URL; the UI already shows the server.
fn without_url(error: &reqwest::Error) -> String {
    let mut text = error.to_string();
    if let Some(url) = error.url() {
        text = text.replace(&format!(" for url ({url})"), "").replace(url.as_str(), "the server");
    }
    if let Some(source) = std::error::Error::source(error) {
        text = format!("{text} ({source})");
    }
    text
}

fn gzip(bytes: &[u8]) -> std::io::Result<Vec<u8>> {
    let mut encoder = flate2::write::GzEncoder::new(Vec::new(), flate2::Compression::default());
    encoder.write_all(bytes)?;
    encoder.finish()
}

enum Outcome {
    Sent,
    /// The server will never take it (400/413): drop it, record a gap.
    Rejected(String),
    Revoked,
    Retry(String),
}

async fn send(client: &reqwest::Client, server: &str, device: &str, key: &clipture_telemetry_protocol::DeviceKey, plain: &[u8]) -> Outcome {
    let body = match gzip(plain) {
        Ok(body) => body,
        Err(error) => return Outcome::Retry(error.to_string()),
    };
    let timestamp = now_ms();
    let signature = key.sign(&batch_message(device, timestamp, &body_digest(&body)));
    let response = client
        .post(format!("{server}{BATCHES_PATH}"))
        .header(DEVICE_HEADER, device)
        .header(TIMESTAMP_HEADER, timestamp.to_string())
        .header(SIGNATURE_HEADER, signature)
        .header(reqwest::header::CONTENT_TYPE, "application/x-ndjson")
        .header(reqwest::header::CONTENT_ENCODING, "gzip")
        .body(body)
        .send()
        .await;
    let response = match response {
        Ok(response) => response,
        Err(error) => return Outcome::Retry(format!("Could not reach the telemetry server: {}", without_url(&error))),
    };
    let status = response.status().as_u16();
    match status {
        200..=299 => {
            let _ = response.json::<BatchResponse>().await;
            Outcome::Sent
        }
        401 | 403 => Outcome::Revoked,
        400 | 413 => Outcome::Rejected(error_text(response).await),
        _ => Outcome::Retry(error_text(response).await),
    }
}

pub async fn run(service: Arc<TelemetryService>) {
    let mut retry_at: Option<Instant> = None;
    let mut backoff = FIRST_BACKOFF;
    loop {
        let urgent = tokio::select! {
            _ = tokio::time::sleep(TICK) => false,
            _ = service.urgent.notified() => true,
        };
        if urgent {
            // Let the records around an error land in the same batch.
            tokio::time::sleep(Duration::from_secs(2)).await;
        }
        service.tick();
        if retry_at.is_some_and(|at| Instant::now() < at) {
            continue;
        }
        // Seal what is due, then send everything sealed.
        let work = {
            let mut inner = service.lock();
            let Some(enrollment) = inner.enrollment.clone().filter(|e| e.state == EnrollmentState::Active) else { continue };
            let Some(journal) = inner.journal.as_mut() else { continue };
            if journal.open_age().is_some_and(|age| urgent || age >= SEAL_AGE) {
                let _ = journal.seal();
            }
            (enrollment, journal.sealed())
        };
        let (enrollment, segments) = work;
        if segments.is_empty() {
            continue;
        }
        let Some(key) = enrollment.key() else {
            service.lock().last_error = Some("The device key cannot be read on this account. Stop telemetry and ask for a new invite.".into());
            continue;
        };
        let Ok(client) = service.client().cloned() else { continue };
        for segment in segments {
            let Ok(plain) = std::fs::read(&segment.path) else { continue };
            match send(&client, &enrollment.server, &enrollment.device_id, &key, &plain).await {
                Outcome::Sent => {
                    let _ = std::fs::write(service.root.join(LAST_BATCH_FILE), &plain);
                    let mut inner = service.lock();
                    if let Some(journal) = &inner.journal {
                        let _ = journal.remove_sent(&segment);
                    }
                    inner.last_upload_at = Some(now_text());
                    inner.last_upload_records = plain.split(|byte| *byte == b'\n').filter(|line| !line.is_empty()).count() as u64;
                    inner.last_error = None;
                    retry_at = None;
                    backoff = FIRST_BACKOFF;
                }
                Outcome::Rejected(reason) => {
                    let mut inner = service.lock();
                    if let Some(journal) = &mut inner.journal {
                        let _ = journal.drop_segment(&segment, &format!("rejected: {reason}"));
                    }
                    inner.last_error = Some(format!("The server rejected a batch: {reason}"));
                }
                Outcome::Revoked => {
                    let mut inner = service.lock();
                    if let Some(enrollment) = &mut inner.enrollment {
                        enrollment.state = EnrollmentState::Revoked;
                        let _ = enrollment.save(&service.root);
                    }
                    // Nothing more will ever be accepted from this key.
                    inner.journal = None;
                    let _ = std::fs::remove_dir_all(service.root.join("journal"));
                    inner.last_error = Some("The server no longer accepts this device. Ask for a new invite to continue.".into());
                    break;
                }
                Outcome::Retry(reason) => {
                    service.lock().last_error = Some(reason);
                    retry_at = Some(Instant::now() + backoff);
                    backoff = (backoff * 2).min(MAXIMUM_BACKOFF);
                    break;
                }
            }
        }
    }
}
