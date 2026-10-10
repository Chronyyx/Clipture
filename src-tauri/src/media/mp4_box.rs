//! Minimal MP4 box reading and writing shared by the layout tools (the linear
//! playback view and the in-place trimmer). Parsing is strict: boxes must
//! exactly fill their parent, so malformed input is refused, not guessed at.
use std::{
    fs::File,
    io::{self, Read, Seek, SeekFrom},
};

pub(super) struct Item<'a> {
    pub(super) kind: [u8; 4],
    /// The whole box, header included.
    pub(super) full: &'a [u8],
    pub(super) body: &'a [u8],
}

/// Every child box of `data`, or `None` when the boxes do not exactly fill it.
pub(super) fn boxes(data: &[u8]) -> Option<Vec<Item<'_>>> {
    let mut items = Vec::new();
    let mut rest = data;
    while !rest.is_empty() {
        let header = box_header_len(rest)?;
        let size = match u32_at(rest, 0)? {
            1 => usize::try_from(u64_at(rest, 8)?).ok()?,
            0 => rest.len(),
            size => size as usize,
        };
        if size < header || size > rest.len() {
            return None;
        }
        let (full, next) = rest.split_at(size);
        items.push(Item {
            kind: full[4..8].try_into().ok()?,
            full,
            body: &full[header..],
        });
        rest = next;
    }
    Some(items)
}

pub(super) fn find<'a>(data: &'a [u8], kind: &[u8; 4]) -> Option<&'a [u8]> {
    boxes(data)?.into_iter().find(|item| &item.kind == kind).map(|item| item.body)
}

pub(super) fn box_header_len(data: &[u8]) -> Option<usize> {
    data.get(4..8)?;
    Some(if u32_at(data, 0)? == 1 { 16 } else { 8 })
}

/// A box around `body`, with a 64-bit size only when it needs one.
pub(super) fn boxed(kind: &[u8; 4], body: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(body.len() + 16);
    match u32::try_from(body.len() + 8) {
        Ok(size) => out.extend_from_slice(&size.to_be_bytes()),
        Err(_) => {
            out.extend_from_slice(&1_u32.to_be_bytes());
            out.extend_from_slice(kind);
            out.extend_from_slice(&(body.len() as u64 + 16).to_be_bytes());
            out.extend_from_slice(body);
            return out;
        }
    }
    out.extend_from_slice(kind);
    out.extend_from_slice(body);
    out
}

pub(super) fn u32_at(data: &[u8], at: usize) -> Option<u32> {
    Some(u32::from_be_bytes(data.get(at..at + 4)?.try_into().ok()?))
}

pub(super) fn u64_at(data: &[u8], at: usize) -> Option<u64> {
    Some(u64::from_be_bytes(data.get(at..at + 8)?.try_into().ok()?))
}

/// A top-level box of a file: where it starts, its header and whole length.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) struct TopBox {
    pub(super) kind: [u8; 4],
    pub(super) at: u64,
    pub(super) header_len: u64,
    pub(super) size: u64,
}

/// The file's top-level boxes in order; `None` when the box headers do not
/// add up to the file. Fewer than 8 trailing bytes are not a box and end the walk.
pub(super) fn top_level_boxes(file: &mut File, end: u64) -> io::Result<Option<Vec<TopBox>>> {
    let mut found = Vec::new();
    let mut position = 0_u64;
    while position + 8 <= end {
        if found.len() >= 1024 {
            return Ok(None);
        }
        let mut header = [0_u8; 16];
        file.seek(SeekFrom::Start(position))?;
        file.read_exact(&mut header[..8])?;
        let mut size = u64::from(u32::from_be_bytes(header[..4].try_into().unwrap()));
        let mut header_len = 8;
        if size == 1 {
            if position + 16 > end {
                return Ok(None);
            }
            file.read_exact(&mut header[8..])?;
            size = u64::from_be_bytes(header[8..].try_into().unwrap());
            header_len = 16;
        } else if size == 0 {
            size = end - position;
        }
        if size < header_len || position.checked_add(size).is_none_or(|next| next > end) {
            return Ok(None);
        }
        found.push(TopBox {
            kind: header[4..8].try_into().unwrap(),
            at: position,
            header_len,
            size,
        });
        position += size;
    }
    Ok(Some(found))
}
