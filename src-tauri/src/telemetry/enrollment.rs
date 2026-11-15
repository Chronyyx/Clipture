//! `telemetry\enrollment.json`: where to send, as which device, and the
//! device key protected for this Windows account (ADR 0035).
use base64::{engine::general_purpose::URL_SAFE_NO_PAD, Engine};
use clipture_telemetry_protocol::{DeviceKey, SECRET_KEY_LENGTH};
use serde::{Deserialize, Serialize};
use std::{fs, path::Path};

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum EnrollmentState {
    Active,
    /// The server refused this device; nothing is sent until a new invite.
    Revoked,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Enrollment {
    pub server: String,
    pub device_id: String,
    pub friend_name: String,
    pub public_key: String,
    /// The Ed25519 secret, DPAPI-protected (base64url).
    pub protected_key: String,
    pub enrolled_at: String,
    pub state: EnrollmentState,
}

const FILE: &str = "enrollment.json";

#[cfg(windows)]
fn protect(bytes: &[u8]) -> std::io::Result<Vec<u8>> {
    crate::platform::windows::dpapi::protect(bytes)
}

#[cfg(windows)]
fn unprotect(bytes: &[u8]) -> std::io::Result<Vec<u8>> {
    crate::platform::windows::dpapi::unprotect(bytes)
}

// Telemetry is Windows-only in practice; other targets keep the tests running.
#[cfg(not(windows))]
fn protect(bytes: &[u8]) -> std::io::Result<Vec<u8>> {
    Ok(bytes.to_vec())
}

#[cfg(not(windows))]
fn unprotect(bytes: &[u8]) -> std::io::Result<Vec<u8>> {
    Ok(bytes.to_vec())
}

pub fn new_key() -> std::io::Result<DeviceKey> {
    let mut secret = [0u8; SECRET_KEY_LENGTH];
    getrandom::fill(&mut secret).map_err(|error| std::io::Error::other(error.to_string()))?;
    Ok(DeviceKey::from_secret(&secret))
}

pub fn protect_key(key: &DeviceKey) -> std::io::Result<String> {
    Ok(URL_SAFE_NO_PAD.encode(protect(&key.secret())?))
}

impl Enrollment {
    pub fn load(root: &Path) -> Option<Self> {
        serde_json::from_slice(&fs::read(root.join(FILE)).ok()?).ok()
    }

    pub fn save(&self, root: &Path) -> std::io::Result<()> {
        fs::create_dir_all(root)?;
        let temporary = root.join(format!("{FILE}.tmp"));
        fs::write(&temporary, serde_json::to_vec_pretty(self).map_err(std::io::Error::other)?)?;
        fs::rename(temporary, root.join(FILE))
    }

    /// The device key, if this account can still decrypt it and it matches
    /// the enrolled public key.
    pub fn key(&self) -> Option<DeviceKey> {
        let protected = URL_SAFE_NO_PAD.decode(&self.protected_key).ok()?;
        let secret: [u8; SECRET_KEY_LENGTH] = unprotect(&protected).ok()?.try_into().ok()?;
        let key = DeviceKey::from_secret(&secret);
        (key.public_key() == self.public_key).then_some(key)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_saved_key_decrypts_and_is_not_stored_plainly() {
        let root = tempfile::tempdir().unwrap();
        let key = new_key().unwrap();
        let enrollment = Enrollment {
            server: "https://t.example".into(),
            device_id: "d-1".into(),
            friend_name: "sam".into(),
            public_key: key.public_key(),
            protected_key: protect_key(&key).unwrap(),
            enrolled_at: "2026-11-15T00:00:00.000Z".into(),
            state: EnrollmentState::Active,
        };
        enrollment.save(root.path()).unwrap();
        let loaded = Enrollment::load(root.path()).unwrap();
        assert_eq!(loaded, enrollment);
        assert_eq!(loaded.key().unwrap().public_key(), key.public_key());
        let file = fs::read_to_string(root.path().join(FILE)).unwrap();
        #[cfg(windows)]
        assert!(!file.contains(&URL_SAFE_NO_PAD.encode(key.secret())));
        // A key that does not match the enrolled public key is refused.
        let other = Enrollment { public_key: new_key().unwrap().public_key(), ..loaded };
        assert!(other.key().is_none());
    }
}
