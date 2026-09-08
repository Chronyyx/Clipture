//! Private controller/UI-worker transport; never exposed as a renderer API.
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::{
    io::{self, Read, Write},
    path::PathBuf,
};

pub const PROTOCOL_VERSION: u32 = 1;
pub const MAXIMUM_JSON_BYTES: usize = 16 * 1024 * 1024;
pub const MAXIMUM_BINARY_BYTES: usize = 4 * 1024 * 1024;

#[derive(Debug, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "camelCase", deny_unknown_fields)]
pub enum Message {
    Bootstrap {
        version: u32,
        webview_directory: PathBuf,
        sounds_directory: PathBuf,
        smoke: bool,
    },
    Ready {
        version: u32,
    },
    Invoke {
        id: u64,
        command: String,
        args: Value,
    },
    Reply {
        id: u64,
        result: Result<Value, String>,
    },
    Media {
        id: u64,
        method: String,
        uri: String,
        range: Option<String>,
        origin: Option<String>,
    },
    MediaReply {
        id: u64,
        status: u16,
        headers: Vec<(String, String)>,
    },
    Event {
        name: String,
        payload: Value,
    },
    Focus {},
    Close {},
    Closing {},
    #[cfg(debug_assertions)]
    SmokeReport {
        report: Value,
    },
}

#[derive(Debug)]
pub struct Frame {
    pub message: Message,
    pub binary: Vec<u8>,
}

impl From<Message> for Frame {
    fn from(message: Message) -> Self {
        Self {
            message,
            binary: vec![],
        }
    }
}

/// Each frame has two little-endian u32 lengths, a JSON header, then an optional
/// binary body. Reject lengths before allocating, including truncated frames.
pub fn read_frame(reader: &mut impl Read) -> io::Result<Option<Frame>> {
    let mut lengths = [0u8; 8];
    loop {
        match reader.read(&mut lengths[..1]) {
            Ok(0) => return Ok(None),
            Ok(_) => break,
            Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
            Err(error) => return Err(error),
        }
    }
    reader.read_exact(&mut lengths[1..])?;
    let json_len = u32::from_le_bytes(lengths[..4].try_into().unwrap()) as usize;
    let binary_len = u32::from_le_bytes(lengths[4..].try_into().unwrap()) as usize;
    if json_len == 0 || json_len > MAXIMUM_JSON_BYTES || binary_len > MAXIMUM_BINARY_BYTES {
        return Err(invalid("UI transport frame exceeds its limits"));
    }
    let mut json = vec![0; json_len];
    reader.read_exact(&mut json)?;
    let message: Message =
        serde_json::from_slice(&json).map_err(|error| invalid(error.to_string()))?;
    if binary_len > 0 && !matches!(message, Message::MediaReply { .. }) {
        return Err(invalid("Only media replies may contain a binary body"));
    }
    let mut binary = vec![0; binary_len];
    reader.read_exact(&mut binary)?;
    Ok(Some(Frame { message, binary }))
}

pub fn write_frame(writer: &mut impl Write, frame: &Frame) -> io::Result<()> {
    if frame.binary.len() > MAXIMUM_BINARY_BYTES
        || (!frame.binary.is_empty() && !matches!(frame.message, Message::MediaReply { .. }))
    {
        return Err(invalid("Invalid UI transport binary body"));
    }
    let mut json = BoundedJson(Vec::new());
    serde_json::to_writer(&mut json, &frame.message).map_err(|error| invalid(error.to_string()))?;
    writer.write_all(&(json.0.len() as u32).to_le_bytes())?;
    writer.write_all(&(frame.binary.len() as u32).to_le_bytes())?;
    writer.write_all(&json.0)?;
    writer.write_all(&frame.binary)?;
    writer.flush()
}

struct BoundedJson(Vec<u8>);
impl Write for BoundedJson {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        if bytes.len() > MAXIMUM_JSON_BYTES.saturating_sub(self.0.len()) {
            return Err(invalid("UI transport JSON exceeds its limit"));
        }
        self.0.extend_from_slice(bytes);
        Ok(bytes.len())
    }
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

fn invalid(message: impl Into<String>) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, message.into())
}

#[cfg(test)]
#[path = "wire_tests.rs"]
mod tests;
