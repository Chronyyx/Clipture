//! Bounded, capture-aware installer transfer. The installer is streamed to a
//! temporary file and authenticated before it can become an install candidate.
use super::{
    model::UpdateOperation,
    service::{UpdateError, UpdateGate},
};
use crate::contracts::CapturePressure;
use base64::Engine;
use minisign_verify::{PublicKey, Signature};
use reqwest::{
    header::{HeaderMap, HeaderValue, ACCEPT},
    Url,
};
use std::{io::Write, time::Duration};
use tauri_plugin_updater::Update;
use tempfile::NamedTempFile;
use tokio::{io::AsyncWriteExt, time::Instant};

const MAXIMUM_PAYLOAD: u64 = 512 * 1024 * 1024;
const MAXIMUM_WRITE: usize = 512 * 1024;

pub async fn download(
    update: &Update,
    public_key: &str,
    gate: &dyn UpdateGate,
    progress: impl FnMut(usize, Option<u64>),
) -> Result<NamedTempFile, UpdateError> {
    let mut builder = client_builder()
        .user_agent("Clipture-Tauri-Updater")
        .connect_timeout(Duration::from_secs(30))
        .timeout(update.timeout.unwrap_or(Duration::from_secs(1800)))
        .redirect(reqwest::redirect::Policy::custom(|attempt| {
            if attempt.previous().len() >= 10 {
                attempt.error("too many redirects")
            } else if attempt.url().scheme() == "https" {
                attempt.follow()
            } else {
                attempt.error("update redirects must use HTTPS")
            }
        }));
    if update.no_proxy {
        builder = builder.no_proxy();
    } else if let Some(proxy) = &update.proxy {
        builder = builder.proxy(reqwest::Proxy::all(proxy.as_str())?);
    }
    let client = builder.build()?;
    transfer(
        &client,
        &update.download_url,
        update.headers.clone(),
        public_key,
        &update.signature,
        gate,
        progress,
    )
    .await
}

fn client_builder() -> reqwest::ClientBuilder {
    if rustls::crypto::CryptoProvider::get_default().is_none() {
        let _ = rustls::crypto::ring::default_provider().install_default();
    }
    let builder = reqwest::Client::builder();
    #[cfg(debug_assertions)]
    return super::smoke::configure_client(builder);
    #[cfg(not(debug_assertions))]
    builder
}

async fn transfer(
    client: &reqwest::Client,
    url: &Url,
    mut headers: HeaderMap,
    public_key: &str,
    signature: &str,
    gate: &dyn UpdateGate,
    mut progress: impl FnMut(usize, Option<u64>),
) -> Result<NamedTempFile, UpdateError> {
    let local_test = cfg!(test) && url.host_str() == Some("127.0.0.1");
    if url.scheme() != "https" && !local_test {
        return Err(UpdateError::Transfer(
            "update downloads require HTTPS".into(),
        ));
    }
    let (key, signature) = signing_material(public_key, signature)?;
    // Tauri's current signing tool produces prehashed minisign signatures.
    // Legacy non-streamable signatures fail closed, never bypass verification.
    let mut verifier = key.verify_stream(&signature).map_err(signature_error)?;
    wait_until_allowed(gate).await?;
    if !headers.contains_key(ACCEPT) {
        headers.insert(ACCEPT, HeaderValue::from_static("application/octet-stream"));
    }
    let mut response = client
        .get(url.clone())
        .headers(headers)
        .send()
        .await?
        .error_for_status()?;
    let total = response.content_length();
    if total.is_some_and(|size| size > MAXIMUM_PAYLOAD) {
        return Err(UpdateError::Transfer(
            "update exceeds the 512 MiB payload limit".into(),
        ));
    }
    let temporary = NamedTempFile::new()?;
    let mut file = tokio::fs::File::from_std(temporary.as_file().try_clone()?);
    let mut received = 0_u64;
    loop {
        wait_until_allowed(gate).await?;
        let Some(chunk) = response.chunk().await? else {
            break;
        };
        received = received
            .checked_add(chunk.len() as u64)
            .filter(|size| *size <= MAXIMUM_PAYLOAD)
            .ok_or_else(|| {
                UpdateError::Transfer("update exceeds the 512 MiB payload limit".into())
            })?;
        for piece in chunk.chunks(MAXIMUM_WRITE) {
            wait_until_allowed(gate).await?;
            let started = Instant::now();
            file.write_all(piece).await?;
            verifier.update(piece);
            progress(piece.len(), total);
            let budget = Duration::from_secs_f64(
                piece.len() as f64 / transfer_rate(gate.capture_pressure()) as f64,
            );
            if let Some(delay) = budget.checked_sub(started.elapsed()) {
                tokio::time::sleep(delay).await;
            }
        }
    }
    if total.is_some_and(|size| size != received) {
        return Err(UpdateError::Transfer(
            "update body length does not match Content-Length".into(),
        ));
    }
    verifier.finalize().map_err(signature_error)?;
    file.flush().await?;
    file.sync_all().await?;
    drop(file);
    Ok(temporary)
}

async fn wait_until_allowed(gate: &dyn UpdateGate) -> Result<(), UpdateError> {
    let deadline = Instant::now() + Duration::from_secs(1800);
    while let Some(reason) = gate.block_reason(UpdateOperation::Download) {
        if Instant::now() >= deadline {
            return Err(UpdateError::Blocked(reason));
        }
        tokio::time::sleep(Duration::from_millis(250)).await;
    }
    Ok(())
}

fn transfer_rate(pressure: CapturePressure) -> u64 {
    // Conservative ceilings from the legacy updater. React to live pressure
    // before every bounded write; do not raise the controller's process priority.
    match pressure {
        CapturePressure::Healthy => 32 * 1024 * 1024,
        CapturePressure::Elevated => 16 * 1024 * 1024,
        CapturePressure::Critical | CapturePressure::Unknown => 4 * 1024 * 1024,
    }
}

fn signing_material(key: &str, signature: &str) -> Result<(PublicKey, Signature), UpdateError> {
    let decode = |value: &str| -> Result<String, UpdateError> {
        let bytes = base64::engine::general_purpose::STANDARD
            .decode(value)
            .map_err(|_| UpdateError::Transfer("invalid signing envelope".into()))?;
        String::from_utf8(bytes)
            .map_err(|_| UpdateError::Transfer("signing envelope is not UTF-8".into()))
    };
    Ok((
        PublicKey::decode(&decode(key)?).map_err(signature_error)?,
        Signature::decode(&decode(signature)?).map_err(signature_error)?,
    ))
}
fn signature_error(error: minisign_verify::Error) -> UpdateError {
    UpdateError::Transfer(format!("update signature verification failed: {error}"))
}
pub fn verify_before_install(
    bytes: &[u8],
    public_key: &str,
    signature: &str,
) -> Result<(), UpdateError> {
    let (key, signature) = signing_material(public_key, signature)?;
    key.verify(bytes, &signature, false)
        .map_err(signature_error)
}

#[cfg(test)]
mod tests {
    use super::super::service::AllowUpdates;
    use super::*;
    use std::{
        io::Read,
        net::TcpListener,
        sync::atomic::{AtomicBool, Ordering},
    };

    // Public minisign-verify 0.2.5 interoperability vector, signed payload "test".
    // These are a PUBLIC key and signature; no private signing material is used.
    fn material() -> (String, String) {
        let key = "untrusted comment: minisign public key\nRWQf6LRCGA9i53mlYecO4IzT51TGPpvWucNSCh1CBM0QTaLn73Y7GFO3";
        let signature = "untrusted comment: signature from minisign secret key\nRUQf6LRCGA9i559r3g7V1qNyJDApGip8MfqcadIgT9CuhV3EMhHoN1mGTkUidF/z7SrlQgXdy8ofjb7bNJJylDOocrCo8KLzZwo=\ntrusted comment: timestamp:1556193335\tfile:test\ny/rUw2y8/hOUYjZU71eHp/Wo1KZ40fGy2VJEDl34XMJM+TX48Ss/17u3IvIfbVR1FkZZSNCisQbuQY+bHwhEBg==";
        let encode = |text: &str| base64::engine::general_purpose::STANDARD.encode(text);
        (encode(key), encode(signature))
    }

    fn server(body: &'static [u8], length: u64) -> (Url, std::thread::JoinHandle<()>) {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let url = Url::parse(&format!(
            "http://{}/fixture",
            listener.local_addr().unwrap()
        ))
        .unwrap();
        let task = std::thread::spawn(move || {
            let (mut stream, _) = listener.accept().unwrap();
            stream
                .set_read_timeout(Some(Duration::from_secs(5)))
                .unwrap();
            let _ = stream.read(&mut [0; 4096]);
            let header =
                format!("HTTP/1.1 200 OK\r\nContent-Length: {length}\r\nConnection: close\r\n\r\n");
            let _ = stream.write_all(header.as_bytes());
            let _ = stream.write_all(body);
        });
        (url, task)
    }

    #[test]
    fn streams_valid_payload_and_rejects_tampering_and_oversized_downloads() {
        let (key, signature) = material();
        tauri::async_runtime::block_on(async {
            let client = client_builder().no_proxy().build().unwrap();
            for (body, length, succeeds) in [
                (b"test" as &'static [u8], 4, true),
                (b"Test", 4, false),
                (b"test", MAXIMUM_PAYLOAD + 1, false),
                (b"tes", 4, false),
            ] {
                let (url, task) = server(body, length);
                let staged = transfer(
                    &client,
                    &url,
                    HeaderMap::new(),
                    &key,
                    &signature,
                    &AllowUpdates,
                    |size, _| assert!(size <= MAXIMUM_WRITE),
                )
                .await;
                assert_eq!(staged.is_ok(), succeeds, "{staged:?}");
                if let Ok(file) = staged {
                    assert_eq!(std::fs::read(file.path()).unwrap(), b"test");
                }
                task.join().unwrap();
            }
        });
        assert!(verify_before_install(b"test", &key, &signature).is_ok());
        assert!(verify_before_install(b"Test", &key, &signature).is_err());
        assert!(verify_before_install(b"test", "not base64", &signature).is_err());
    }

    #[test]
    fn live_pressure_changes_rates_and_a_save_pauses_then_resumes() {
        struct Gate(AtomicBool);
        impl UpdateGate for Gate {
            fn block_reason(&self, _: UpdateOperation) -> Option<String> {
                self.0.load(Ordering::Acquire).then(|| "saving".into())
            }
        }
        assert_eq!(transfer_rate(CapturePressure::Healthy), 32 * 1024 * 1024);
        assert_eq!(transfer_rate(CapturePressure::Elevated), 16 * 1024 * 1024);
        assert_eq!(transfer_rate(CapturePressure::Critical), 4 * 1024 * 1024);
        let gate = std::sync::Arc::new(Gate(AtomicBool::new(true)));
        let release = gate.clone();
        let task = std::thread::spawn(move || {
            std::thread::sleep(Duration::from_millis(80));
            release.0.store(false, Ordering::Release);
        });
        let started = std::time::Instant::now();
        tauri::async_runtime::block_on(wait_until_allowed(gate.as_ref())).unwrap();
        assert!(started.elapsed() >= Duration::from_millis(80));
        task.join().unwrap();
    }
}
