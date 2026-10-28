//! Elevated, one-time removal of a Program Files installation that a
//! per-user installation superseded (ADR 0020). It touches only machine-wide
//! state: the old directory, its HKLM registrations, all-users shortcuts, and
//! firewall rules naming executables that are going away. The user's choice
//! in those rules moves to the per-user executable, so Windows does not ask
//! again. Per-user state was already moved by the unelevated installer.
use std::{ffi::OsString, path::{Path, PathBuf}};

const FLAG: &str = "--remove-machine-install";
const UNINSTALL_KEY: &str = r"Software\Microsoft\Windows\CurrentVersion\Uninstall\Clipture";
const FILES: [&str; 6] = ["clipture.exe", "clipture_engine.exe", "ffmpeg.exe", r"assets\default.mp3", r"assets\option2.wav", "uninstall.exe"];

pub fn command() -> Option<i32> {
    let args: Vec<OsString> = std::env::args_os().skip(1).collect();
    if args.first().map(OsString::as_os_str) != Some(FLAG.as_ref()) {
        return None;
    }
    let result = match &args[1..] {
        [machine, replacement, store] => remove(Path::new(machine), Path::new(replacement), Path::new(store)),
        _ => Err("Expected the old directory, the new executable and the runtime store".into()),
    };
    Some(match result {
        Ok(()) => 0,
        Err(error) => {
            eprintln!("Program Files cleanup: {error}");
            13
        }
    })
}

#[cfg(windows)]
fn remove(machine: &Path, replacement: &Path, store: &Path) -> Result<(), String> {
    // SAFETY: no arguments; reads the current token.
    if !unsafe { windows::Win32::UI::Shell::IsUserAnAdmin() }.as_bool() {
        return Err("Must run elevated".into());
    }
    let registered = registered_machine_location().ok_or("No Program Files installation is registered")?;
    let machine = machine.canonicalize().map_err(|error| format!("Old directory: {error}"))?;
    if registered.canonicalize().ok().as_ref() != Some(&machine) {
        return Err("Directory is not the registered Program Files installation".into());
    }
    if !machine.join("uninstall.exe").is_file() || !machine.join("clipture.exe").is_file() {
        return Err("Directory does not look like a Clipture installation".into());
    }
    let replacement = replacement.canonicalize().map_err(|error| format!("New executable: {error}"))?;
    if replacement.starts_with(&machine) || !replacement.ends_with("clipture.exe") {
        return Err("New executable must be a separate clipture.exe".into());
    }
    if !store.ends_with(r"Clipture\runtime") {
        return Err("Unexpected runtime store path".into());
    }
    if let Err(error) = move_firewall_rules(&machine.join("clipture.exe"), &replacement, store) {
        // Losing the rule only means Windows asks once more.
        eprintln!("Firewall rules were not moved: {error}");
    }
    // Delete files before registrations: if anything still runs from the
    // old directory, stop here and leave a working installation behind.
    for name in FILES {
        match std::fs::remove_file(machine.join(name)) {
            Err(error) if error.kind() != std::io::ErrorKind::NotFound => return Err(format!("{name}: {error}")),
            _ => {}
        }
    }
    let _ = std::fs::remove_dir(machine.join("assets"));
    let _ = std::fs::remove_dir(&machine);
    remove_registrations(&machine)?;
    for shortcut in all_users_shortcuts() {
        if shortcut_targets(&shortcut, &machine) {
            let _ = std::fs::remove_file(&shortcut);
        }
    }
    Ok(())
}

#[cfg(not(windows))]
fn remove(_: &Path, _: &Path, _: &Path) -> Result<(), String> { Err("Windows only".into()) }

#[cfg(windows)]
fn registered_machine_location() -> Option<PathBuf> {
    use winreg::{enums::HKEY_LOCAL_MACHINE, RegKey};
    let location: String = RegKey::predef(HKEY_LOCAL_MACHINE).open_subkey(UNINSTALL_KEY).ok()?.get_value("InstallLocation").ok()?;
    Some(PathBuf::from(location.trim().trim_matches('"')))
}

#[cfg(windows)]
fn remove_registrations(machine: &Path) -> Result<(), String> {
    use winreg::{enums::HKEY_LOCAL_MACHINE, RegKey};
    let hive = RegKey::predef(HKEY_LOCAL_MACHINE);
    hive.delete_subkey_all(UNINSTALL_KEY).map_err(|error| format!("Uninstall entry: {error}"))?;
    let _ = hive.delete_subkey_all(r"Software\clipture\Clipture");
    let _ = hive.delete_subkey(r"Software\clipture");
    // The per-user `clipture:` handler under HKCU already takes precedence;
    // drop the machine one only if it still names the old directory.
    let command: Option<String> = hive.open_subkey(r"Software\Classes\clipture\shell\open\command").ok().and_then(|key| key.get_value("").ok());
    if command.is_some_and(|command| names(&command, machine)) {
        let _ = hive.delete_subkey_all(r"Software\Classes\clipture");
    }
    Ok(())
}

fn all_users_shortcuts() -> Vec<PathBuf> {
    let mut shortcuts = Vec::new();
    if let Some(data) = std::env::var_os("ProgramData").map(PathBuf::from) {
        let programs = data.join(r"Microsoft\Windows\Start Menu\Programs");
        shortcuts.push(programs.join("Clipture.lnk"));
        shortcuts.push(programs.join(r"Clipture\Clipture.lnk"));
    }
    if let Some(public) = std::env::var_os("PUBLIC").map(PathBuf::from) {
        shortcuts.push(public.join(r"Desktop\Clipture.lnk"));
    }
    shortcuts
}

/// Shell links store their target as ANSI and as UTF-16 at either alignment.
fn shortcut_targets(shortcut: &Path, machine: &Path) -> bool {
    let Ok(bytes) = std::fs::read(shortcut) else { return false };
    let wide = |offset: usize| {
        let units: Vec<u16> = bytes.get(offset..).unwrap_or_default().chunks_exact(2)
            .map(|pair| u16::from_le_bytes([pair[0], pair[1]])).collect();
        String::from_utf16_lossy(&units)
    };
    names(&String::from_utf8_lossy(&bytes), machine) || names(&wide(0), machine) || names(&wide(1), machine)
}

fn names(text: &str, machine: &Path) -> bool {
    let directory = machine.to_string_lossy();
    let directory = directory.trim_start_matches(r"\\?\").to_lowercase();
    text.to_lowercase().contains(&directory)
}

/// Copies each distinct rule for the old executable or a runtime-store
/// version to the new executable, then deletes the old rules. PowerShell
/// receives paths through the environment, never through its command text.
#[cfg(windows)]
fn move_firewall_rules(old: &Path, new: &Path, store: &Path) -> Result<(), String> {
    use base64::Engine;
    const SCRIPT: &str = r#"
$ErrorActionPreference = 'Stop'
$store = $env:CLIPTURE_STORE.TrimEnd('\') + '\'
$filters = @(Get-NetFirewallApplicationFilter | Where-Object {
  $_.Program -and ($_.Program -ieq $env:CLIPTURE_OLD -or $_.Program.StartsWith($store, [StringComparison]::OrdinalIgnoreCase)) })
$covered = @(Get-NetFirewallApplicationFilter | Where-Object { $_.Program -ieq $env:CLIPTURE_NEW }).Count -gt 0
$seen = @{}
foreach ($rule in @($filters | Get-NetFirewallRule)) {
  $protocol = ($rule | Get-NetFirewallPortFilter).Protocol
  $key = "$($rule.Direction)|$($rule.Action)|$($rule.Profile)|$protocol"
  if (-not $covered -and -not $seen.ContainsKey($key)) {
    $seen[$key] = $true
    New-NetFirewallRule -DisplayName 'Clipture' -Direction $rule.Direction -Action $rule.Action -Profile $rule.Profile -Protocol $protocol -Program $env:CLIPTURE_NEW | Out-Null
  }
  Remove-NetFirewallRule -Name $rule.Name
}
"#;
    let strip = |path: &Path| path.to_string_lossy().trim_start_matches(r"\\?\").to_owned();
    let encoded = base64::engine::general_purpose::STANDARD
        .encode(SCRIPT.encode_utf16().flat_map(u16::to_le_bytes).collect::<Vec<u8>>());
    let status = std::process::Command::new("powershell.exe")
        .args(["-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-EncodedCommand", &encoded])
        .env("CLIPTURE_OLD", strip(old))
        .env("CLIPTURE_NEW", strip(new))
        .env("CLIPTURE_STORE", strip(store))
        .status()
        .map_err(|error| error.to_string())?;
    status.success().then_some(()).ok_or_else(|| format!("PowerShell ended with {status}"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn shortcut_match_ignores_case_and_verbatim_prefixes() {
        let root = tempfile::tempdir().unwrap();
        let link = root.path().join("Clipture.lnk");
        let target: Vec<u8> = r"C:\Program Files\Clipture\clipture.exe".encode_utf16().flat_map(u16::to_le_bytes).collect();
        std::fs::write(&link, [b"L\0\0\0".as_slice(), &target].concat()).unwrap();
        assert!(shortcut_targets(&link, Path::new(r"\\?\C:\PROGRAM FILES\Clipture")));
        assert!(!shortcut_targets(&link, Path::new(r"C:\Users\someone\AppData\Local\Programs\Clipture")));
    }

    #[test]
    fn helper_ignores_unrelated_command_lines() {
        assert!(names(r#""C:\Program Files\Clipture\clipture.exe" "%1""#, Path::new(r"C:\Program Files\Clipture")));
        assert!(!names(r#""C:\Users\a\AppData\Local\Programs\Clipture\clipture.exe" "%1""#, Path::new(r"C:\Program Files\Clipture")));
    }
}
