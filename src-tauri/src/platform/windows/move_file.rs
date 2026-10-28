use std::{io, os::windows::ffi::OsStrExt, path::Path};

use windows::{
    core::PCWSTR,
    Win32::Storage::FileSystem::{MoveFileExW, MOVEFILE_REPLACE_EXISTING, MOVEFILE_WRITE_THROUGH},
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
