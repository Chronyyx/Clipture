use super::{
    manifest::{invalid, Manifest, RuntimeFile, SignedManifest, MANIFEST_LIMIT},
    service::{UpdateError, UpdateGate},
    stream_download::{transfer_rate, wait_until_allowed},
};
use std::{path::Path, time::Duration};
use tokio::{io::AsyncWriteExt, time::Instant};

pub const METADATA_URL: &str = "https://api.github.com/repos/Chronyyx/Clipture/releases/latest";
pub const ASSET_BASE: &str = "https://github.com/Chronyyx/Clipture/releases/download";

pub fn client() -> Result<reqwest::Client, UpdateError> {
    if rustls::crypto::CryptoProvider::get_default().is_none() {
        let _ = rustls::crypto::ring::default_provider().install_default();
    }
    Ok(reqwest::Client::builder()
        .user_agent("Clipture-Native-Updater")
        .connect_timeout(Duration::from_secs(30))
        .timeout(Duration::from_secs(1800))
        .redirect(reqwest::redirect::Policy::custom(|attempt| {
            if attempt.previous().len() >= 10 || attempt.url().scheme() != "https" {
                attempt.error("Unsafe update redirect")
            } else {
                attempt.follow()
            }
        }))
        .build()?)
}

async fn bounded(client: &reqwest::Client, url: &reqwest::Url, limit: usize) -> Result<Vec<u8>, UpdateError> {
    let mut response = client.get(url.clone()).send().await?.error_for_status()?;
    if response.content_length().is_some_and(|length| length > limit as u64) {
        return Err(invalid("Update metadata is too large"));
    }
    let mut bytes = Vec::new();
    while let Some(chunk) = response.chunk().await? {
        if bytes.len().saturating_add(chunk.len()) > limit {
            return Err(invalid("Update metadata is too large"));
        }
        bytes.extend_from_slice(&chunk);
    }
    Ok(bytes)
}

pub async fn check(key: &str, current: &str) -> Result<Option<(SignedManifest, Manifest)>, UpdateError> {
    let client = client()?;
    let metadata = reqwest::Url::parse(METADATA_URL).map_err(|_| invalid("Invalid update metadata URL"))?;
    if metadata.scheme() != "https" && !(cfg!(test) && metadata.host_str() == Some("127.0.0.1")) {
        return Err(invalid("Update metadata requires HTTPS"));
    }
    let release: serde_json::Value = serde_json::from_slice(&bounded(&client, &metadata, 1024 * 1024).await?)
        .map_err(|error| invalid(format!("Invalid release metadata: {error}")))?;
    let tag = release["tag_name"].as_str().ok_or_else(|| invalid("Release tag missing"))?;
    let version = tag.strip_prefix('v').ok_or_else(|| invalid("Invalid release tag"))?;
    let candidate = semver::Version::parse(version).map_err(|_| invalid("Invalid release version"))?;
    let running = semver::Version::parse(current).map_err(|_| invalid("Invalid running version"))?;
    if candidate <= running {
        return Ok(None);
    }
    if release["draft"].as_bool() != Some(false) || release["prerelease"].as_bool() != Some(false) {
        return Err(invalid("Release is not stable"));
    }
    let asset = |name: &str| -> Result<String, UpdateError> {
        let expected = format!("{ASSET_BASE}/{tag}/{name}");
        let assets = release["assets"].as_array().ok_or_else(|| invalid("Release assets missing"))?;
        if assets
            .iter()
            .filter(|asset| asset["name"] == name && asset["browser_download_url"] == expected)
            .count()
            != 1
        {
            return Err(invalid(format!("Signed runtime asset {name} is missing or ambiguous")));
        }
        Ok(expected)
    };
    let result = fetch_manifest(&client, &asset("components-v1.json")?, &asset("components-v1.json.sig")?, key, current).await?;
    if result.1.version != version {
        return Err(invalid("Signed manifest version differs from the release tag"));
    }
    Ok(Some(result))
}

pub async fn fetch_manifest(
    client: &reqwest::Client, manifest_url: &str, signature_url: &str, key: &str, current: &str,
) -> Result<(SignedManifest, Manifest), UpdateError> {
    let manifest_url = reqwest::Url::parse(manifest_url).map_err(|_| invalid("Invalid manifest URL"))?;
    if manifest_url.scheme() != "https" && !(cfg!(test) && manifest_url.host_str() == Some("127.0.0.1")) {
        return Err(invalid("Signed manifest requires HTTPS"));
    }
    let signature_url = reqwest::Url::parse(signature_url).map_err(|_| invalid("Invalid signature URL"))?;
    if signature_url.scheme() != "https" && !(cfg!(test) && signature_url.host_str() == Some("127.0.0.1")) {
        return Err(invalid("Manifest signature requires HTTPS"));
    }
    {
        let bytes = bounded(client, &manifest_url, MANIFEST_LIMIT).await?;
        let signature = bounded(client, &signature_url, 4096).await?;
        let signed = SignedManifest {
            manifest: String::from_utf8(bytes).map_err(|_| invalid("Manifest is not UTF-8"))?,
            signature: String::from_utf8(signature).map_err(|_| invalid("Signature is not UTF-8"))?,
        };
        let manifest = signed.verify(key)?;
        if !manifest.newer_than(current) {
            return Err(invalid("Signed manifest does not advance the installed version"));
        }
        Ok((signed, manifest))
    }
}

pub async fn download_differential(
    client: &reqwest::Client, expected: &RuntimeFile, source: &Path, destination: &Path,
    gate: &dyn UpdateGate, progress: &mut impl FnMut(u64),
) -> Result<(), UpdateError> {
    use sha2::{Digest, Sha256};
    use tokio::io::{AsyncReadExt, AsyncSeekExt};
    let mut reusable = std::collections::HashMap::new();
    let mut old = match std::fs::symlink_metadata(source) {
        Ok(metadata) if metadata.is_file() && !super::manifest::is_reparse(&metadata)
            && metadata.len() <= super::manifest::FILE_LIMIT => Some(tokio::fs::File::open(source).await?),
        _ => None,
    };
    let mut buffer = vec![0; expected.block_size as usize];
    if let Some(old) = old.as_mut() {
        let length = old.metadata().await?.len();
        let mut offset = 0;
        while offset < length {
            wait_until_allowed(gate).await?;
            let size = (length - offset).min(expected.block_size) as usize;
            old.read_exact(&mut buffer[..size]).await?;
            reusable.insert(format!("{:x}", Sha256::digest(&buffer[..size])), (offset, size));
            offset += size as u64;
            tokio::task::yield_now().await;
        }
    }
    if !expected.blocks.iter().any(|hash| reusable.contains_key(hash)) {
        return download_file(client, expected, destination, gate, progress).await;
    }
    let url = reqwest::Url::parse(&expected.url).map_err(|_| invalid("Invalid runtime URL"))?;
    if url.scheme() != "https" && !(cfg!(test) && url.host_str() == Some("127.0.0.1")) {
        return Err(invalid("Runtime transfers require HTTPS"));
    }
    let mut output = tokio::fs::OpenOptions::new().write(true).create_new(true).open(destination).await?;
    let mut completed = 0;
    for (index, hash) in expected.blocks.iter().enumerate() {
        wait_until_allowed(gate).await?;
        let start = index as u64 * expected.block_size;
        let length = (expected.size - start).min(expected.block_size) as usize;
        let mut reused = false;
        if let (Some(old), Some((offset, size))) = (old.as_mut(), reusable.get(hash)) {
            if *size == length {
                old.seek(std::io::SeekFrom::Start(*offset)).await?;
                if old.read_exact(&mut buffer[..length]).await.is_ok()
                    && format!("{:x}", Sha256::digest(&buffer[..length])) == *hash {
                    reused = true;
                }
            }
        }
        if !reused {
            let end = start + length as u64 - 1;
            let mut response = client.get(url.clone()).header(reqwest::header::RANGE, format!("bytes={start}-{end}"))
                .header(reqwest::header::ACCEPT_ENCODING, "identity").send().await?.error_for_status()?;
            if response.status() == reqwest::StatusCode::OK {
                drop(output);
                tokio::fs::remove_file(destination).await?;
                let mut position = 0_u64;
                return write_response(response, expected, destination, gate, &mut |size| {
                    let previous = position;
                    position += size;
                    progress(position.saturating_sub(completed) - previous.saturating_sub(completed));
                }).await;
            }
            let content_range = format!("bytes {start}-{end}/{}", expected.size);
            if response.status() != reqwest::StatusCode::PARTIAL_CONTENT
                || response.headers().get(reqwest::header::CONTENT_RANGE).and_then(|value| value.to_str().ok()) != Some(content_range.as_str())
                || response.content_length().is_some_and(|size| size != length as u64) {
                return Err(invalid("Invalid runtime block range response"));
            }
            let mut received = 0;
            while let Some(chunk) = response.chunk().await? {
                wait_until_allowed(gate).await?;
                if received + chunk.len() > length { return Err(invalid("Oversized runtime block")); }
                let started = Instant::now();
                buffer[received..received + chunk.len()].copy_from_slice(&chunk);
                received += chunk.len();
                let budget = Duration::from_secs_f64(chunk.len() as f64 / transfer_rate(gate.capture_pressure()) as f64);
                if let Some(delay) = budget.checked_sub(started.elapsed()) { tokio::time::sleep(delay).await; }
            }
            if received != length { return Err(invalid("Truncated runtime block")); }
        }
        if format!("{:x}", Sha256::digest(&buffer[..length])) != *hash {
            return Err(invalid("Runtime block hash mismatch"));
        }
        output.write_all(&buffer[..length]).await?;
        completed += length as u64;
        progress(length as u64);
    }
    output.flush().await?;
    output.sync_all().await?;
    drop(output);
    super::manifest::verify_file(destination, expected)
}

pub async fn download_file(
    client: &reqwest::Client, expected: &RuntimeFile, destination: &Path,
    gate: &dyn UpdateGate, progress: &mut impl FnMut(u64),
) -> Result<(), UpdateError> {
    let url = reqwest::Url::parse(&expected.url).map_err(|_| invalid("Invalid runtime URL"))?;
    if url.scheme() != "https" && !(cfg!(test) && url.host_str() == Some("127.0.0.1")) {
        return Err(invalid("Runtime transfers require HTTPS"));
    }
    wait_until_allowed(gate).await?;
    let response = client.get(url).send().await?.error_for_status()?;
    write_response(response, expected, destination, gate, progress).await
}

async fn write_response(
    mut response: reqwest::Response, expected: &RuntimeFile, destination: &Path,
    gate: &dyn UpdateGate, progress: &mut impl FnMut(u64),
) -> Result<(), UpdateError> {
    if response.content_length().is_some_and(|length| length != expected.size) {
        return Err(invalid("Runtime asset Content-Length mismatch"));
    }
    let mut file = tokio::fs::OpenOptions::new().write(true).create_new(true).open(destination).await?;
    let mut received = 0_u64;
    loop {
        wait_until_allowed(gate).await?;
        let Some(chunk) = response.chunk().await? else { break; };
        received = received
            .checked_add(chunk.len() as u64)
            .filter(|length| *length <= expected.size)
            .ok_or_else(|| invalid("Runtime asset exceeds signed size"))?;
        for piece in chunk.chunks(512 * 1024) {
            wait_until_allowed(gate).await?;
            let started = Instant::now();
            file.write_all(piece).await?;
            progress(piece.len() as u64);
            let budget = Duration::from_secs_f64(
                piece.len() as f64 / transfer_rate(gate.capture_pressure()) as f64,
            );
            if let Some(delay) = budget.checked_sub(started.elapsed()) {
                tokio::time::sleep(delay).await;
            }
        }
    }
    if received != expected.size {
        return Err(invalid("Truncated runtime asset"));
    }
    file.flush().await?;
    file.sync_all().await?;
    drop(file);
    super::manifest::verify_file(destination, expected)
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use base64::Engine;
    use sha2::{Digest, Sha256};
    use std::{io::{Read, Write}, net::TcpListener};

    pub struct TestServer {
        address: String,
        state: std::sync::Arc<std::sync::Mutex<std::collections::HashMap<String, (Vec<u8>, u32)>>>,
    }

    impl TestServer {
        pub fn start() -> Self {
            let listener = TcpListener::bind("127.0.0.1:0").unwrap();
            let address = listener.local_addr().unwrap().to_string();
            let state: std::sync::Arc<std::sync::Mutex<std::collections::HashMap<String, (Vec<u8>, u32)>>> = std::sync::Arc::new(std::sync::Mutex::new(std::collections::HashMap::new()));
            let server_state = state.clone();
            std::thread::spawn(move || {
                for stream in listener.incoming() {
                    let Ok(mut stream) = stream else { continue; };
                    let state = server_state.clone();
                    std::thread::spawn(move || {
                        stream.set_read_timeout(Some(Duration::from_secs(10))).unwrap();
                        let mut buffer = [0; 4096];
                        let _ = stream.read(&mut buffer);
                        let request = String::from_utf8_lossy(&buffer).to_string();
                        let route = request.split_whitespace().nth(1).unwrap_or_default().to_string();
                        let Some((body, count)) = state.lock().unwrap().get(&route).cloned() else {
                            let _ = write!(stream, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                            return;
                        };
                        if count <= 1 {
                            state.lock().unwrap().remove(&route);
                        } else {
                            state.lock().unwrap().insert(route, (body.clone(), count - 1));
                        }
                        let _ = write!(stream, "HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n", body.len());
                        let _ = stream.write_all(&body);
                    });
                }
            });
            Self { address, state }
        }

        pub fn serve(&self, path: &str, body: impl Into<Vec<u8>>, times: u32) -> String {
            let body = body.into();
            let url = format!("http://{}/{}", self.address, path.trim_start_matches('/'));
            self.state.lock().unwrap().insert(format!("/{}", path.trim_start_matches('/')), (body, times));
            url
        }
    }

    #[test]
    fn differential_reuses_shifted_blocks_and_rejects_bad_ranges_and_tampering() {
        for mode in ["range", "wrong-range", "tamper", "fallback"] {
            let root = tempfile::tempdir().unwrap();
            let mut original = vec![1; 1_048_576];
            original.extend(vec![2; 1_048_576]);
            let mut target = vec![2; 1_048_576];
            target.extend(vec![3; 17]);
            let listener = TcpListener::bind("127.0.0.1:0").unwrap();
            let url = format!("http://{}/file", listener.local_addr().unwrap());
            let response_body = target.clone();
            let server = std::thread::spawn(move || {
                let mut transferred = 0;
                for index in 0..1 {
                    let (mut stream, _) = listener.accept().unwrap();
                    stream.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
                    let mut bytes = [0; 4096];
                    let count = stream.read(&mut bytes).unwrap();
                    let request = String::from_utf8_lossy(&bytes[..count]).to_lowercase();
                    if index == 0 { assert!(request.contains("range: bytes=1048576-1048592")); }
                    let mut body = response_body[1_048_576..].to_vec();
                    if mode == "fallback" {
                        body = response_body.clone();
                        write!(stream, "HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n", body.len()).unwrap();
                    } else {
                        if mode == "tamper" { body[0] ^= 1; }
                        let start = if mode == "wrong-range" { 0 } else { 1_048_576 };
                        write!(stream, "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes {start}-1048592/1048593\r\nContent-Length: {}\r\nConnection: close\r\n\r\n", body.len()).unwrap();
                    }
                    if stream.write_all(&body).is_ok() { transferred += body.len(); }
                }
                transferred
            });
            let expected = RuntimeFile {
                path: "clipture.exe".into(), size: target.len() as u64,
                sha256: format!("{:x}", Sha256::digest(&target)), url,
                blocks: target.chunks(1_048_576).map(|block| format!("{:x}", Sha256::digest(block))).collect(),
                block_size: 1_048_576,
            };
            let source = root.path().join("source");
            let destination = root.path().join("destination");
            std::fs::write(&source, original).unwrap();
            let mut progress = 0;
            let result = tauri::async_runtime::block_on(download_differential(&client().unwrap(), &expected, &source, &destination,
                &crate::updates::service::AllowUpdates, &mut |size| progress += size));
            let transferred = server.join().unwrap();
            if matches!(mode, "range" | "fallback") {
                result.unwrap();
                assert_eq!(std::fs::read(destination).unwrap(), target);
                assert_eq!(progress, expected.size);
                if mode == "range" { assert_eq!(transferred, 17); }
            } else { assert!(result.is_err()); }
        }
    }

    pub fn signed_fixture(server: &TestServer, version: &str) -> (String, SignedManifest) {
        let keys = minisign::KeyPair::generate_unencrypted_keypair().unwrap();
        let hash = format!("{:x}", Sha256::digest(b"test"));
        let manifest = serde_json::to_string(&Manifest {
            schema_version: 1,
            version: version.into(),
            platform: "windows-x86_64".into(),
            compatibility: super::super::manifest::Compatibility { host: hash.clone(), ui_protocol: 1, engine_protocol: 1 },
            components: super::super::manifest::Components { controller: hash.clone(), ui: hash.clone(), engine: hash.clone() },
            files: super::super::manifest::PATHS
                .iter()
                .map(|path| RuntimeFile {
                    path: (*path).into(),
                    size: 4,
                    sha256: hash.clone(),
                    url: server.serve(&format!("v{version}/{}", path.rsplit('/').next().unwrap()), b"test", 1),
                    blocks: vec![hash.clone()],
                    block_size: 1_048_576,
                })
                .collect(),
        })
        .unwrap();
        let signature = minisign::sign(Some(&keys.pk), &keys.sk, manifest.as_bytes(), None, None).unwrap();
        (
            base64::engine::general_purpose::STANDARD.encode(keys.pk.to_box().unwrap().to_string()),
            SignedManifest {
                manifest,
                signature: base64::engine::general_purpose::STANDARD.encode(signature.to_string()),
            },
        )
    }
}
