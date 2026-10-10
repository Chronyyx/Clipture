//! A mix's redo journal: the whole plan (new bytes and where they go), then a
//! checksummed header written last as the commit point. Until the header is
//! durable the clip is untouched; after it, applying the plan again always
//! yields the finished mix.
//!
//! ```text
//! 0     magic "CLPMIX01", body length, BLAKE3 of the body
//! 4096  body: version, clip path, lengths, then every write
//! ```
use std::{io, path::PathBuf};

use super::super::trim::disk::Disk;
use super::plan::{Plan, Write};

const MAGIC: &[u8; 8] = b"CLPMIX01";
const HEADER_LEN: usize = 48;
const BODY_AT: u64 = 4096;
const VERSION: u64 = 1;
/// No real mix comes near this; a larger length is a damaged header.
const MAXIMUM_BODY_BYTES: u64 = 1024 * 1024 * 1024;

/// Writes `plan` and makes it durable.
pub(super) fn commit(journal: &mut impl Disk, plan: &Plan) -> io::Result<()> {
    let body = encode(plan);
    journal.write_at(BODY_AT, &body)?;
    journal.sync()?;
    let mut header = Vec::with_capacity(HEADER_LEN);
    header.extend_from_slice(MAGIC);
    header.extend((body.len() as u64).to_be_bytes());
    header.extend_from_slice(blake3::hash(&body).as_bytes());
    journal.write_at(0, &header)?;
    journal.sync()
}

/// The committed plan; `None` when the journal was never committed.
pub(super) fn read(journal: &mut impl Disk) -> io::Result<Option<Plan>> {
    let len = journal.len()?;
    if len < BODY_AT {
        return Ok(None);
    }
    let mut header = [0_u8; HEADER_LEN];
    journal.read_at(0, &mut header)?;
    let body_len = u64::from_be_bytes(header[8..16].try_into().unwrap());
    if &header[..8] != MAGIC || body_len > MAXIMUM_BODY_BYTES || len < BODY_AT + body_len {
        return Ok(None);
    }
    let mut body = vec![0_u8; body_len as usize];
    journal.read_at(BODY_AT, &mut body)?;
    if blake3::hash(&body).as_bytes() != &header[16..48] {
        return Ok(None);
    }
    decode(&body).map(Some).ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "a committed mix journal is unreadable"))
}

fn encode(plan: &Plan) -> Vec<u8> {
    let data: usize = plan.writes.iter().map(|write| write.bytes.len() + 16).sum();
    let mut out = Vec::with_capacity(data + 512);
    let path = plan.clip.to_string_lossy().into_owned().into_bytes();
    for value in [VERSION, path.len() as u64] {
        out.extend(value.to_be_bytes());
    }
    out.extend(&path);
    for value in [plan.clip_len, plan.final_len, plan.tracks_changed as u64, plan.writes.len() as u64] {
        out.extend(value.to_be_bytes());
    }
    for write in &plan.writes {
        out.extend(write.at.to_be_bytes());
        out.extend((write.bytes.len() as u64).to_be_bytes());
        out.extend(&write.bytes);
    }
    out
}

fn decode(bytes: &[u8]) -> Option<Plan> {
    let mut reader = Reader { bytes, at: 0 };
    if reader.word()? != VERSION {
        return None;
    }
    let path_len = reader.len()?;
    let clip = PathBuf::from(String::from_utf8(reader.take(path_len)?.to_vec()).ok()?);
    let clip_len = reader.word()?;
    let final_len = reader.word()?;
    let tracks_changed = reader.len()?;
    let count = reader.len()?;
    let mut writes = Vec::with_capacity(count.min(1 << 20));
    for _ in 0..count {
        let at = reader.word()?;
        let len = reader.len()?;
        writes.push(Write { at, bytes: reader.take(len)?.to_vec() });
    }
    (reader.at == bytes.len()).then_some(Plan { clip, clip_len, writes, final_len, tracks_changed })
}

struct Reader<'a> {
    bytes: &'a [u8],
    at: usize,
}

impl<'a> Reader<'a> {
    fn word(&mut self) -> Option<u64> {
        Some(u64::from_be_bytes(self.take(8)?.try_into().ok()?))
    }

    fn len(&mut self) -> Option<usize> {
        usize::try_from(self.word()?).ok()
    }

    fn take(&mut self, len: usize) -> Option<&'a [u8]> {
        let slice = self.bytes.get(self.at..self.at.checked_add(len)?)?;
        self.at += len;
        Some(slice)
    }
}
