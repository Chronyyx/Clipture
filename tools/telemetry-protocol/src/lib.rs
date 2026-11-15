//! The telemetry wire format (ADR 0035): invite codes, the strings each side
//! signs, the enrollment exchange and the records a batch carries.
mod invite;
mod records;
mod signing;

pub use invite::{Invite, InviteError};
pub use records::*;
pub use signing::{
    batch_message, body_digest, enroll_message, verify, DeviceKey, KeyError, PUBLIC_KEY_LENGTH,
    SECRET_KEY_LENGTH,
};

use serde::{Deserialize, Serialize};

pub const ENROLL_PATH: &str = "/v1/enroll";
pub const BATCHES_PATH: &str = "/v1/batches";
pub const DEVICE_HEADER: &str = "x-clipture-device";
pub const TIMESTAMP_HEADER: &str = "x-clipture-timestamp";
pub const SIGNATURE_HEADER: &str = "x-clipture-signature";
/// A batch is one gzip-compressed journal segment of JSON lines.
pub const MAXIMUM_COMPRESSED_BATCH: usize = 1024 * 1024;
pub const MAXIMUM_DECOMPRESSED_BATCH: usize = 8 * 1024 * 1024;
/// How far a batch's timestamp may be from the server's clock.
pub const MAXIMUM_CLOCK_SKEW_MS: i64 = 10 * 60 * 1000;

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct EnrollRequest {
    /// The invite token, base64url.
    pub token: String,
    /// The device's Ed25519 public key, base64url.
    pub public_key: String,
    /// Signature over `enroll_message(token, public_key)`, base64url.
    pub signature: String,
    pub version: String,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct EnrollResponse {
    pub device_id: String,
    pub friend_name: String,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct BatchResponse {
    /// Records stored from this batch (duplicates are skipped).
    pub accepted: u64,
    /// The highest sequence number the server holds for this device.
    pub last_seq: u64,
}
