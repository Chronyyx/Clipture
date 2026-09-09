use tauri::{AppHandle, Emitter, Manager, WebviewUrl, WebviewWindowBuilder, Window, WindowEvent};

use crate::{error::AppResult, state::AppState};

const MAIN_WINDOW_LABEL: &str = "main";

pub fn open_main(app: &AppHandle) -> AppResult<()> {
    if super::ui_process::enabled() {
        return super::ui_process::open(app);
    }
    if let Some(window) = app.get_webview_window(MAIN_WINDOW_LABEL) {
        // The renderer reveals the window after its initial theme/DOM commit.
        if !window.is_visible()? {
            return Ok(());
        }
        if window.is_minimized()? {
            window.unminimize()?;
        }
        window.show()?;
        window.set_focus()?;
        return Ok(());
    }

    let builder =
        WebviewWindowBuilder::new(app, MAIN_WINDOW_LABEL, WebviewUrl::App("index.html".into()))
            .title("")
            .background_color(tauri::webview::Color(17, 17, 16, 255))
            .inner_size(1240.0, 780.0)
            .min_inner_size(980.0, 640.0)
            .visible(false)
            .on_page_load(|window, payload| {
                #[cfg(debug_assertions)]
                if payload.event() == tauri::webview::PageLoadEvent::Finished {
                    super::smoke::on_loaded(&window);
                }
                #[cfg(not(debug_assertions))]
                let _ = (window, payload);
            });
    #[cfg(debug_assertions)]
    let builder = super::smoke::prepare_window(builder, app);
    let window = builder.build()?;
    super::window_appearance::watch(&window);
    super::window_appearance::apply(&window, &app.state::<AppState>().settings.get());
    let _ = window.emit("host://ready", ());
    Ok(())
}

pub fn destroy_main(app: &AppHandle) -> AppResult<()> {
    if super::ui_process::enabled() {
        super::ui_process::close(app);
        return Ok(());
    }
    release_main_media_sessions(app);
    if let Some(window) = app.get_webview_window(MAIN_WINDOW_LABEL) {
        #[cfg(windows)]
        {
            let closing = window.clone();
            window.with_webview(move |webview| {
                // Close the browser controller while its parent HWND is still
                // valid. Tauri's window destroy otherwise drops it afterwards.
                // Explicit Close also releases WebView2 event-handler cycles.
                if let Err(error) = unsafe { webview.controller().Close() } {
                    tracing::warn!(%error, "could not close the WebView2 controller");
                }
                if let Err(error) = closing.destroy() {
                    tracing::warn!(%error, "could not destroy the main window");
                }
            })?;
        }
        #[cfg(not(windows))]
        window.destroy()?;
    }
    Ok(())
}

pub fn handle_window_event(window: &Window, event: &WindowEvent) {
    if window.label() != MAIN_WINDOW_LABEL {
        return;
    }
    match event {
        WindowEvent::CloseRequested { api, .. } => {
            let exiting = window
                .app_handle()
                .try_state::<AppState>()
                .is_some_and(|state| state.is_exiting());
            if !exiting {
                api.prevent_close();
                if let Err(error) = destroy_main(window.app_handle()) {
                    tracing::warn!(%error, "could not destroy main WebView");
                }
            }
        }
        WindowEvent::Destroyed => release_main_media_sessions(window.app_handle()),
        _ => {}
    }
}

fn release_main_media_sessions(app: &AppHandle) {
    if let Some(state) = app.try_state::<AppState>() {
        state.media.release_owner(MAIN_WINDOW_LABEL);
        state.process_icons.clear();
        state.processes.invalidate();
    }
}
