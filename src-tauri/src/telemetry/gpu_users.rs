//! What else was using the GPU when recording dropped frames, as categories
//! (ADR 0035). Process names never leave the PC: each becomes one of a fixed
//! list of app kinds, and anything not on the list is "other app".
use clipture_telemetry_protocol::{Event, Level};

/// One process's GPU use over the probe window, in percent per engine type.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct GpuUser {
    pub name: String,
    /// Started by this Clipture process (its window's WebView).
    pub ours: bool,
    /// The recording engine itself.
    pub recorder: bool,
    pub encode: f64,
    pub decode: f64,
    pub three_d: f64,
}

/// Below this share of an engine a process is not worth naming.
const NOTABLE_PERCENT: f64 = 1.0;
/// 3D users named: the busiest few besides the game and the compositor.
const MOST_3D: usize = 3;

/// The app kind for an executable name.
pub fn category(exe: &str, ours: bool) -> &'static str {
    if ours {
        return "Clipture window";
    }
    match exe.to_ascii_lowercase().trim_end_matches(".exe") {
        "discord" | "discordptb" | "discordcanary" => "Discord",
        "nvidia share" | "nvcontainer" | "nvsphelper64" | "nvidia overlay" | "nvoawrappercache" => "NVIDIA overlay/ShadowPlay",
        "obs64" | "obs32" | "obs" => "OBS",
        "medal" => "Medal",
        "outplayed" | "overwolf" => "Overwolf/Outplayed",
        "streamlabs obs" | "streamlabs desktop" => "Streamlabs",
        "twitchstudio" | "twitch studio" => "Twitch Studio",
        "steam" | "steamwebhelper" => "Steam",
        "epicgameslauncher" | "epicwebhelper" => "Epic Games launcher",
        "chrome" | "msedge" | "firefox" | "brave" | "opera" | "opera_gx" | "vivaldi" | "arc" | "zen" | "librewolf" => "browser",
        "msedgewebview2" => "app web view",
        "spotify" => "Spotify",
        "vlc" | "mpc-hc64" | "mpc-be64" | "mpv" | "potplayermini64" | "potplayer" | "wmplayer" | "video.ui" | "microsoft.media.player" => "video player",
        "wallpaper32" | "wallpaper64" => "Wallpaper Engine",
        "lively" => "Lively Wallpaper",
        "gamebar" | "gamebarftserver" | "xboxgamebar" | "gamebarpresencewriter" => "Xbox Game Bar",
        "radeonsoftware" | "amdow" | "amdrsserv" => "AMD overlay",
        "dwm" => "Windows compositor",
        "svchost" => "Windows service",
        "robloxplayerbeta" | "robloxstudiobeta" => "Roblox",
        "clipture" => "Clipture window",
        _ => "other app",
    }
}

fn names(users: &[&GpuUser], encoding: bool) -> String {
    let mut kinds: Vec<&str> = Vec::new();
    for user in users {
        let mut kind = category(&user.name, user.ours);
        // The Windows service that encodes video is Game DVR, Xbox Game
        // Bar's background recording (BcastDVRUserService, in svchost).
        if encoding && kind == "Windows service" {
            kind = "Xbox Game Bar background recording";
        }
        if !kinds.contains(&kind) {
            kinds.push(kind);
        }
    }
    kinds.join(", ")
}

/// The `gpu_users` event for a probe: who encoded, who decoded, and the
/// busiest 3D users. `None` when nothing besides the recorder used them.
pub fn event(users: &[GpuUser], game: &str) -> Option<Event> {
    let others: Vec<&GpuUser> = users.iter().filter(|u| !u.recorder).collect();
    let encoders: Vec<&GpuUser> = others.iter().copied().filter(|u| u.encode >= NOTABLE_PERCENT).collect();
    let decoders: Vec<&GpuUser> = others.iter().copied().filter(|u| u.decode >= NOTABLE_PERCENT).collect();
    let game = game.to_ascii_lowercase();
    let mut three_d: Vec<&GpuUser> = others
        .iter()
        .copied()
        .filter(|u| u.three_d >= NOTABLE_PERCENT && u.name.to_ascii_lowercase() != game && category(&u.name, u.ours) != "Windows compositor")
        .collect();
    three_d.sort_by(|a, b| b.three_d.total_cmp(&a.three_d));
    three_d.truncate(MOST_3D);
    let recorder = |engine: fn(&GpuUser) -> f64| (users.iter().filter(|u| u.recorder).map(engine).sum::<f64>() * 10.0).round() / 10.0;
    let (recorder_encode, recorder_3d) = (recorder(|u| u.encode), recorder(|u| u.three_d));
    // The recorder's own share matters even alone: on a GPU at idle clocks
    // (the desktop) it can fill most of the 3D engine by itself.
    if encoders.is_empty() && decoders.is_empty() && three_d.is_empty() && recorder_3d < 50.0 {
        return None;
    }
    let mut event = Event::new(Level::Info, "gpu_users");
    if !encoders.is_empty() {
        event = event.with("encoding", names(&encoders, true));
    }
    if !decoders.is_empty() {
        event = event.with("decoding", names(&decoders, false));
    }
    if !three_d.is_empty() {
        event = event.with("rendering", names(&three_d, false));
    }
    Some(event.with("recorderEncodePercent", recorder_encode).with("recorderRenderPercent", recorder_3d))
}

/// One probe in flight at a time.
static PROBING: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

/// Samples GPU engine use on a background thread and records what it found
/// as a `gpu_users` event. The first probe of a run costs about half a
/// second of CPU on that thread (Windows loads its counter providers); later
/// ones about 16 ms, plus the sampling window spent waiting.
#[cfg(windows)]
pub fn probe(game: String) {
    use std::sync::atomic::Ordering;
    if PROBING.swap(true, Ordering::AcqRel) {
        return;
    }
    let spawned = std::thread::Builder::new().name("telemetry-gpu-users".into()).spawn(move || {
        let sampled = crate::platform::windows::gpu_engine_users::sample(std::time::Duration::from_millis(500));
        PROBING.store(false, Ordering::Release);
        let Ok(sampled) = sampled else { return };
        let processes = crate::platform::windows::gpu_engine_users::processes();
        let own = std::process::id();
        // WebView2 runs under the browser process Clipture starts, so a
        // window process is a child or grandchild of Clipture.
        let ours = |pid: u32| {
            let mut current = pid;
            for _ in 0..3 {
                match processes.get(&current) {
                    Some((parent, _)) if *parent == own => return true,
                    Some((parent, _)) if *parent != 0 && *parent != current => current = *parent,
                    _ => return false,
                }
            }
            false
        };
        let users: Vec<GpuUser> = sampled
            .into_iter()
            .map(|user| GpuUser {
                recorder: user.name.eq_ignore_ascii_case("clipture_engine.exe"),
                ours: user.pid == own || ours(user.pid),
                name: user.name,
                encode: user.encode,
                decode: user.decode,
                three_d: user.three_d,
            })
            .collect();
        if let Some(event) = event(&users, &game) {
            if let Some(service) = super::SINK.get().and_then(std::sync::Weak::upgrade) {
                service.external(event);
            }
        }
    });
    if spawned.is_err() {
        PROBING.store(false, Ordering::Release);
    }
}

#[cfg(not(windows))]
pub fn probe(_game: String) {}

#[cfg(test)]
mod tests {
    use super::*;

    fn user(name: &str, encode: f64, decode: f64, three_d: f64) -> GpuUser {
        GpuUser { name: name.into(), encode, decode, three_d, ..Default::default() }
    }

    /// What the event says on this PC right now (run with --ignored --nocapture).
    #[cfg(windows)]
    #[test]
    #[ignore]
    fn live_probe() {
        let sampled = crate::platform::windows::gpu_engine_users::sample(std::time::Duration::from_millis(500)).unwrap();
        for user in &sampled {
            eprintln!("{:<28} encode {:5.1} decode {:5.1} 3d {:5.1} -> {}", user.name, user.encode, user.decode, user.three_d, category(&user.name, false));
        }
        let users: Vec<GpuUser> = sampled.into_iter().map(|u| GpuUser { recorder: u.name.eq_ignore_ascii_case("clipture_engine.exe"), name: u.name, encode: u.encode, decode: u.decode, three_d: u.three_d, ..Default::default() }).collect();
        eprintln!("event: {:?}", event(&users, "").map(|e| e.data));
    }

    #[test]
    fn names_become_categories_and_unknown_apps_stay_anonymous() {
        assert_eq!(category("Discord.exe", false), "Discord");
        assert_eq!(category("NVIDIA Share.exe", false), "NVIDIA overlay/ShadowPlay");
        assert_eq!(category("msedgewebview2.exe", true), "Clipture window");
        assert_eq!(category("msedgewebview2.exe", false), "app web view");
        assert_eq!(category("my-secret-project.exe", false), "other app");
    }

    #[test]
    fn the_event_names_who_competed_for_the_encoder() {
        let users = [
            GpuUser { name: "clipture_engine.exe".into(), recorder: true, encode: 22.0, three_d: 3.0, ..Default::default() },
            user("Discord.exe", 31.0, 0.0, 2.0),
            user("chrome.exe", 0.0, 12.0, 8.0),
            user("dwm.exe", 0.0, 0.0, 15.0),
            user("deadlock.exe", 0.0, 0.0, 90.0),
            user("tool.exe", 0.4, 0.0, 0.0),
            user("svchost.exe", 10.0, 0.0, 0.0),
        ];
        let found = event(&users, "deadlock.exe").expect("others used the GPU");
        assert_eq!(found.code, "gpu_users");
        assert_eq!(found.data.get("encoding"), Some(&serde_json::json!("Discord, Xbox Game Bar background recording")));
        assert_eq!(found.data.get("decoding"), Some(&serde_json::json!("browser")));
        assert_eq!(found.data.get("rendering"), Some(&serde_json::json!("browser, Discord")));
        assert_eq!(found.data.get("recorderEncodePercent"), Some(&serde_json::json!(22.0)));
        assert_eq!(found.data.get("recorderRenderPercent"), Some(&serde_json::json!(3.0)));
        assert!(event(&[GpuUser { recorder: true, encode: 30.0, three_d: 20.0, ..Default::default() }], "").is_none());
        // Alone but filling the 3D engine: still worth recording.
        let alone = event(&[GpuUser { recorder: true, encode: 28.0, three_d: 78.0, ..Default::default() }], "").expect("recorder fills the GPU");
        assert_eq!(alone.data.get("recorderRenderPercent"), Some(&serde_json::json!(78.0)));
    }
}
