//! Remove only the two known Electron login-item identities, after backing them up.
use std::io;
use winreg::{
    enums::{HKEY_CURRENT_USER, KEY_READ, KEY_WRITE},
    RegKey,
};

const RUN: &str = r"Software\Microsoft\Windows\CurrentVersion\Run";
const BACKUP: &str = r"Software\Clipture\Migration\LegacyStartup";

pub(crate) fn remove_legacy_login_items() -> io::Result<()> {
    let user = RegKey::predef(HKEY_CURRENT_USER);
    let run = match user.open_subkey_with_flags(RUN, KEY_READ | KEY_WRITE) {
        Ok(key) => key,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(()),
        Err(error) => return Err(error),
    };
    for name in ["electron.app.Clipture", "app.clipture.desktop"] {
        let command: String = match run.get_value(name) {
            Ok(value) => value,
            Err(error) if error.kind() == io::ErrorKind::NotFound => continue,
            Err(error) => return Err(error),
        };
        if !is_legacy_command(name, &command) {
            continue;
        }
        let original = run.get_raw_value(name)?;
        let (backup, _) = user.create_subkey(BACKUP)?;
        match backup.get_raw_value(name) {
            Ok(saved) if saved != original => {
                return Err(io::Error::new(
                    io::ErrorKind::AlreadyExists,
                    "Legacy startup backup differs; refusing to overwrite it",
                ));
            }
            Ok(_) => {}
            Err(error) if error.kind() == io::ErrorKind::NotFound => {
                backup.set_raw_value(name, &original)?;
            }
            Err(error) => return Err(error),
        }
        // Do not delete an entry another process changed while we backed it up.
        if run.get_raw_value(name)? == original {
            run.delete_value(name)?;
        }
    }
    Ok(())
}

fn is_legacy_command(name: &str, command: &str) -> bool {
    if !["electron.app.Clipture", "app.clipture.desktop"].contains(&name) {
        return false;
    }
    let command = command.to_ascii_lowercase().replace('/', "\\");
    let command = command.trim();
    let executable = if let Some(quoted) = command.strip_prefix('"') {
        quoted.split('"').next().unwrap_or_default()
    } else if let Some(end) = command.find(".exe") {
        let end = end + 4;
        if !command[end..].is_empty() && !command[end..].starts_with(char::is_whitespace) {
            return false;
        }
        &command[..end]
    } else {
        return false;
    };
    // Packaged Electron, or the old repo-local Electron development launch.
    executable.ends_with("\\clipture\\clipture.exe")
        || executable.ends_with("\\clipture\\node_modules\\electron\\dist\\electron.exe")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn legacy_startup_only_matches_known_clipture_entries() {
        let installed = r#""C:\Users\Ada\AppData\Local\Programs\Clipture\Clipture.exe" --hidden"#;
        let development = r#"C:\source\Clipture\node_modules\electron\dist\electron.exe C:\source\Clipture --hidden"#;
        assert!(is_legacy_command("electron.app.Clipture", installed));
        assert!(is_legacy_command("app.clipture.desktop", development));
        assert!(!is_legacy_command("Clipture", installed));
        assert!(!is_legacy_command(
            "app.clipture.desktop",
            &format!("C:\\OtherApp.exe {installed}")
        ));
        assert!(!is_legacy_command(
            "electron.app.Clipture",
            r"C:\Clipture\clipture.exe.backup"
        ));
        assert!(!is_legacy_command("OtherApp", development));
        assert!(!is_legacy_command(
            "app.clipture.desktop",
            r"C:\OtherApp\electron.exe"
        ));
        assert!(!is_legacy_command(
            "electron.app.Clipture",
            r"C:\OtherApp\clipture.exe"
        ));
    }
}
