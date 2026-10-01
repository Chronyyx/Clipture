//! Finds the user's default web browser and an installed Discord build, so
//! they can be offered as separate audio tracks. Read-only registry and
//! filesystem queries; nothing is launched.
use std::{
    env, fs,
    path::{Path, PathBuf},
};

use winreg::{
    enums::{HKEY_CLASSES_ROOT, HKEY_CURRENT_USER},
    RegKey,
};

use crate::settings::DetectedAudioApp;

const URL_CHOICE: &str = r"Software\Microsoft\Windows\Shell\Associations\UrlAssociations";

pub(crate) fn detect_default_audio_apps() -> Vec<DetectedAudioApp> {
    [default_browser(), installed_discord()].into_iter().flatten().collect()
}

fn default_browser() -> Option<DetectedAudioApp> {
    let user = RegKey::predef(HKEY_CURRENT_USER);
    let prog_id: String = ["https", "http"].iter().find_map(|scheme| {
        user.open_subkey(format!(r"{URL_CHOICE}\{scheme}\UserChoice"))
            .ok()?
            .get_value("ProgId")
            .ok()
    })?;
    let classes = RegKey::predef(HKEY_CLASSES_ROOT);
    let command: String = classes
        .open_subkey(format!(r"{prog_id}\shell\open\command"))
        .ok()?
        .get_value("")
        .ok()?;
    // The handler's own display name tells Chromium from Chrome, which share
    // chrome.exe. Resource references ("@...") fall back to the process name.
    let registered_name = classes
        .open_subkey(format!(r"{prog_id}\Application"))
        .and_then(|key| key.get_value::<String, _>("ApplicationName"))
        .ok()
        .filter(|name| !name.trim().is_empty() && !name.starts_with('@'));
    let executable = executable_from_command(&command)?;
    if !executable.is_file() {
        return None;
    }
    let process_name = executable.file_name()?.to_string_lossy().into_owned();
    Some(DetectedAudioApp {
        id: "app-default-browser",
        label: registered_name.unwrap_or_else(|| browser_label(&process_name)),
        process_name,
        executable_path: Some(executable.to_string_lossy().into_owned()),
    })
}

/// `"C:\path\app.exe" --flag "%1"` or `C:\path\app.exe %1` → the executable.
fn executable_from_command(command: &str) -> Option<PathBuf> {
    let command = command.trim();
    let path = if let Some(rest) = command.strip_prefix('"') {
        rest.split('"').next()?
    } else {
        let end = command.to_ascii_lowercase().find(".exe")? + 4;
        &command[..end]
    };
    let path = PathBuf::from(path.trim());
    path.extension()
        .is_some_and(|extension| extension.eq_ignore_ascii_case("exe"))
        .then_some(path)
}

fn browser_label(process_name: &str) -> String {
    match process_name.to_ascii_lowercase().as_str() {
        "chrome.exe" => "Google Chrome".into(),
        "msedge.exe" => "Microsoft Edge".into(),
        "firefox.exe" => "Firefox".into(),
        "brave.exe" => "Brave".into(),
        "opera.exe" | "launcher.exe" => "Opera".into(),
        "vivaldi.exe" => "Vivaldi".into(),
        "zen.exe" => "Zen Browser".into(),
        "arc.exe" => "Arc".into(),
        _ => process_name.trim_end_matches(".exe").trim_end_matches(".EXE").into(),
    }
}

fn installed_discord() -> Option<DetectedAudioApp> {
    let local = PathBuf::from(env::var_os("LOCALAPPDATA")?);
    [
        ("Discord", "Discord.exe", "Discord"),
        ("DiscordPTB", "DiscordPTB.exe", "Discord PTB"),
        ("DiscordCanary", "DiscordCanary.exe", "Discord Canary"),
    ]
    .into_iter()
    .find_map(|(folder, process, label)| {
        let root = local.join(folder);
        root.join("Update.exe").is_file().then(|| DetectedAudioApp {
            id: "app-discord",
            label: label.into(),
            process_name: process.into(),
            executable_path: newest_app_build(&root, process).map(|path| path.to_string_lossy().into_owned()),
        })
    })
}

/// Discord installs each update into `app-<version>`; the newest one holds the
/// executable whose icon the settings page shows.
fn newest_app_build(root: &Path, process: &str) -> Option<PathBuf> {
    let version = |name: &str| -> Vec<u32> { name.split('.').filter_map(|part| part.parse().ok()).collect() };
    fs::read_dir(root)
        .ok()?
        .flatten()
        .filter_map(|entry| {
            let name = entry.file_name().to_string_lossy().into_owned();
            let executable = entry.path().join(process);
            (name.starts_with("app-") && executable.is_file()).then(|| (version(&name[4..]), executable))
        })
        .max_by(|left, right| left.0.cmp(&right.0))
        .map(|(_, executable)| executable)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn browser_commands_resolve_to_their_executable() {
        assert_eq!(
            executable_from_command(r#""C:\Program Files\Google\Chrome\Application\chrome.exe" --single-argument %1"#),
            Some(PathBuf::from(r"C:\Program Files\Google\Chrome\Application\chrome.exe"))
        );
        assert_eq!(
            executable_from_command(r"C:\Apps\Firefox\firefox.exe -osint -url %1"),
            Some(PathBuf::from(r"C:\Apps\Firefox\firefox.exe"))
        );
        assert_eq!(executable_from_command("rundll32 url.dll,FileProtocolHandler %1"), None);
        assert_eq!(browser_label("MSEdge.exe"), "Microsoft Edge");
        assert_eq!(browser_label("thorium.exe"), "thorium");
    }

    /// Manual check of this machine: cargo test -- --ignored --nocapture detects_this
    #[test]
    #[ignore]
    fn detects_this_machines_apps() {
        for app in detect_default_audio_apps() {
            println!("{app:?}");
        }
    }

    #[test]
    fn newest_discord_build_wins_by_version_not_by_name() {
        let root = tempfile::tempdir().unwrap();
        for version in ["app-1.0.9", "app-1.0.10"] {
            fs::create_dir(root.path().join(version)).unwrap();
            fs::write(root.path().join(version).join("Discord.exe"), b"").unwrap();
        }
        assert_eq!(
            newest_app_build(root.path(), "Discord.exe"),
            Some(root.path().join("app-1.0.10").join("Discord.exe"))
        );
    }
}
