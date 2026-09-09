//! Window-only presentation. Reuses settings snapshots/events; no new host API.
use crate::contracts::{ClipSettings, UiTheme};
use tauri::{Listener, WebviewWindow, WindowEvent};

pub fn watch(window: &WebviewWindow) {
    apply_background(window, 0x111110);
    let themed_window = window.clone();
    let subscription = window.listen("settings://changed", move |event| {
        if let Ok(settings) = serde_json::from_str::<ClipSettings>(event.payload()) {
            apply(&themed_window, &settings);
        }
    });
    let closing_window = window.clone();
    window.on_window_event(move |event| {
        if matches!(event, WindowEvent::Destroyed) {
            closing_window.unlisten(subscription);
        }
    });
}

pub fn apply(window: &WebviewWindow, settings: &ClipSettings) {
    #[cfg(windows)]
    {
        let background = match settings.ui_theme {
            UiTheme::Light => 0xf1f1ed,
            UiTheme::Glitten => 0xe8decd,
            UiTheme::Milate => 0x4b4e24,
            UiTheme::Custom => parse_color(&settings.custom_main_color).unwrap_or(0x111110),
            _ => 0x111110,
        };
        apply_background(window, background);
    }
    #[cfg(not(windows))]
    let _ = (window, settings);
}

fn apply_background(window: &WebviewWindow, background: u32) {
    #[cfg(windows)]
    {
        let window = window.clone();
        let dispatcher = window.clone();
        // Win32 frame mutation stays on the window's owning thread.
        let _ = dispatcher.run_on_main_thread(move || {
            let _ = window.set_background_color(Some(tauri::webview::Color(
                (background >> 16) as u8,
                (background >> 8) as u8,
                background as u8,
                255,
            )));
            if let Ok(hwnd) = window.hwnd() {
                crate::platform::windows::style_main_caption(hwnd.0, background);
            }
        });
    }
    #[cfg(not(windows))]
    let _ = (window, background);
}

#[cfg(windows)]
fn parse_color(value: &str) -> Option<u32> {
    let hex = value.strip_prefix('#')?;
    (hex.len() == 6)
        .then(|| u32::from_str_radix(hex, 16).ok())
        .flatten()
}
