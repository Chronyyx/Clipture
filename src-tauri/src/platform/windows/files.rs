//! File-system primitives with Windows semantics: write-through renames and
//! replacements, and sparse files.
//!
//! Sparse files: unwritten ranges take no disk space and need no zero-filling.
//! A normal NTFS file written far past its end first zero-fills the gap, so
//! a stream cache that receives a clip's index (at the end) first would write
//! hundreds of megabytes of zeros before playing anything.
use std::{
    fs::File,
    io,
    os::windows::{ffi::OsStrExt, io::AsRawHandle},
    path::Path,
};

use windows::{
    core::PCWSTR,
    Win32::{
        Foundation::HANDLE,
        Storage::FileSystem::{
            MoveFileExW, ReplaceFileW, MOVEFILE_REPLACE_EXISTING, MOVEFILE_WRITE_THROUGH, REPLACEFILE_WRITE_THROUGH,
        },
        System::{Ioctl::FSCTL_SET_SPARSE, IO::DeviceIoControl},
    },
};

/// Renames within one volume and returns only once the rename is on disk, so a
/// power loss cannot reorder it with a later journal write. A running
/// executable may be moved this way; it cannot be deleted or overwritten.
pub(crate) fn move_file(from: &Path, to: &Path, replace: bool) -> io::Result<()> {
    let wide = |path: &Path| path.as_os_str().encode_wide().chain(Some(0)).collect::<Vec<u16>>();
    let (from, to) = (wide(from), wide(to));
    let mut flags = MOVEFILE_WRITE_THROUGH;
    if replace {
        flags |= MOVEFILE_REPLACE_EXISTING;
    }
    // SAFETY: both strings are NUL-terminated and outlive the call.
    unsafe { MoveFileExW(PCWSTR(from.as_ptr()), PCWSTR(to.as_ptr()), flags) }
        .map_err(|error| io::Error::from_raw_os_error(error.code().0 & 0xFFFF))
}

/// Swaps `replacement` into `original`'s place in one step, keeping the
/// original's creation time, attributes and security descriptor.
pub(crate) fn replace_file(original: &Path, replacement: &Path) -> io::Result<()> {
    let wide = |path: &Path| path.as_os_str().encode_wide().chain(Some(0)).collect::<Vec<u16>>();
    let (original, replacement) = (wide(original), wide(replacement));
    // SAFETY: both strings are NUL-terminated and outlive the call; no backup
    // file or reserved pointers are passed.
    unsafe {
        ReplaceFileW(
            PCWSTR(original.as_ptr()),
            PCWSTR(replacement.as_ptr()),
            PCWSTR::null(),
            REPLACEFILE_WRITE_THROUGH,
            None,
            None,
        )
    }
    .map_err(io::Error::other)
}

pub(crate) fn make_sparse(file: &File) -> io::Result<()> {
    let mut returned = 0_u32;
    unsafe {
        DeviceIoControl(
            HANDLE(file.as_raw_handle()),
            FSCTL_SET_SPARSE,
            None,
            0,
            None,
            0,
            Some(&mut returned),
            None,
        )
    }
    .map_err(io::Error::other)
}

#[cfg(test)]
mod tests {
    use std::os::windows::fs::{FileExt, MetadataExt};

    #[test]
    fn a_write_far_past_the_end_reserves_no_space_for_the_gap() {
        let folder = tempfile::tempdir().unwrap();
        let path = folder.path().join("sparse");
        let file = File::create(&path).unwrap();
        super::make_sparse(&file).unwrap();
        file.seek_write(b"index", 512 * 1024 * 1024).unwrap();
        drop(file);
        assert_eq!(std::fs::metadata(&path).unwrap().len(), 512 * 1024 * 1024 + 5);
        const FILE_ATTRIBUTE_SPARSE_FILE: u32 = 0x200;
        assert_ne!(std::fs::metadata(&path).unwrap().file_attributes() & FILE_ATTRIBUTE_SPARSE_FILE, 0);
    }

    use std::fs::File;
}
