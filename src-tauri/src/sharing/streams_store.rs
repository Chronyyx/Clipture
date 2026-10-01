//! Where a stream's bytes live: a temporary file in the system temp folder,
//! written block by block as runways deliver them, and deleted with the
//! session (leftovers from a crash are swept at start). On disk rather than
//! in memory, so clips of any size stay whole while watched (seeking back is
//! instant), and once every block is here the same file is the complete clip
//! FFmpeg mixes all audio tracks from.
use std::{
    collections::BTreeMap,
    io,
    os::windows::fs::FileExt,
    path::Path,
};

use super::BLOCK_BYTES;

/// Temporary stream files; leftovers from a crash are removed at start.
pub(super) const TEMP_PREFIX: &str = "clipture-stream-";

#[derive(Default)]
pub(super) struct BlockStore {
    /// Created with the first block.
    file: Option<tempfile::NamedTempFile>,
    /// Block index to its length (the last block may be short).
    present: BTreeMap<u64, u32>,
}

impl BlockStore {
    pub(super) fn contains(&self, block: u64) -> bool {
        self.present.contains_key(&block)
    }

    pub(super) fn count(&self) -> u64 {
        self.present.len() as u64
    }

    /// The file, once anything has arrived.
    pub(super) fn path(&self) -> Option<&Path> {
        self.file.as_ref().map(tempfile::NamedTempFile::path)
    }

    pub(super) fn put(&mut self, block: u64, bytes: &[u8]) -> io::Result<()> {
        if self.file.is_none() {
            let file = tempfile::Builder::new()
                .prefix(TEMP_PREFIX)
                .suffix(".mp4")
                .tempfile()?;
            // Blocks arrive out of order (the index first); without this,
            // NTFS zero-fills every gap before the block written past it.
            if let Err(error) = crate::platform::windows::make_sparse(file.as_file()) {
                tracing::debug!(%error, "stream cache is not sparse");
            }
            self.file = Some(file);
        }
        let file = self.file.as_ref().expect("created above").as_file();
        write_all_at(file, bytes, block * BLOCK_BYTES)?;
        self.present.insert(block, bytes.len() as u32);
        Ok(())
    }

    /// `length` bytes at `offset`, if every block they span has arrived.
    pub(super) fn read(&self, offset: u64, length: usize) -> Option<Vec<u8>> {
        let first = offset / BLOCK_BYTES;
        let last = (offset + length.max(1) as u64 - 1) / BLOCK_BYTES;
        let end = last * BLOCK_BYTES + u64::from(*self.present.get(&last)?);
        if (first..=last).any(|block| !self.contains(block)) || offset + length as u64 > end {
            return None;
        }
        let mut bytes = vec![0_u8; length];
        read_exact_at(self.file.as_ref()?.as_file(), &mut bytes, offset).ok()?;
        Some(bytes)
    }

    /// Arrived bytes as sorted, merged `[start, end)` ranges.
    pub(super) fn ranges(&self) -> Vec<[u64; 2]> {
        let mut ranges: Vec<[u64; 2]> = Vec::new();
        for (block, length) in &self.present {
            let start = block * BLOCK_BYTES;
            let end = start + u64::from(*length);
            match ranges.last_mut() {
                Some(last) if last[1] == start => last[1] = end,
                _ => ranges.push([start, end]),
            }
        }
        ranges
    }
}

fn write_all_at(file: &std::fs::File, mut bytes: &[u8], mut offset: u64) -> io::Result<()> {
    while !bytes.is_empty() {
        let written = file.seek_write(bytes, offset)?;
        if written == 0 {
            return Err(io::ErrorKind::WriteZero.into());
        }
        bytes = &bytes[written..];
        offset += written as u64;
    }
    Ok(())
}

fn read_exact_at(file: &std::fs::File, mut bytes: &mut [u8], mut offset: u64) -> io::Result<()> {
    while !bytes.is_empty() {
        let read = file.seek_read(bytes, offset)?;
        if read == 0 {
            return Err(io::ErrorKind::UnexpectedEof.into());
        }
        bytes = &mut bytes[read..];
        offset += read as u64;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn blocks_are_kept_on_disk_and_read_back_only_when_whole() {
        let mut store = BlockStore::default();
        assert!(store.path().is_none(), "nothing on disk until something arrives");
        let block = |seed: u8| vec![seed; BLOCK_BYTES as usize];
        store.put(0, &block(1)).unwrap();
        store.put(2, &[3; 100]).unwrap();
        assert_eq!(store.ranges(), [[0, BLOCK_BYTES], [2 * BLOCK_BYTES, 2 * BLOCK_BYTES + 100]]);
        assert_eq!(store.read(BLOCK_BYTES - 2, 2), Some(vec![1, 1]));
        assert!(store.read(BLOCK_BYTES - 2, 3).is_none(), "block 1 is missing");
        assert!(store.read(2 * BLOCK_BYTES + 90, 11).is_none(), "past the short last block");
        store.put(1, &block(2)).unwrap();
        assert_eq!(store.read(BLOCK_BYTES - 1, 2), Some(vec![1, 2]));
        assert_eq!(store.ranges(), [[0, 2 * BLOCK_BYTES + 100]]);
        let path = store.path().unwrap().to_owned();
        assert_eq!(std::fs::metadata(&path).unwrap().len(), 2 * BLOCK_BYTES + 100);
        drop(store);
        assert!(!path.exists(), "the temporary file goes with the session");
    }
}
