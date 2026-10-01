//! The peer protocol. Transport security (encryption, integrity and the peer's
//! identity) is provided by iroh's QUIC/TLS 1.3; this module only frames and
//! validates application messages. Each request uses one bidirectional
//! stream: a length-prefixed JSON request, a length-prefixed JSON response,
//! and for `Range` the range's bytes after the response (see `range_body`).
//! Fields added later default when absent and are ignored by older peers, so
//! old and new builds keep talking.
use serde::{de::DeserializeOwned, Deserialize, Serialize};
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};

use super::model::ClipOffer;
use super::store::MAX_NAME_CHARS;

pub const ALPN: &[u8] = b"clipture/share/1";
const MAX_MESSAGE_BYTES: u32 = 16 * 1024;
/// Largest clip we will offer or accept.
pub const MAX_CLIP_BYTES: u64 = 16 * 1024 * 1024 * 1024;
const MAX_TITLE_CHARS: usize = 200;
const MAX_LABEL_CHARS: usize = 80;

#[derive(Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(tag = "type", rename_all = "camelCase")]
pub enum Request {
    /// Friend request, acceptance, or a name refresh.
    Hello { name: String },
    /// Tell a friend about a clip they may stream.
    Offer { offer: ClipOffer },
    /// Read bytes of a clip that was offered to the requester.
    #[serde(rename_all = "camelCase")]
    Range {
        share_id: String,
        start: u64,
        length: u64,
        /// The requester can read a sparse body (zero runs elided).
        #[serde(default)]
        sparse: bool,
    },
    /// The requester removed us from their friends.
    Goodbye {},
    /// A friend came online (or heartbeats), or is going offline.
    Presence { online: bool },
}

#[derive(Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(tag = "status", rename_all = "camelCase")]
pub enum Response {
    Ok {},
    /// `length` raw bytes follow; `total` is the clip size.
    Range {
        total: u64,
        length: u64,
    },
    /// A sparse body describing `length` bytes follows.
    SparseRange {
        total: u64,
        length: u64,
    },
    Denied {
        reason: String,
    },
}

#[derive(Debug, thiserror::Error)]
pub enum WireError {
    #[error("peer message exceeds {MAX_MESSAGE_BYTES} bytes")]
    TooLarge,
    #[error("peer stream failed: {0}")]
    Io(#[from] std::io::Error),
    #[error("peer message is malformed: {0}")]
    Json(#[from] serde_json::Error),
    #[error("peer message is invalid: {0}")]
    Invalid(&'static str),
}

pub async fn write_message<W, T>(writer: &mut W, message: &T) -> Result<(), WireError>
where
    W: AsyncWrite + Unpin,
    T: Serialize,
{
    let bytes = serde_json::to_vec(message)?;
    let length = u32::try_from(bytes.len()).map_err(|_| WireError::TooLarge)?;
    if length > MAX_MESSAGE_BYTES {
        return Err(WireError::TooLarge);
    }
    writer.write_all(&length.to_be_bytes()).await?;
    writer.write_all(&bytes).await?;
    Ok(())
}

pub async fn read_message<R, T>(reader: &mut R) -> Result<T, WireError>
where
    R: AsyncRead + Unpin,
    T: DeserializeOwned,
{
    let length = reader.read_u32().await?;
    if length > MAX_MESSAGE_BYTES {
        return Err(WireError::TooLarge);
    }
    let mut bytes = vec![0_u8; length as usize];
    reader.read_exact(&mut bytes).await?;
    Ok(serde_json::from_slice(&bytes)?)
}

/// Cleans display text from a peer: no control characters, bounded length.
pub fn clean_text(value: &str, maximum_chars: usize) -> String {
    value
        .chars()
        .filter(|character| !character.is_control())
        .take(maximum_chars)
        .collect::<String>()
        .trim()
        .to_owned()
}

pub fn clean_name(value: &str) -> String {
    clean_text(value, MAX_NAME_CHARS)
}

pub fn valid_share_id(value: &str) -> bool {
    value.len() == 32
        && value
            .bytes()
            .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
}

fn valid_digest(value: &str) -> bool {
    value.len() == 64
        && value
            .bytes()
            .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
}

/// Validates and normalizes an offer received from a peer.
pub fn sanitize_offer(mut offer: ClipOffer) -> Result<ClipOffer, WireError> {
    if !valid_share_id(&offer.share_id) {
        return Err(WireError::Invalid("share id"));
    }
    if !valid_digest(&offer.blake3) {
        return Err(WireError::Invalid("digest"));
    }
    if offer.size == 0 || offer.size > MAX_CLIP_BYTES {
        return Err(WireError::Invalid("clip size"));
    }
    offer.title = clean_text(&offer.title, MAX_TITLE_CHARS);
    if offer.title.is_empty() {
        offer.title = "Shared clip".into();
    }
    offer.resolution = clean_text(&offer.resolution, 24);
    offer.game_or_app = clean_text(&offer.game_or_app, MAX_LABEL_CHARS);
    offer.duration_seconds = offer.duration_seconds.min(24 * 60 * 60);
    offer.fps = offer.fps.min(1_000);
    offer.audio_tracks = offer
        .audio_tracks
        .iter()
        .take(16)
        .map(|label| clean_text(label, MAX_LABEL_CHARS))
        .collect();
    Ok(offer)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn offer() -> ClipOffer {
        ClipOffer {
            share_id: "0123456789abcdef0123456789abcdef".into(),
            title: "Ace\u{0007} round".into(),
            size: 1024,
            blake3: "ab".repeat(32),
            duration_seconds: 30,
            resolution: "1920x1080".into(),
            game_or_app: "cs2".into(),
            created_at_ms: 5,
            fps: 60,
            audio_tracks: vec!["Game".into()],
        }
    }

    #[tokio::test]
    async fn messages_round_trip_with_a_length_prefix() {
        let request = Request::Range {
            share_id: "0123456789abcdef0123456789abcdef".into(),
            start: 7,
            length: 9,
            sparse: true,
        };
        let mut bytes = Vec::new();
        write_message(&mut bytes, &request).await.unwrap();
        let decoded: Request = read_message(&mut bytes.as_slice()).await.unwrap();
        assert_eq!(decoded, request);
    }

    #[tokio::test]
    async fn oversized_frames_are_rejected_before_allocation() {
        let bytes = (MAX_MESSAGE_BYTES + 1).to_be_bytes();
        let result: Result<Request, _> = read_message(&mut bytes.as_slice()).await;
        assert!(matches!(result, Err(WireError::TooLarge)));
    }

    #[tokio::test]
    async fn unknown_message_types_are_rejected() {
        let body = br#"{"type":"deleteEverything"}"#;
        let mut bytes = (body.len() as u32).to_be_bytes().to_vec();
        bytes.extend_from_slice(body);
        let result: Result<Request, _> = read_message(&mut bytes.as_slice()).await;
        assert!(matches!(result, Err(WireError::Json(_))));
    }

    #[test]
    fn offers_are_validated_and_cleaned() {
        let cleaned = sanitize_offer(offer()).unwrap();
        assert_eq!(cleaned.title, "Ace round");
        let mut bad = offer();
        bad.share_id = "../../etc".into();
        assert!(sanitize_offer(bad).is_err());
        let mut huge = offer();
        huge.size = MAX_CLIP_BYTES + 1;
        assert!(sanitize_offer(huge).is_err());
        let mut digest = offer();
        digest.blake3 = "AB".repeat(32);
        assert!(sanitize_offer(digest).is_err());
    }
}
