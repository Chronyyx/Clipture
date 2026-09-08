use std::{
    collections::HashMap,
    process::{Command, Stdio},
    sync::{Arc, Mutex},
    time::{Duration, Instant},
};

use serde::{Deserialize, Serialize};

use crate::error::{AppError, AppResult};

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ActiveProcess {
    pub name: String,
    pub pid: u32,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub executable_path: Option<String>,
}

pub trait ProcessProvider: Send + Sync {
    fn list(&self) -> AppResult<Vec<ActiveProcess>>;
}

#[derive(Default)]
pub struct TasklistProvider;

impl ProcessProvider for TasklistProvider {
    fn list(&self) -> AppResult<Vec<ActiveProcess>> {
        let mut command = Command::new("tasklist.exe");
        command
            .args(["/fo", "csv", "/nh"])
            .stdin(Stdio::null())
            .stderr(Stdio::null())
            .stdout(Stdio::piped());
        configure_hidden(&mut command);
        let output = command.output().map_err(|source| AppError::Io {
            action: "enumerate Windows processes",
            path: "tasklist.exe".into(),
            source,
        })?;
        if !output.status.success() {
            return Err(AppError::Integration(format!(
                "tasklist exited with code {:?}",
                output.status.code()
            )));
        }
        let text = String::from_utf8_lossy(&output.stdout);
        Ok(text.lines().filter_map(parse_tasklist_line).collect())
    }
}

#[derive(Clone)]
struct CachedSnapshot {
    expires_at: Instant,
    processes: Vec<ActiveProcess>,
}

pub struct ProcessService {
    provider: Arc<dyn ProcessProvider>,
    cache_ttl: Duration,
    cached: Mutex<Option<CachedSnapshot>>,
}

impl ProcessService {
    pub fn new(provider: Arc<dyn ProcessProvider>) -> Self {
        Self::with_cache_ttl(provider, Duration::from_secs(5))
    }

    pub fn with_cache_ttl(provider: Arc<dyn ProcessProvider>, cache_ttl: Duration) -> Self {
        Self {
            provider,
            cache_ttl,
            cached: Mutex::new(None),
        }
    }

    pub fn list(&self) -> AppResult<Vec<ActiveProcess>> {
        if let Some(cached) = self
            .cached
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .as_ref()
            .filter(|cached| cached.expires_at > Instant::now())
            .cloned()
        {
            return Ok(cached.processes);
        }
        Ok(self.remember(self.provider.list()?))
    }

    /// Accept only host-enumerated snapshots, never renderer-supplied paths.
    pub fn remember(&self, processes: Vec<ActiveProcess>) -> Vec<ActiveProcess> {
        let processes = normalize_processes(processes);
        *self
            .cached
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner()) = Some(CachedSnapshot {
            expires_at: Instant::now() + self.cache_ttl,
            processes: processes.clone(),
        });
        processes
    }

    pub fn invalidate(&self) {
        *self
            .cached
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner()) = None;
    }
}

fn normalize_processes(processes: Vec<ActiveProcess>) -> Vec<ActiveProcess> {
    let mut by_name: HashMap<String, ActiveProcess> = HashMap::new();
    for mut process in processes {
        process.name = process.name.trim().to_owned();
        if process.name.is_empty() || process.pid == 0 {
            continue;
        }
        process.executable_path = process
            .executable_path
            .take()
            .filter(|path| !path.trim().is_empty());
        let key = process.name.to_ascii_lowercase();
        let replace = by_name.get(&key).is_none_or(|existing| {
            existing.executable_path.is_none() && process.executable_path.is_some()
        });
        if replace {
            by_name.insert(key, process);
        }
    }
    let mut processes: Vec<_> = by_name.into_values().collect();
    processes.sort_by(|left, right| {
        left.name
            .to_ascii_lowercase()
            .cmp(&right.name.to_ascii_lowercase())
    });
    processes
}

fn parse_tasklist_line(line: &str) -> Option<ActiveProcess> {
    let columns = parse_csv(line);
    let name = columns.first()?.trim().to_owned();
    let pid = columns.get(1)?.trim().parse().ok()?;
    (!name.is_empty() && pid > 0).then_some(ActiveProcess {
        name,
        pid,
        executable_path: None,
    })
}

fn parse_csv(line: &str) -> Vec<String> {
    let mut values = Vec::new();
    let mut value = String::new();
    let mut quoted = false;
    let mut characters = line.chars().peekable();
    while let Some(character) = characters.next() {
        match character {
            '"' if quoted && characters.peek() == Some(&'"') => {
                value.push('"');
                characters.next();
            }
            '"' => quoted = !quoted,
            ',' if !quoted => values.push(std::mem::take(&mut value)),
            character => value.push(character),
        }
    }
    values.push(value);
    values
}

#[cfg(windows)]
fn configure_hidden(command: &mut Command) {
    use std::os::windows::process::CommandExt;
    command.creation_flags(0x0800_0000 | 0x0000_4000);
}

#[cfg(not(windows))]
fn configure_hidden(_: &mut Command) {}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn tasklist_csv_parser_handles_quoted_commas_and_escaped_quotes() {
        let columns = parse_csv(r#""game, ""special"".exe","42","Console""#);
        assert_eq!(columns[0], "game, \"special\".exe");
        assert_eq!(columns[1], "42");
    }

    #[test]
    fn duplicate_names_prefer_an_executable_path() {
        let output = normalize_processes(vec![
            ActiveProcess {
                name: "Game.exe".into(),
                pid: 1,
                executable_path: None,
            },
            ActiveProcess {
                name: "game.exe".into(),
                pid: 2,
                executable_path: Some("C:\\Game.exe".into()),
            },
        ]);
        assert_eq!(output.len(), 1);
        assert_eq!(output[0].pid, 2);
    }
}
