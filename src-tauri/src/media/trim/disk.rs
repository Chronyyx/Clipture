//! Positioned file access for the trimmer, behind a trait so tests can cut
//! the power at any point and see what survives.
use std::{
    fs::File,
    io::{self, Read, Seek, SeekFrom, Write},
    path::Path,
};

pub(in crate::media) trait Disk {
    fn read_at(&mut self, offset: u64, buffer: &mut [u8]) -> io::Result<()>;
    fn write_at(&mut self, offset: u64, bytes: &[u8]) -> io::Result<()>;
    /// Everything written so far is durable when this returns.
    fn sync(&mut self) -> io::Result<()>;
    fn set_len(&mut self, len: u64) -> io::Result<()>;
    fn len(&mut self) -> io::Result<u64>;
}

impl Disk for File {
    fn read_at(&mut self, offset: u64, buffer: &mut [u8]) -> io::Result<()> {
        self.seek(SeekFrom::Start(offset))?;
        self.read_exact(buffer)
    }

    fn write_at(&mut self, offset: u64, bytes: &[u8]) -> io::Result<()> {
        self.seek(SeekFrom::Start(offset))?;
        self.write_all(bytes)
    }

    fn sync(&mut self) -> io::Result<()> {
        self.sync_all()
    }

    fn set_len(&mut self, len: u64) -> io::Result<()> {
        File::set_len(self, len)
    }

    fn len(&mut self) -> io::Result<u64> {
        Ok(self.metadata()?.len())
    }
}

/// Opens `path` for reading and writing with no sharing: while the trimmer
/// holds it, nothing else can open the clip (and see it half moved), and it
/// cannot be opened while something else holds it, including the recorder,
/// which keeps a just-saved clip open while it still copies footage from it.
pub(in crate::media) fn open_exclusive(path: &Path, create: bool) -> io::Result<File> {
    let mut options = std::fs::OpenOptions::new();
    options.read(true).write(true);
    if create {
        options.create_new(true);
    }
    #[cfg(windows)]
    {
        use std::os::windows::fs::OpenOptionsExt;
        options.share_mode(0);
    }
    let file = options.open(path)?;
    lower_io_priority(&file);
    Ok(file)
}

/// Another process has the file open.
pub(in crate::media) fn is_in_use(error: &io::Error) -> bool {
    // ERROR_SHARING_VIOLATION, ERROR_LOCK_VIOLATION
    matches!(error.raw_os_error(), Some(32 | 33))
}

/// Recording shares the disk; the trim yields to it. Best effort.
fn lower_io_priority(file: &File) {
    #[cfg(windows)]
    {
        use std::os::windows::io::AsRawHandle;
        use windows::Win32::{
            Foundation::HANDLE,
            Storage::FileSystem::{FileIoPriorityHintInfo, IoPriorityHintLow, SetFileInformationByHandle, FILE_IO_PRIORITY_HINT_INFO},
        };
        let hint = FILE_IO_PRIORITY_HINT_INFO { PriorityHint: IoPriorityHintLow };
        // SAFETY: the handle is open for the duration of the call and the
        // buffer is the documented structure for this information class.
        let _ = unsafe {
            SetFileInformationByHandle(
                HANDLE(file.as_raw_handle()),
                FileIoPriorityHintInfo,
                std::ptr::from_ref(&hint).cast(),
                std::mem::size_of::<FILE_IO_PRIORITY_HINT_INFO>() as u32,
            )
        };
    }
}
