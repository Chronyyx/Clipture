use std::{io, os::windows::ffi::OsStrExt, path::{Path, PathBuf}};

use windows::{
    core::{Interface, PCWSTR},
    Win32::{
        System::Com::{CoCreateInstance, CoInitializeEx, IPersistFile, CLSCTX_INPROC_SERVER, COINIT_APARTMENTTHREADED, STGM_READ},
        UI::Shell::{IShellLinkW, ShellLink},
    },
};

fn wide(path: &Path) -> Vec<u16> { path.as_os_str().encode_wide().chain(Some(0)).collect() }

fn shell_link() -> io::Result<IShellLinkW> {
    // SAFETY: COM is initialized for this thread (repeat calls are harmless)
    // before the in-process ShellLink object is created.
    unsafe {
        let _ = CoInitializeEx(None, COINIT_APARTMENTTHREADED);
        CoCreateInstance(&ShellLink, None, CLSCTX_INPROC_SERVER).map_err(io::Error::other)
    }
}

/// The expanded target of a shell link. A 64-bit caller sees the real path,
/// not the raw `%ProgramFiles%` form or a 32-bit redirection of it.
pub(crate) fn shortcut_target(link: &Path) -> Option<PathBuf> {
    let shell = shell_link().ok()?;
    let mut buffer = vec![0_u16; 32_768];
    // SAFETY: the link path is NUL-terminated; GetPath writes at most the
    // buffer length and no find data is requested.
    unsafe {
        shell.cast::<IPersistFile>().ok()?.Load(PCWSTR(wide(link).as_ptr()), STGM_READ).ok()?;
        shell.GetPath(&mut buffer, std::ptr::null_mut(), 0).ok()?;
    }
    let length = buffer.iter().position(|unit| *unit == 0).unwrap_or(buffer.len());
    (length > 0).then(|| PathBuf::from(String::from_utf16_lossy(&buffer[..length])))
}

/// Creates or overwrites a shell link to `target`, starting in its directory.
pub(crate) fn write_shortcut(link: &Path, target: &Path) -> io::Result<()> {
    let shell = shell_link()?;
    let directory = target.parent().unwrap_or(target);
    // SAFETY: all strings are NUL-terminated and outlive the calls.
    unsafe {
        shell.SetPath(PCWSTR(wide(target).as_ptr())).map_err(io::Error::other)?;
        shell.SetWorkingDirectory(PCWSTR(wide(directory).as_ptr())).map_err(io::Error::other)?;
        shell.cast::<IPersistFile>().map_err(io::Error::other)?
            .Save(PCWSTR(wide(link).as_ptr()), true).map_err(io::Error::other)
    }
}
