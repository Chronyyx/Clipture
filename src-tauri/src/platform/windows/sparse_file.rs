//! Sparse files: unwritten ranges take no disk space and need no zero-filling.
//! A normal NTFS file written far past its end first zero-fills the gap, so
//! a stream cache that receives a clip's index (at the end) first would write
//! hundreds of megabytes of zeros before playing anything.
use std::{fs::File, io, os::windows::io::AsRawHandle};

use windows::Win32::{
    Foundation::HANDLE,
    System::{Ioctl::FSCTL_SET_SPARSE, IO::DeviceIoControl},
};

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
