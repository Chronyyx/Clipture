use serde_json::Value;
use std::{
    fs,
    io::Read,
    path::{Path, PathBuf},
};

/// The legacy uninstaller removes its install directory recursively. Refuse to
/// invoke it if known user data would be included, including junction aliases.
pub(super) fn verify(legacy: &Path, data: &Path) -> Result<(), String> {
    if !legacy.is_absolute() {
        return Err("Install directory must be absolute".into());
    }
    let legacy = legacy
        .canonicalize()
        .map_err(|e| format!("Inspect legacy installation: {e}"))?;
    if legacy.parent().is_none()
        || !legacy.join("resources/app.asar").is_file()
        || !legacy.join("Uninstall Clipture.exe").is_file()
    {
        return Err("Directory is not a recognized Electron installation".into());
    }
    for name in [
        "USERPROFILE",
        "APPDATA",
        "LOCALAPPDATA",
        "ProgramFiles",
        "ProgramFiles(x86)",
        "SystemRoot",
    ] {
        if let Some(root) =
            std::env::var_os(name).and_then(|p| PathBuf::from(p).canonicalize().ok())
        {
            if same_path(&legacy, &root) || (name == "SystemRoot" && within(&legacy, &root)) {
                return Err("Refusing to uninstall a system or profile directory".into());
            }
        }
    }
    protect(data, &legacy)?;
    if let Some(settings) = read_json(&data.join("settings.json"))? {
        if !settings.is_object() {
            return Err("Legacy settings must be an object".into());
        }
        if let Some(path) = settings.get("saveFolder").and_then(Value::as_str) {
            protect(Path::new(path), &legacy)?;
        }
        if let Some(directories) = settings
            .get("importedVideoDirectories")
            .and_then(Value::as_array)
        {
            for path in directories.iter().filter_map(Value::as_str) {
                protect(Path::new(path), &legacy)?;
            }
        }
    }
    if let Some(clips) = read_json(&data.join("clips.json"))? {
        let clips = clips
            .as_array()
            .ok_or("Legacy clip metadata must be an array")?;
        for clip in clips {
            if let Some(path) = clip.get("filePath").and_then(Value::as_str) {
                protect(Path::new(path), &legacy)?;
            }
            for segments in ["segmentFiles", "segments"]
                .iter()
                .filter_map(|field| clip.get(field).and_then(Value::as_array))
            {
                for segment in segments {
                    let path = segment
                        .as_str()
                        .or_else(|| segment.get("filePath").and_then(Value::as_str));
                    if let Some(path) = path {
                        protect(Path::new(path), &legacy)?;
                    }
                }
            }
        }
    }
    Ok(())
}

fn read_json(path: &Path) -> Result<Option<Value>, String> {
    const LIMIT: u64 = 64 * 1024 * 1024;
    let file = match fs::File::open(path) {
        Ok(file) => file,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(e) => return Err(format!("Inspect {}: {e}", path.display())),
    };
    let metadata = file
        .metadata()
        .map_err(|e| format!("Inspect {}: {e}", path.display()))?;
    if !metadata.is_file() || metadata.len() > LIMIT {
        return Err("Legacy metadata is not a bounded regular file".into());
    }
    let mut bytes = Vec::new();
    file.take(LIMIT + 1)
        .read_to_end(&mut bytes)
        .map_err(|e| format!("Read {}: {e}", path.display()))?;
    if bytes.len() as u64 > LIMIT {
        return Err("Legacy metadata grew beyond the safety limit".into());
    }
    serde_json::from_slice(&bytes)
        .map(Some)
        .map_err(|e| format!("Cannot safely read {}: {e}", path.display()))
}

fn protect(path: &Path, legacy: &Path) -> Result<(), String> {
    if path.as_os_str().is_empty() {
        return Ok(());
    }
    match path.canonicalize() {
        Ok(path) if within(&path, legacy) => Err(format!("User data is inside the old install folder: {}. Move it and update Clipture's settings before upgrading.", path.display())),
        Ok(_) => Ok(()),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(e) => Err(format!("Cannot check user data {}: {e}", path.display())),
    }
}
fn same_path(a: &Path, b: &Path) -> bool {
    a.as_os_str()
        .to_string_lossy()
        .eq_ignore_ascii_case(&b.as_os_str().to_string_lossy())
}
fn within(path: &Path, root: &Path) -> bool {
    path.ancestors().any(|ancestor| same_path(ancestor, root))
}

#[cfg(test)]
mod tests {
    use super::*;
    fn fixture() -> (tempfile::TempDir, PathBuf, PathBuf) {
        let root = tempfile::tempdir().unwrap();
        let legacy = root.path().join("Electron");
        let data = root.path().join("data");
        fs::create_dir_all(legacy.join("resources")).unwrap();
        fs::create_dir(&data).unwrap();
        fs::write(legacy.join("resources/app.asar"), b"fixture").unwrap();
        fs::write(legacy.join("Uninstall Clipture.exe"), b"fixture").unwrap();
        (root, legacy, data)
    }
    #[test]
    fn separate_data_is_preserved_without_any_writes() {
        let (_root, legacy, data) = fixture();
        let bytes = br#"{"saveFolder":"","unknown":"preserve"}"#;
        fs::write(data.join("settings.json"), bytes).unwrap();
        assert!(verify(&legacy, &data).is_ok());
        assert_eq!(fs::read(data.join("settings.json")).unwrap(), bytes);
    }
    #[test]
    fn nested_save_folder_is_rejected_before_uninstall() {
        let (_root, legacy, data) = fixture();
        let clips = legacy.join("User Clips");
        fs::create_dir(&clips).unwrap();
        fs::write(
            data.join("settings.json"),
            serde_json::json!({"saveFolder":clips}).to_string(),
        )
        .unwrap();
        assert!(verify(&legacy, &data)
            .unwrap_err()
            .contains("User data is inside"));
        assert!(clips.is_dir());
    }
    #[test]
    fn recorded_clip_inside_installation_is_rejected() {
        let (_root, legacy, data) = fixture();
        let clip = legacy.join("saved.mp4");
        fs::write(&clip, b"user clip").unwrap();
        fs::write(
            data.join("clips.json"),
            serde_json::json!([{"filePath":clip}]).to_string(),
        )
        .unwrap();
        assert!(verify(&legacy, &data).is_err());
        assert_eq!(fs::read(clip).unwrap(), b"user clip");
    }
    #[test]
    fn malformed_metadata_and_unrecognized_installations_fail_closed() {
        let (_root, legacy, data) = fixture();
        fs::write(data.join("settings.json"), b"{broken").unwrap();
        assert!(verify(&legacy, &data).is_err());
        assert!(verify(&data, &data).is_err());
    }

    #[test]
    fn nested_data_and_imported_directories_are_rejected() {
        let (_root, legacy, data) = fixture();
        let nested = legacy.join("user data");
        fs::create_dir(&nested).unwrap();
        assert!(verify(&legacy, &nested).is_err());
        fs::write(
            data.join("settings.json"),
            serde_json::json!({"importedVideoDirectories":[nested]}).to_string(),
        )
        .unwrap();
        assert!(verify(&legacy, &data).is_err());
    }

    #[test]
    fn both_segment_shapes_are_protected() {
        let (_root, legacy, data) = fixture();
        let segment = legacy.join("segment.mp4");
        fs::write(&segment, b"keep").unwrap();
        for value in [
            serde_json::json!(segment),
            serde_json::json!({"filePath":segment}),
        ] {
            fs::write(
                data.join("clips.json"),
                serde_json::json!([{"segments":[value]}]).to_string(),
            )
            .unwrap();
            assert!(verify(&legacy, &data).is_err());
        }
        assert_eq!(fs::read(segment).unwrap(), b"keep");
    }

    #[test]
    fn oversized_metadata_fails_before_allocation() {
        let (_root, legacy, data) = fixture();
        fs::File::create(data.join("clips.json"))
            .unwrap()
            .set_len(64 * 1024 * 1024 + 1)
            .unwrap();
        assert!(verify(&legacy, &data)
            .unwrap_err()
            .contains("bounded regular file"));
    }

    #[test]
    fn actual_clip_contract_segment_files_are_protected() {
        let (_root, legacy, data) = fixture();
        let segment = legacy.join("recorded-segment.mp4");
        fs::write(&segment, b"preserved").unwrap();
        let record = crate::contracts::ClipRecord {
            segment_files: Some(vec![segment.to_string_lossy().into_owned()]),
            ..Default::default()
        };
        fs::write(
            data.join("clips.json"),
            serde_json::to_vec(&vec![record]).unwrap(),
        )
        .unwrap();
        assert!(verify(&legacy, &data)
            .unwrap_err()
            .contains("User data is inside"));
        assert_eq!(fs::read(segment).unwrap(), b"preserved");
    }
}
