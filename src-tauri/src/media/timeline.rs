use std::{
    fs::File,
    io::{Read, Seek, SeekFrom},
    path::Path,
};

const MAXIMUM_MOOV_BYTES: u64 = 32 * 1024 * 1024;

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct PlaybackPatch {
    pub offset: u64,
    pub bytes: Vec<u8>,
}

#[derive(Clone, Copy, Debug)]
struct BoxView {
    kind: [u8; 4],
    start: usize,
    end: usize,
    payload: usize,
}

/// Finds positive audio edit-list media times that Electron zeroed while
/// streaming. Limits and checked arithmetic make malformed MP4 metadata a
/// harmless no-patch result.
pub fn audio_edit_list_patches(path: &Path) -> Vec<PlaybackPatch> {
    if !matches!(
        path.extension()
            .and_then(|value| value.to_str())
            .map(str::to_ascii_lowercase)
            .as_deref(),
        Some("mp4" | "m4v" | "mov")
    ) {
        return Vec::new();
    }
    find_patches(path).unwrap_or_default()
}

fn find_patches(path: &Path) -> std::io::Result<Vec<PlaybackPatch>> {
    let mut file = File::open(path)?;
    let file_size = file.metadata()?.len();
    let mut cursor = 0_u64;
    let mut moov = None;
    while cursor.saturating_add(8) <= file_size {
        let Some((kind, size, header)) = read_file_box_header(&mut file, cursor, file_size)? else {
            return Ok(Vec::new());
        };
        if kind == *b"moov" {
            moov = Some((cursor, size));
            break;
        }
        cursor = cursor.saturating_add(size);
        if size < header {
            return Ok(Vec::new());
        }
    }
    let Some((moov_offset, moov_size)) = moov else {
        return Ok(Vec::new());
    };
    if moov_size > MAXIMUM_MOOV_BYTES || moov_size > usize::MAX as u64 {
        return Ok(Vec::new());
    }
    file.seek(SeekFrom::Start(moov_offset))?;
    let mut data = vec![0_u8; moov_size as usize];
    file.read_exact(&mut data)?;
    let Some(root) = box_at(&data, 0, data.len()) else {
        return Ok(Vec::new());
    };
    if root.kind != *b"moov" {
        return Ok(Vec::new());
    }

    let mut patches = Vec::new();
    for track in children(&data, root)
        .into_iter()
        .filter(|view| view.kind == *b"trak")
    {
        let track_children = children(&data, track);
        let Some(media) = track_children
            .iter()
            .find(|view| view.kind == *b"mdia")
            .copied()
        else {
            continue;
        };
        let Some(edits) = track_children
            .iter()
            .find(|view| view.kind == *b"edts")
            .copied()
        else {
            continue;
        };
        if !is_audio_track(&data, media) {
            continue;
        }
        let Some(edit_list) = children(&data, edits)
            .into_iter()
            .find(|view| view.kind == *b"elst")
        else {
            continue;
        };
        patches.extend(edit_list_patches(&data, edit_list, moov_offset));
    }
    Ok(patches)
}

fn read_file_box_header(
    file: &mut File,
    start: u64,
    limit: u64,
) -> std::io::Result<Option<([u8; 4], u64, u64)>> {
    file.seek(SeekFrom::Start(start))?;
    let mut header = [0_u8; 16];
    file.read_exact(&mut header[..8])?;
    let mut size = u32::from_be_bytes(header[..4].try_into().unwrap()) as u64;
    let kind = header[4..8].try_into().unwrap();
    let header_size = if size == 1 {
        file.read_exact(&mut header[8..16])?;
        size = u64::from_be_bytes(header[8..16].try_into().unwrap());
        16
    } else {
        if size == 0 {
            size = limit.saturating_sub(start);
        }
        8
    };
    if size < header_size || start.checked_add(size).is_none_or(|end| end > limit) {
        return Ok(None);
    }
    Ok(Some((kind, size, header_size)))
}

fn box_at(data: &[u8], start: usize, limit: usize) -> Option<BoxView> {
    if start.checked_add(8)? > limit || limit > data.len() {
        return None;
    }
    let mut size = u32::from_be_bytes(data[start..start + 4].try_into().ok()?) as usize;
    let kind = data[start + 4..start + 8].try_into().ok()?;
    let header = if size == 1 {
        if start.checked_add(16)? > limit {
            return None;
        }
        let large = u64::from_be_bytes(data[start + 8..start + 16].try_into().ok()?);
        size = usize::try_from(large).ok()?;
        16
    } else {
        if size == 0 {
            size = limit - start;
        }
        8
    };
    let end = start.checked_add(size)?;
    if size < header || end > limit {
        return None;
    }
    Some(BoxView {
        kind,
        start,
        end,
        payload: start + header,
    })
}

fn children(data: &[u8], parent: BoxView) -> Vec<BoxView> {
    let mut result = Vec::new();
    let mut cursor = parent.payload;
    while cursor.saturating_add(8) <= parent.end {
        let Some(view) = box_at(data, cursor, parent.end) else {
            break;
        };
        result.push(view);
        if view.end <= cursor {
            break;
        }
        cursor = view.end;
    }
    result
}

fn is_audio_track(data: &[u8], media: BoxView) -> bool {
    let Some(handler) = children(data, media)
        .into_iter()
        .find(|view| view.kind == *b"hdlr")
    else {
        return false;
    };
    handler.payload.checked_add(12).is_some_and(|end| {
        end <= handler.end && data[handler.payload + 8..handler.payload + 12] == *b"soun"
    })
}

fn edit_list_patches(data: &[u8], list: BoxView, moov_offset: u64) -> Vec<PlaybackPatch> {
    if list.payload.saturating_add(8) > list.end {
        return Vec::new();
    }
    let version = data[list.payload];
    if version > 1 {
        return Vec::new();
    }
    let count = u32::from_be_bytes(data[list.payload + 4..list.payload + 8].try_into().unwrap());
    let entry_size = if version == 1 { 20_usize } else { 12_usize };
    let media_offset = if version == 1 { 8_usize } else { 4_usize };
    let media_size = if version == 1 { 8_usize } else { 4_usize };
    let mut cursor = list.payload + 8;
    let mut patches = Vec::new();
    for _ in 0..count {
        if cursor
            .checked_add(entry_size)
            .is_none_or(|end| end > list.end)
        {
            break;
        }
        let start = cursor + media_offset;
        let positive = if version == 1 {
            i64::from_be_bytes(data[start..start + 8].try_into().unwrap()) > 0
        } else {
            i32::from_be_bytes(data[start..start + 4].try_into().unwrap()) > 0
        };
        if positive {
            patches.push(PlaybackPatch {
                offset: moov_offset + start as u64,
                bytes: vec![0; media_size],
            });
        }
        cursor += entry_size;
    }
    patches
}

pub fn apply_patches(absolute_offset: u64, buffer: &mut [u8], patches: &[PlaybackPatch]) {
    let buffer_end = absolute_offset.saturating_add(buffer.len() as u64);
    for patch in patches {
        let patch_end = patch.offset.saturating_add(patch.bytes.len() as u64);
        let overlap_start = absolute_offset.max(patch.offset);
        let overlap_end = buffer_end.min(patch_end);
        if overlap_start >= overlap_end {
            continue;
        }
        let target_start = (overlap_start - absolute_offset) as usize;
        let source_start = (overlap_start - patch.offset) as usize;
        let length = (overlap_end - overlap_start) as usize;
        buffer[target_start..target_start + length]
            .copy_from_slice(&patch.bytes[source_start..source_start + length]);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn applies_partial_overlaps_at_absolute_offsets() {
        let patches = vec![PlaybackPatch {
            offset: 10,
            bytes: vec![0, 0, 0, 0],
        }];
        let mut chunk = vec![9_u8; 4];
        apply_patches(8, &mut chunk, &patches);
        assert_eq!(chunk, vec![9, 9, 0, 0]);
    }
}
