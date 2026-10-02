//! "Add to library": copies a friend's clip to disk. Bytes go to a partial
//! file named after the share, so a dropped connection resumes where it
//! stopped (automatically, and on "Try adding again", even after a restart).
//! The clip is published only after its size, MP4 signature and BLAKE3
//! digest over the whole file match the offer.
use std::{
    path::{Path, PathBuf},
    time::{Duration, Instant},
};

use iroh::EndpointId;
use tokio::{
    fs::{File, OpenOptions},
    io::{AsyncReadExt, AsyncWriteExt},
    time::timeout,
};

use crate::{
    clips::{safe_file_stem, unique_destination},
    error::{AppError, AppResult},
};

use super::{
    model::ClipOffer,
    node::{is_unavailable, Node},
    wire::RangePurpose,
};

const READ_TIMEOUT: Duration = Duration::from_secs(45);
const BUFFER_BYTES: usize = 1024 * 1024;
const ROUTE_CHECK: Duration = Duration::from_secs(1);
/// Waits between reconnect attempts; any progress restarts the sequence.
const RETRY_DELAYS: [Duration; 6] = [
    Duration::from_secs(1),
    Duration::from_secs(2),
    Duration::from_secs(4),
    Duration::from_secs(8),
    Duration::from_secs(15),
    Duration::from_secs(30),
];

/// Transfer progress for the UI.
pub struct Progress {
    pub received: u64,
    /// Through a relay rather than directly; `None` until known.
    pub relayed: Option<bool>,
    /// The connection dropped and is being re-established.
    pub reconnecting: bool,
}

/// Where the unfinished bytes of a share live. Share ids are validated hex.
pub fn partial_path(folder: &Path, share_id: &str) -> PathBuf {
    folder.join(format!(".clipture-incoming-{share_id}.part"))
}

/// The bytes received so far, with their running digest.
struct Partial {
    file: File,
    path: PathBuf,
    hasher: blake3::Hasher,
    header: Vec<u8>,
    received: u64,
}

impl Partial {
    /// Opens (or creates) the partial file and re-hashes what already
    /// arrived. A partial longer than the clip cannot be ours: it is emptied.
    async fn open(path: PathBuf, size: u64, buffer: &mut [u8]) -> std::io::Result<Self> {
        let file = OpenOptions::new()
            .create(true)
            .read(true)
            .write(true)
            .truncate(false)
            .open(&path)
            .await?;
        if file.metadata().await?.len() > size {
            file.set_len(0).await?;
        }
        let mut partial = Self {
            file,
            path,
            hasher: blake3::Hasher::new(),
            header: Vec::with_capacity(12),
            received: 0,
        };
        loop {
            let read = partial.file.read(buffer).await?;
            if read == 0 {
                break;
            }
            partial.absorb(&buffer[..read]);
        }
        Ok(partial)
    }

    fn absorb(&mut self, chunk: &[u8]) {
        if self.header.len() < 12 {
            let missing = 12 - self.header.len();
            self.header.extend(chunk.iter().take(missing));
        }
        self.hasher.update(chunk);
        self.received += chunk.len() as u64;
    }

    async fn append(&mut self, chunk: &[u8]) -> AppResult<()> {
        self.file
            .write_all(chunk)
            .await
            .map_err(|source| io("write shared clip", &self.path, source))?;
        self.absorb(chunk);
        Ok(())
    }

    fn matches(&self, offer: &ClipOffer) -> AppResult<()> {
        if !is_mp4(&self.header) {
            return Err(AppError::Path("the shared file is not an MP4 video".into()));
        }
        if self.hasher.finalize().to_hex().as_str() != offer.blake3 {
            return Err(AppError::Path(
                "the downloaded clip failed its integrity check and was discarded".into(),
            ));
        }
        Ok(())
    }
}

pub async fn download(
    node: &Node,
    peer: EndpointId,
    offer: &ClipOffer,
    folder: &Path,
    mut progress: impl FnMut(Progress),
) -> AppResult<PathBuf> {
    tokio::fs::create_dir_all(folder)
        .await
        .map_err(|source| io("create shared clips folder", folder, source))?;
    let path = partial_path(folder, &offer.share_id);
    let mut buffer = vec![0_u8; BUFFER_BYTES];
    let mut partial = Partial::open(path.clone(), offer.size, &mut buffer)
        .await
        .map_err(|source| io("open partial clip", &path, source))?;
    let mut relayed = node.relayed(peer).await;
    progress(Progress {
        received: partial.received,
        relayed,
        reconnecting: false,
    });

    let mut failures = 0_usize;
    while partial.received < offer.size {
        let before = partial.received;
        let attempt = receive(
            node,
            peer,
            offer,
            &mut partial,
            &mut buffer,
            &mut relayed,
            &mut progress,
        )
        .await;
        let Err(failure) = attempt else { break };
        if partial.received > before {
            failures = 0;
        }
        let Failure::Retry(error) = failure else {
            return Err(failure.into_error());
        };
        let Some(delay) = RETRY_DELAYS.get(failures) else {
            return Err(error);
        };
        progress(Progress {
            received: partial.received,
            relayed,
            reconnecting: true,
        });
        tokio::time::sleep(*delay).await;
        failures += 1;
    }

    if let Err(error) = partial.matches(offer) {
        // Bad bytes must never be resumed from; the next try starts over.
        drop(partial);
        let _ = tokio::fs::remove_file(&path).await;
        return Err(error);
    }
    partial
        .file
        .sync_all()
        .await
        .map_err(|source| io("flush shared clip", &path, source))?;
    drop(partial);

    let folder = folder.to_owned();
    let stem = safe_file_stem(&offer.title, "Shared clip");
    tokio::task::spawn_blocking(move || publish(&path, &folder, &stem))
        .await
        .map_err(|error| AppError::Integration(error.to_string()))?
}

enum Failure {
    /// The connection dropped or stalled; worth reconnecting.
    Retry(AppError),
    /// The friend refused, the clip changed, or the disk failed.
    Final(AppError),
}

impl Failure {
    fn into_error(self) -> AppError {
        match self {
            Self::Retry(error) | Self::Final(error) => error,
        }
    }
}

/// One connection's worth of transfer, appending to the partial file.
async fn receive(
    node: &Node,
    peer: EndpointId,
    offer: &ClipOffer,
    partial: &mut Partial,
    buffer: &mut [u8],
    relayed: &mut Option<bool>,
    progress: &mut impl FnMut(Progress),
) -> Result<(), Failure> {
    let start = partial.received;
    let (total, mut stream) = node
        .open_range(peer, &offer.share_id, start, offer.size - start, RangePurpose::Keep)
        .await
        .map_err(|error| {
            if is_unavailable(&error) {
                Failure::Final(error)
            } else {
                Failure::Retry(error)
            }
        })?;
    if total != offer.size {
        return Err(Failure::Final(AppError::Path(
            "the shared clip changed on your friend's PC".into(),
        )));
    }
    let mut route_checked = Instant::now();
    while partial.received < offer.size {
        let wanted = buffer.len().min((offer.size - partial.received) as usize);
        let read = timeout(READ_TIMEOUT, stream.read(&mut buffer[..wanted]))
            .await
            .map_err(|_| Failure::Retry(AppError::Path("the download stalled".into())))?
            .map_err(|_| Failure::Retry(interrupted()))?;
        if read == 0 {
            return Err(Failure::Retry(interrupted()));
        }
        partial
            .append(&buffer[..read])
            .await
            .map_err(Failure::Final)?;
        if route_checked.elapsed() >= ROUTE_CHECK {
            route_checked = Instant::now();
            *relayed = node.relayed(peer).await;
        }
        progress(Progress {
            received: partial.received,
            relayed: *relayed,
            reconnecting: false,
        });
    }
    Ok(())
}

/// Moves the verified partial into place without ever overwriting a clip:
/// a hard link fails if the name exists, and the partial is then removed.
fn publish(partial: &Path, folder: &Path, stem: &str) -> AppResult<PathBuf> {
    for _ in 0..8 {
        let destination = unique_destination(folder, stem, "mp4", Path::new(""));
        match std::fs::hard_link(partial, &destination) {
            Ok(()) => {
                let _ = std::fs::remove_file(partial);
                return Ok(destination);
            }
            Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => {}
            Err(error) => return Err(io("save shared clip", &destination, error)),
        }
    }
    Err(AppError::Path(
        "could not choose a file name for the shared clip".into(),
    ))
}

/// ISO BMFF files start with a box whose type is `ftyp`.
fn is_mp4(header: &[u8]) -> bool {
    header.len() >= 8 && &header[4..8] == b"ftyp"
}

fn interrupted() -> AppError {
    AppError::Path("the connection to your friend was interrupted".into())
}

fn io(action: &'static str, path: &Path, source: std::io::Error) -> AppError {
    AppError::Io {
        action,
        path: path.to_owned(),
        source,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_iso_media_headers_are_accepted() {
        assert!(is_mp4(b"\0\0\0\x20ftypisom"));
        assert!(!is_mp4(b"MZ\x90\0\x03\0\0\0\x04\0"));
        assert!(!is_mp4(b"ftyp"));
    }

    #[test]
    fn publishing_never_overwrites_an_existing_clip() {
        let folder = tempfile::tempdir().unwrap();
        std::fs::write(folder.path().join("Ace.mp4"), b"mine").unwrap();
        let partial = partial_path(folder.path(), "abc");
        std::fs::write(&partial, b"theirs").unwrap();
        let saved = publish(&partial, folder.path(), "Ace").unwrap();
        assert_eq!(saved.file_name().unwrap(), "Ace_1.mp4");
        assert_eq!(std::fs::read(folder.path().join("Ace.mp4")).unwrap(), b"mine");
        assert_eq!(std::fs::read(&saved).unwrap(), b"theirs");
        assert!(!partial.exists());
    }

    #[tokio::test]
    async fn resuming_rehashes_what_already_arrived() {
        let folder = tempfile::tempdir().unwrap();
        let path = folder.path().join("part");
        std::fs::write(&path, b"\0\0\0\x20ftypisom-first-half").unwrap();
        let mut partial = Partial::open(path.clone(), 1000, &mut [0; 4]).await.unwrap();
        assert_eq!(partial.received, 23);
        partial.append(b"+second").await.unwrap();
        partial.file.flush().await.unwrap();
        let whole = std::fs::read(&path).unwrap();
        assert_eq!(whole.len(), 30);
        assert_eq!(partial.hasher.finalize(), blake3::hash(&whole));
        assert!(is_mp4(&partial.header));
    }

    #[tokio::test]
    async fn an_oversized_partial_starts_over() {
        let folder = tempfile::tempdir().unwrap();
        let path = folder.path().join("part");
        std::fs::write(&path, vec![7_u8; 64]).unwrap();
        let partial = Partial::open(path, 10, &mut [0; 16]).await.unwrap();
        assert_eq!(partial.received, 0);
    }
}
