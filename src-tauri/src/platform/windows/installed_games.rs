use std::{
    collections::{HashSet, VecDeque},
    env, fs,
    path::{Path, PathBuf},
    sync::OnceLock,
};

#[derive(Clone)]
struct InstalledTarget {
    name: String,
    root: PathBuf,
}

static TARGETS: OnceLock<Vec<InstalledTarget>> = OnceLock::new();

pub fn installed_executable(candidates: &[String]) -> Option<PathBuf> {
    let aliases: HashSet<_> = candidates
        .iter()
        .flat_map(|candidate| {
            let name = candidate
                .rsplit(['/', '\\'])
                .next()
                .unwrap_or(candidate)
                .to_ascii_lowercase();
            let stem = name.trim_end_matches(".exe").to_owned();
            [name, stem.clone(), format!("{stem}.exe")]
        })
        .collect();
    installed_targets()
        .iter()
        .filter(|target| aliases.contains(&target.name.to_ascii_lowercase()))
        .find_map(|target| find_executable(&target.root, &aliases))
}

fn installed_targets() -> &'static [InstalledTarget] {
    TARGETS.get_or_init(|| {
        let mut targets = steam_targets();
        targets.extend(epic_targets());
        targets
    })
}

fn steam_targets() -> Vec<InstalledTarget> {
    let default_root = env::var_os("ProgramFiles(x86)")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(r"C:\Program Files (x86)"))
        .join("Steam");
    let mut roots = vec![default_root.clone()];
    if let Ok(text) = fs::read_to_string(default_root.join("steamapps/libraryfolders.vdf")) {
        roots.extend(
            manifest_values(&text, "path")
                .into_iter()
                .map(|value| PathBuf::from(value.replace("\\\\", "\\"))),
        );
    }
    let mut targets = Vec::new();
    for root in roots {
        let steam_apps = root.join("steamapps");
        let Ok(entries) = fs::read_dir(&steam_apps) else {
            continue;
        };
        for entry in entries.flatten() {
            let name = entry.file_name().to_string_lossy().to_ascii_lowercase();
            if !name.starts_with("appmanifest_") || !name.ends_with(".acf") {
                continue;
            }
            let Ok(text) = fs::read_to_string(entry.path()) else {
                continue;
            };
            let (Some(display_name), Some(directory)) = (
                manifest_value(&text, "name"),
                manifest_value(&text, "installdir"),
            ) else {
                continue;
            };
            targets.push(InstalledTarget {
                name: display_name,
                root: steam_apps.join("common").join(directory),
            });
        }
    }
    targets
}

fn epic_targets() -> Vec<InstalledTarget> {
    let root = env::var_os("ProgramData")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(r"C:\ProgramData"))
        .join("Epic/EpicGamesLauncher/Data/Manifests");
    let Ok(entries) = fs::read_dir(root) else {
        return Vec::new();
    };
    entries
        .flatten()
        .filter(|entry| {
            entry
                .path()
                .extension()
                .and_then(|value| value.to_str())
                .is_some_and(|value| value.eq_ignore_ascii_case("item"))
        })
        .filter_map(|entry| fs::read_to_string(entry.path()).ok())
        .filter_map(|text| {
            Some(InstalledTarget {
                name: manifest_value(&text, "DisplayName")?,
                root: PathBuf::from(manifest_value(&text, "InstallLocation")?),
            })
        })
        .collect()
}

fn manifest_values(text: &str, field: &str) -> Vec<String> {
    text.lines()
        .filter_map(|line| quoted_field_value(line, field))
        .collect()
}

fn manifest_value(text: &str, field: &str) -> Option<String> {
    manifest_values(text, field).into_iter().next().or_else(|| {
        let needle = format!("\"{field}\"");
        let tail = text.split_once(&needle)?.1.trim_start();
        let value = tail.strip_prefix(':')?.trim_start().strip_prefix('"')?;
        Some(value.split_once('"')?.0.into())
    })
}

fn quoted_field_value(line: &str, field: &str) -> Option<String> {
    let line = line.trim_start().strip_prefix('"')?;
    let (key, tail) = line.split_once('"')?;
    if !key.eq_ignore_ascii_case(field) {
        return None;
    }
    let tail = tail.trim_start();
    let tail = tail.strip_prefix(':').unwrap_or(tail).trim_start();
    let value = tail.strip_prefix('"')?.split_once('"')?.0;
    Some(value.to_owned())
}

fn find_executable(root: &Path, aliases: &HashSet<String>) -> Option<PathBuf> {
    if !root.is_dir() {
        return None;
    }
    let mut queue = VecDeque::from([root.to_owned()]);
    let mut scanned = 0_usize;
    while let Some(directory) = queue.pop_front() {
        let Ok(entries) = fs::read_dir(&directory) else {
            continue;
        };
        for entry in entries.flatten() {
            if scanned >= 2_500 {
                return None;
            }
            scanned += 1;
            let Ok(file_type) = entry.file_type() else {
                continue;
            };
            if file_type.is_symlink() {
                continue;
            }
            let name = entry.file_name().to_string_lossy().to_ascii_lowercase();
            if file_type.is_file() && aliases.contains(&name) {
                return Some(entry.path());
            }
            if file_type.is_dir()
                && (directory == root
                    || matches!(
                        name.as_str(),
                        "bin" | "binaries" | "game" | "win64" | "windows" | "retail" | "csgo"
                    ))
            {
                queue.push_back(entry.path());
            }
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_vdf_and_json_manifest_fields() {
        assert_eq!(
            manifest_value(r#""name"  "A Game""#, "name").as_deref(),
            Some("A Game")
        );
        assert_eq!(
            manifest_value(r#"{"DisplayName":"B Game"}"#, "DisplayName").as_deref(),
            Some("B Game")
        );
    }
}
