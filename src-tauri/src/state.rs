use std::{
    path::PathBuf,
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc,
    },
};

use crate::{
    clips::{ClipRepository, ClipService},
    contracts::HostInfo,
    diagnostics::{ApplicationInfo, DiagnosticsBuilder},
    engine::EngineClient,
    library::{ImportedScanner, LibraryService},
    media::{AlwaysReady, ClipLayoutRepair, CommandFfmpeg, MediaService, MediaSessionRegistry},
    notifications::{NotificationService, NotificationSink},
    paths::AppPaths,
    platform::HostSystemInfo,
    processes::{IconSource, ProcessIconService, ProcessService, TasklistProvider},
    save::{SaveCoordinator, SaveIoAnalyzer},
    settings::SettingsStore,
    sharing::{ClipLibrary, Network, SharingEvents, SharingService},
    sounds::{
        BundledSound, DecodedSoundPlayer, SilentSoundPlayer, SoundLibrary, SoundPlayer,
        SoundService,
    },
};

pub struct AppState {
    pub paths: AppPaths,
    pub settings: Arc<SettingsStore>,
    pub engine: Arc<EngineClient>,
    pub saves: Arc<SaveCoordinator>,
    pub clips: Arc<ClipService>,
    pub library: Arc<LibraryService>,
    pub media: Arc<MediaService>,
    pub clip_repair: Arc<ClipLayoutRepair>,
    pub processes: Arc<ProcessService>,
    pub process_icons: Arc<ProcessIconService>,
    pub sounds: Arc<SoundLibrary>,
    pub notifications: Arc<NotificationService>,
    pub diagnostics: Arc<DiagnosticsBuilder>,
    pub save_io: Arc<SaveIoAnalyzer>,
    pub sharing: Arc<SharingService>,
    exiting: AtomicBool,
}

impl AppState {
    pub fn new(
        paths: AppPaths,
        settings: Arc<SettingsStore>,
        engine: Arc<EngineClient>,
        icon_source: Arc<dyn IconSource>,
        notification_sink: Arc<dyn NotificationSink>,
        sharing_events: Box<dyn SharingEvents>,
        runtime: tokio::runtime::Handle,
    ) -> Self {
        let repository = Arc::new(ClipRepository::new(paths.clips_file.clone()));
        let clips = Arc::new(ClipService::new(repository));
        let library = Arc::new(LibraryService::new(
            clips.clone(),
            ImportedScanner::default(),
        ));
        let ffmpeg = Arc::new(CommandFfmpeg::new(
            resolve_ffmpeg_path(&paths),
            Arc::new(AlwaysReady),
        ));
        let sessions = Arc::new(
            MediaSessionRegistry::new(if cfg!(windows) {
                "http://clipture-media.localhost"
            } else {
                "clipture-media://localhost"
            })
            .expect("the built-in media endpoint must be valid"),
        );
        let media = Arc::new(MediaService::new(sessions, ffmpeg.clone()));
        // Its own single FFmpeg slot: a long remux must never hold the slots
        // playback audio and thumbnails use.
        let clip_repair = Arc::new(ClipLayoutRepair::new(Arc::new(CommandFfmpeg::with_concurrency(
            resolve_ffmpeg_path(&paths),
            Arc::new(AlwaysReady),
            1,
        ))));
        let processes = Arc::new(ProcessService::new(Arc::new(TasklistProvider)));
        let process_icons = Arc::new(ProcessIconService::new(processes.clone(), icon_source));
        let sounds = Arc::new(SoundLibrary::new(
            paths.sounds_dir.clone(),
            bundled_sounds(),
        ));
        let player: Arc<dyn SoundPlayer> = if paths.test_mode {
            Arc::new(SilentSoundPlayer)
        } else {
            #[cfg(windows)]
            {
                Arc::new(DecodedSoundPlayer::new(
                    ffmpeg.clone(),
                    Arc::new(crate::platform::windows::WindowsWaveSoundSink),
                ))
            }
            #[cfg(not(windows))]
            {
                Arc::new(SilentSoundPlayer)
            }
        };
        let sound_playback = Arc::new(SoundService::new(sounds.clone(), player));
        let notifications = Arc::new(NotificationService::new(notification_sink));
        let save_io = Arc::new(SaveIoAnalyzer::new(cfg!(debug_assertions)));
        let saves = Arc::new(SaveCoordinator::new(
            engine.clone(),
            clips.clone(),
            sound_playback.clone(),
            notifications.clone(),
            save_io.clone(),
            Arc::new(crate::media::MediaSaveProcessor::new(ffmpeg)),
        ));
        let sharing = SharingService::new(
            paths.data_dir.join("sharing"),
            sharing_events,
            Arc::new(SharedClipLibrary {
                settings: settings.clone(),
                clips: clips.clone(),
            }),
            runtime,
            if paths.test_mode {
                Network::Disabled
            } else {
                Network::Internet
            },
        );
        media.set_remote_source(sharing.clone());
        let diagnostics = Arc::new(DiagnosticsBuilder::new(
            ApplicationInfo {
                name: "Clipture".into(),
                version: env!("CARGO_PKG_VERSION").into(),
                packaged: !cfg!(debug_assertions),
            },
            Arc::new(HostSystemInfo),
        ));

        Self {
            paths,
            settings,
            engine,
            saves,
            clips,
            library,
            media,
            clip_repair,
            processes,
            process_icons,
            sounds,
            notifications,
            diagnostics,
            save_io,
            sharing,
            exiting: AtomicBool::new(false),
        }
    }

    pub fn host_info(&self, version: String) -> HostInfo {
        HostInfo {
            host: "tauri",
            version,
            test_mode: self.paths.test_mode,
            capabilities: vec![
                "host_info",
                "settings",
                "engine",
                "clips",
                "library",
                "media",
                "processes",
                "sounds",
                "diagnostics",
                "native-notifications",
                "host://ready",
                "settings://changed",
                "engine://hotkey",
                "engine://diagnostics",
                "engine://status",
                "clip://save",
                "library://changed",
                "get_update_state",
                "check_for_updates",
                "download_update",
                "install_update",
                "updates://state-changed",
                "capture-aware-updates",
                "lazy-disposable-webview",
                "single-instance",
                "autostart",
                "sharing",
                "sharing://changed",
            ],
        }
    }

    pub fn begin_exit(&self) {
        self.exiting.store(true, Ordering::Release);
    }

    pub fn is_exiting(&self) -> bool {
        self.exiting.load(Ordering::Acquire)
    }
}

/// Kept clips from friends go under the user's save folder and join the
/// saved-clip library like any recorded clip.
struct SharedClipLibrary {
    settings: Arc<SettingsStore>,
    clips: Arc<ClipService>,
}

impl ClipLibrary for SharedClipLibrary {
    fn shared_clips_folder(&self) -> PathBuf {
        PathBuf::from(self.settings.get().save_folder).join("Shared")
    }

    fn publish(&self, record: crate::contracts::ClipRecord) -> crate::error::AppResult<()> {
        self.clips.append_saved(record)
    }

    /// Hidden (dot) folders are skipped by the library scan.
    fn outgoing_copies_folder(&self) -> PathBuf {
        PathBuf::from(self.settings.get().save_folder).join(".clipture-sharing")
    }
}

fn resolve_ffmpeg_path(paths: &AppPaths) -> PathBuf {
    if !cfg!(debug_assertions) {
        return std::env::current_exe().expect("current executable is available")
            .parent().expect("executable parent is available").join("ffmpeg.exe");
    }
    if let Some(path) = &paths.ffmpeg_override {
        return path.clone();
    }
    let executable_sibling = std::env::current_exe()
        .ok()
        .and_then(|path| path.parent().map(|parent| parent.join("ffmpeg.exe")));
    // Prefer the staged, hash-verified sidecar (scripts/fetch-ffmpeg.cjs) over
    // the older npm copy kept only for the legacy Electron build.
    let development = std::env::current_dir().ok().map(|root| {
        [
            root.join("src-tauri/binaries/ffmpeg-x86_64-pc-windows-msvc.exe"),
            root.join("node_modules/ffmpeg-static/ffmpeg.exe"),
        ]
    });
    executable_sibling
        .into_iter()
        .chain(development.into_iter().flatten())
        .find(|candidate| candidate.is_file())
        .unwrap_or_else(|| PathBuf::from("ffmpeg.exe"))
}

fn bundled_sounds() -> Vec<BundledSound> {
    let resource_root = std::env::current_exe()
        .ok()
        .and_then(|path| path.parent().map(|parent| parent.join("assets")));
    let development_root = std::env::current_dir().ok().map(|root| root.join("assets"));
    let root = resource_root
        .into_iter()
        .chain(development_root)
        .find(|candidate| candidate.is_dir())
        .unwrap_or_else(|| PathBuf::from("assets"));
    vec![
        BundledSound {
            file_name: "default.mp3".into(),
            label: "Default".into(),
            source: root.join("default.mp3"),
        },
        BundledSound {
            file_name: "option2.wav".into(),
            label: "Option 2".into(),
            source: root.join("option2.wav"),
        },
    ]
}
