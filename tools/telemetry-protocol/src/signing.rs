use base64::{engine::general_purpose::URL_SAFE_NO_PAD, Engine};
use ed25519_dalek::{Signature, Signer, SigningKey, VerifyingKey};
use sha2::{Digest, Sha256};
use std::fmt;

pub use ed25519_dalek::{PUBLIC_KEY_LENGTH, SECRET_KEY_LENGTH};

/// What a device signs to prove it holds the key it enrolls.
pub fn enroll_message(token: &str, public_key: &str) -> Vec<u8> {
    format!("clipture-telemetry-enroll-v1\n{token}\n{public_key}").into_bytes()
}

/// What a device signs for each batch: the body is bound by its digest, the
/// timestamp limits how long a captured request stays acceptable.
pub fn batch_message(device: &str, timestamp_ms: i64, body_digest: &str) -> Vec<u8> {
    format!("clipture-telemetry-batch-v1\n{device}\n{timestamp_ms}\n{body_digest}").into_bytes()
}

pub fn body_digest(body: &[u8]) -> String {
    Sha256::digest(body).iter().map(|byte| format!("{byte:02x}")).collect()
}

#[derive(Debug, PartialEq, Eq)]
pub enum KeyError {
    Encoding,
    Signature,
}

impl fmt::Display for KeyError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Self::Encoding => "malformed key or signature",
            Self::Signature => "signature does not verify",
        })
    }
}

impl std::error::Error for KeyError {}

/// A device's private key. Created from 32 random bytes the caller supplies,
/// so this crate needs no randomness source of its own.
pub struct DeviceKey(SigningKey);

impl DeviceKey {
    pub fn from_secret(secret: &[u8; SECRET_KEY_LENGTH]) -> Self {
        Self(SigningKey::from_bytes(secret))
    }

    pub fn secret(&self) -> [u8; SECRET_KEY_LENGTH] {
        self.0.to_bytes()
    }

    pub fn public_key(&self) -> String {
        URL_SAFE_NO_PAD.encode(self.0.verifying_key().to_bytes())
    }

    pub fn sign(&self, message: &[u8]) -> String {
        URL_SAFE_NO_PAD.encode(self.0.sign(message).to_bytes())
    }
}

/// Checks a base64url signature against a base64url public key.
pub fn verify(public_key: &str, message: &[u8], signature: &str) -> Result<(), KeyError> {
    let key: [u8; PUBLIC_KEY_LENGTH] = URL_SAFE_NO_PAD
        .decode(public_key)
        .ok()
        .and_then(|bytes| bytes.try_into().ok())
        .ok_or(KeyError::Encoding)?;
    let key = VerifyingKey::from_bytes(&key).map_err(|_| KeyError::Encoding)?;
    let signature = URL_SAFE_NO_PAD
        .decode(signature)
        .ok()
        .and_then(|bytes| Signature::from_slice(&bytes).ok())
        .ok_or(KeyError::Encoding)?;
    key.verify_strict(message, &signature).map_err(|_| KeyError::Signature)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn signs_and_verifies_batches() {
        let key = DeviceKey::from_secret(&[3; SECRET_KEY_LENGTH]);
        let body = b"{\"seq\":1}\n";
        let message = batch_message("d-1", 1_700_000_000_000, &body_digest(body));
        let signature = key.sign(&message);
        assert_eq!(verify(&key.public_key(), &message, &signature), Ok(()));
        // Any change to device, time or body breaks it.
        for other in [
            batch_message("d-2", 1_700_000_000_000, &body_digest(body)),
            batch_message("d-1", 1_700_000_000_001, &body_digest(body)),
            batch_message("d-1", 1_700_000_000_000, &body_digest(b"{\"seq\":2}\n")),
        ] {
            assert_eq!(verify(&key.public_key(), &other, &signature), Err(KeyError::Signature));
        }
        let stranger = DeviceKey::from_secret(&[4; SECRET_KEY_LENGTH]);
        assert_eq!(verify(&stranger.public_key(), &message, &signature), Err(KeyError::Signature));
    }

    #[test]
    fn secret_round_trips() {
        let key = DeviceKey::from_secret(&[9; SECRET_KEY_LENGTH]);
        assert_eq!(DeviceKey::from_secret(&key.secret()).public_key(), key.public_key());
    }

    #[test]
    fn rejects_garbage() {
        assert_eq!(verify("nope", b"m", "nope"), Err(KeyError::Encoding));
        let key = DeviceKey::from_secret(&[3; SECRET_KEY_LENGTH]);
        assert_eq!(verify(&key.public_key(), b"m", "AAAA"), Err(KeyError::Encoding));
    }

    #[test]
    fn digest_is_lowercase_sha256_hex() {
        assert_eq!(body_digest(b""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    }
}
