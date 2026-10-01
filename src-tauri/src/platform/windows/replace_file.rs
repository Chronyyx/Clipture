use std::{io, os::windows::ffi::OsStrExt, path::Path};

use windows::{
    core::PCWSTR,
    Win32::Storage::FileSystem::{ReplaceFileW, REPLACEFILE_WRITE_THROUGH},
};

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
