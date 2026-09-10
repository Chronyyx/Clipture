//! Reveal an already-authorized file without Explorer command-line parsing.
use std::{os::windows::ffi::OsStrExt, path::Path};

use windows::{
    core::PCWSTR,
    Win32::{
        Foundation::RPC_E_CHANGED_MODE,
        System::Com::{
            CoInitializeEx, CoTaskMemFree, CoUninitialize, IBindCtx, COINIT_APARTMENTTHREADED,
        },
        UI::Shell::{Common::ITEMIDLIST, SHOpenFolderAndSelectItems, SHParseDisplayName},
    },
};

use crate::error::{AppError, AppResult};

struct Apartment(bool);
impl Apartment {
    fn enter() -> AppResult<Self> {
        let result = unsafe { CoInitializeEx(None, COINIT_APARTMENTTHREADED) };
        if result == RPC_E_CHANGED_MODE {
            // The blocking-pool thread already owns a different COM apartment.
            return Ok(Self(false));
        }
        result.ok().map_err(shell_error)?;
        Ok(Self(true))
    }
}
impl Drop for Apartment {
    fn drop(&mut self) {
        if self.0 {
            unsafe { CoUninitialize() };
        }
    }
}

struct ShellItem(*mut ITEMIDLIST);
impl Drop for ShellItem {
    fn drop(&mut self) {
        unsafe { CoTaskMemFree(Some(self.0.cast())) };
    }
}

fn shell_error(error: windows::core::Error) -> AppError {
    AppError::Integration(format!(
        "could not reveal clip in Windows Explorer: {error}"
    ))
}

// Rust canonicalization produces verbatim paths. Shell parsing expects normal
// filesystem syntax. Preserve UTF-16 exactly, including spaces/commas/Unicode;
// reject non-filesystem device namespaces rather than treating them as folders.
fn shell_path(path: &Path) -> AppResult<Vec<u16>> {
    let mut value: Vec<u16> = path.as_os_str().encode_wide().collect();
    if !path.is_absolute() || value.contains(&0) {
        return Err(AppError::Path(
            "Explorer requires an absolute filesystem path".into(),
        ));
    }
    if value.starts_with(&[92, 92, 63, 92]) {
        if value.starts_with(&[92, 92, 63, 92, 85, 78, 67, 92]) {
            value.drain(..6); // \\?\UNC\server -> \\server
            value[0] = 92;
        } else if value.len() >= 7
            && value[5] == 58
            && value[6] == 92
            && ((65..=90).contains(&value[4]) || (97..=122).contains(&value[4]))
        {
            value.drain(..4);
        } else {
            return Err(AppError::Path("unsupported Explorer device path".into()));
        }
    }
    value.push(0);
    Ok(value)
}

fn parse_item(path: &Path) -> AppResult<ShellItem> {
    let name = shell_path(path)?;
    let mut item = ShellItem(std::ptr::null_mut());
    unsafe {
        SHParseDisplayName(
            PCWSTR(name.as_ptr()),
            None::<&IBindCtx>,
            &mut item.0,
            0,
            None,
        )
    }
    .map_err(shell_error)?;
    if item.0.is_null() {
        return Err(AppError::Path(
            "Windows could not resolve the clip location".into(),
        ));
    }
    Ok(item)
}

/// Call on a blocking worker, after library path authorization. A zero selection
/// count tells Shell to open the item's parent and select that exact item.
pub(crate) fn reveal_file(path: &Path) -> AppResult<()> {
    let _apartment = Apartment::enter()?;
    let item = parse_item(path)?;
    unsafe { SHOpenFolderAndSelectItems(item.0, None, 0) }.map_err(shell_error)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn shell_paths_preserve_drive_unc_spaces_commas_and_unicode() {
        for (input, expected) in [
            (
                r"\\?\C:\Clips\Game, highlights\café 01.mp4",
                r"C:\Clips\Game, highlights\café 01.mp4",
            ),
            (
                r"\\?\UNC\server\clips\Game one.mp4",
                r"\\server\clips\Game one.mp4",
            ),
            (r"C:\Clips\normal.mp4", r"C:\Clips\normal.mp4"),
        ] {
            let actual = shell_path(Path::new(input)).unwrap();
            assert_eq!(actual.last(), Some(&0));
            assert_eq!(
                String::from_utf16(&actual[..actual.len() - 1]).unwrap(),
                expected
            );
        }
        assert!(shell_path(Path::new("relative.mp4")).is_err());
        assert!(shell_path(Path::new("C:\\clip\0.mp4")).is_err());
        assert!(shell_path(Path::new(r"\\?\GLOBALROOT\Device\HarddiskVolume1\clip.mp4")).is_err());
    }

    #[test]
    fn canonical_authorized_clip_resolves_to_the_same_shell_file_without_opening_ui() {
        use windows::Win32::UI::Shell::SHGetPathFromIDListW;
        let root = tempfile::tempdir().unwrap();
        let folder = root.path().join("Game, highlights café");
        std::fs::create_dir(&folder).unwrap();
        let clip = folder.join("Clip 01.mp4");
        std::fs::write(&clip, b"fixture").unwrap();
        let authority =
            crate::clips::PathAuthorizer::from_records([crate::contracts::ClipRecord {
                id: "fixture".into(),
                file_path: clip.to_string_lossy().into(),
                ..Default::default()
            }]);
        let _apartment = Apartment::enter().unwrap();
        let item = parse_item(authority.primary("fixture").unwrap()).unwrap();
        let mut path = [0u16; 260];
        assert!(unsafe { SHGetPathFromIDListW(item.0, &mut path) }.as_bool());
        let end = path.iter().position(|&c| c == 0).unwrap();
        assert_eq!(
            Path::new(&String::from_utf16(&path[..end]).unwrap())
                .canonicalize()
                .unwrap(),
            clip.canonicalize().unwrap()
        );
    }
}
