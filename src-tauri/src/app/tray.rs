use tauri::{
    menu::MenuBuilder,
    tray::{MouseButton, TrayIconBuilder, TrayIconEvent},
    AppHandle, Manager,
};

use crate::{contracts::SaveSource, error::AppResult, state::AppState};

pub fn create(app: &AppHandle) -> AppResult<()> {
    let menu = MenuBuilder::new(app)
        .text("open", "Open Clipture")
        .text("save", "Save Clip")
        .separator()
        .text("exit", "Exit")
        .build()?;

    let mut tray = TrayIconBuilder::with_id("main")
        .tooltip("Clipture")
        .menu(&menu)
        .on_menu_event(|app, event| match event.id.as_ref() {
            "open" => {
                if let Err(error) = super::windows::open_main(app) {
                    tracing::error!(%error, "could not open main window from tray");
                }
            }
            "save" => save_from_tray(app.clone()),
            "exit" => super::request_exit(app),
            _ => {}
        })
        .on_tray_icon_event(|tray, event| {
            if matches!(
                event,
                TrayIconEvent::DoubleClick {
                    button: MouseButton::Left,
                    ..
                }
            ) {
                if let Err(error) = super::windows::open_main(tray.app_handle()) {
                    tracing::error!(%error, "could not open main window from tray icon");
                }
            }
        });
    if let Some(icon) = app.default_window_icon() {
        tray = tray.icon(icon.clone());
    }
    tray.build(app)?;
    Ok(())
}

fn save_from_tray(app: AppHandle) {
    let state = app.state::<AppState>();
    let settings = state.settings.get();
    let saves = state.saves.clone();
    tauri::async_runtime::spawn(async move {
        let result = saves.save(&app, &settings, None, SaveSource::Tray).await;
        if !result.ok {
            tracing::warn!(message = %result.message, "tray save failed");
        }
    });
}
