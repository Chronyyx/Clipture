use serde::Serialize;
use serde_json::{Map, Value};

use crate::error::{AppError, AppResult};

pub(super) const MAX_ENGINE_LINE_BYTES: usize = 8 * 1024 * 1024;

#[derive(Debug, serde::Deserialize)]
pub(super) struct ResponseEnvelope {
    pub id: Option<u64>,
    pub payload: Option<Value>,
    pub error: Option<String>,
    pub event: Option<String>,
    pub source: Option<String>,
}

impl ResponseEnvelope {
    pub fn parse(line: &str) -> Result<Self, serde_json::Error> {
        serde_json::from_str(line)
    }
}

pub(super) fn encode_request(
    id: u64,
    command: &str,
    payload: &impl Serialize,
) -> AppResult<Vec<u8>> {
    let mut object = match serde_json::to_value(payload)? {
        Value::Object(object) => object,
        Value::Null => Map::new(),
        _ => {
            return Err(AppError::Engine(
                "engine request payload must be an object".into(),
            ))
        }
    };
    object.insert("id".into(), id.into());
    object.insert("type".into(), command.into());
    let mut bytes = serde_json::to_vec(&object)?;
    bytes.push(b'\n');
    Ok(bytes)
}

/// Incremental stdout decoder. Once a line exceeds the cap, bytes are discarded
/// through its newline so a later well-formed frame can still be processed.
pub(super) struct JsonLineDecoder {
    remainder: Vec<u8>,
    maximum_line_bytes: usize,
    discarding_oversized_line: bool,
}

impl Default for JsonLineDecoder {
    fn default() -> Self {
        Self::with_limit(MAX_ENGINE_LINE_BYTES)
    }
}

impl JsonLineDecoder {
    fn with_limit(maximum_line_bytes: usize) -> Self {
        Self {
            remainder: Vec::new(),
            maximum_line_bytes,
            discarding_oversized_line: false,
        }
    }

    pub fn push(&mut self, chunk: &[u8]) -> Vec<Result<String, String>> {
        let mut frames = Vec::new();
        for &byte in chunk {
            if self.discarding_oversized_line {
                if byte == b'\n' {
                    self.discarding_oversized_line = false;
                }
                continue;
            }

            if byte == b'\n' {
                if self.remainder.last() == Some(&b'\r') {
                    self.remainder.pop();
                }
                let frame = std::mem::take(&mut self.remainder);
                frames.push(
                    String::from_utf8(frame)
                        .map_err(|error| format!("engine stdout frame was not UTF-8: {error}")),
                );
                continue;
            }

            if self.remainder.len() == self.maximum_line_bytes {
                self.remainder.clear();
                self.discarding_oversized_line = true;
                frames.push(Err(format!(
                    "engine JSON line exceeded {} bytes",
                    self.maximum_line_bytes
                )));
                continue;
            }
            self.remainder.push(byte);
        }
        frames
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn request_is_one_camel_case_json_line() {
        let message = encode_request(
            7,
            "saveClip",
            &serde_json::json!({ "durationSeconds": 30, "saveFolder": "C:\\clips" }),
        )
        .unwrap();
        assert_eq!(message.last(), Some(&b'\n'));
        let value: Value = serde_json::from_slice(&message).unwrap();
        assert_eq!(value["id"], 7);
        assert_eq!(value["type"], "saveClip");
        assert_eq!(value["durationSeconds"], 30);
    }

    #[test]
    fn reassembles_split_json_and_handles_crlf() {
        let mut decoder = JsonLineDecoder::with_limit(128);
        assert!(decoder.push(br#"{"id":1,"pay"#).is_empty());
        let frames = decoder.push(b"load\":true}\r\n{\"id\":2");
        assert_eq!(frames.len(), 1);
        assert_eq!(frames[0].as_ref().unwrap(), r#"{"id":1,"payload":true}"#);
        let frames = decoder.push(b"}\n");
        assert_eq!(frames[0].as_ref().unwrap(), r#"{"id":2}"#);
    }

    #[test]
    fn returns_every_complete_line_in_one_chunk() {
        let mut decoder = JsonLineDecoder::with_limit(128);
        let frames = decoder.push(b"{\"id\":1}\n{\"id\":2}\n");
        let lines: Vec<_> = frames.into_iter().map(Result::unwrap).collect();
        assert_eq!(lines, vec![r#"{"id":1}"#, r#"{"id":2}"#]);
    }

    #[test]
    fn rejects_oversized_line_and_resynchronizes() {
        let mut decoder = JsonLineDecoder::with_limit(8);
        let frames = decoder.push(b"123456789discarded\n{}\n");
        assert_eq!(frames.len(), 2);
        assert!(frames[0].as_ref().unwrap_err().contains("exceeded 8"));
        assert_eq!(frames[1].as_ref().unwrap(), "{}");
        assert!(decoder.remainder.is_empty());
    }

    #[test]
    fn rejects_bad_utf8_without_losing_the_next_frame() {
        let mut decoder = JsonLineDecoder::with_limit(128);
        let frames = decoder.push(&[0xff, b'\n', b'{', b'}', b'\n']);
        assert!(frames[0].is_err());
        assert_eq!(frames[1].as_ref().unwrap(), "{}");
    }

    #[test]
    fn malformed_json_is_rejected_without_panicking() {
        assert!(ResponseEnvelope::parse("{not-json}").is_err());
        assert!(ResponseEnvelope::parse(r#"{"id":1,"payload":true}"#).is_ok());
    }
}
