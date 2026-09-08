use tauri::{AppHandle, Manager};
use tauri_plugin_dialog::{DialogExt, FileDialogBuilder};

/// Native dialogs use callbacks. Waiting on a modal dialog must not occupy a
/// Tokio worker needed by recording, hotkeys, saves, or UI pipe replies.
pub async fn select<T: Send + 'static>(
    show: impl FnOnce(Box<dyn FnOnce(Option<T>) + Send>),
) -> super::CommandResult<Option<T>> {
    let (sender, receiver) = tokio::sync::oneshot::channel();
    show(Box::new(move |selection| {
        let _ = sender.send(selection);
    }));
    receiver
        .await
        .map_err(|_| "Native picker closed before returning a result".into())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn picker_selection_cancellation_and_lost_callback_are_distinct() {
        assert_eq!(
            tauri::async_runtime::block_on(select(|done| done(Some(7)))).unwrap(),
            Some(7)
        );
        assert_eq!(
            tauri::async_runtime::block_on(select::<u8>(|done| done(None))).unwrap(),
            None
        );
        assert!(tauri::async_runtime::block_on(select::<u8>(drop)).is_err());
    }
}

/// The resident controller owns picker results and their filesystem effects.
/// In the in-process reference UI, preserve its native modal parent. A detached
/// UI worker cannot lend a Tauri Window across processes, so use an unparented
/// native dialog there instead of trusting a renderer-supplied HWND or path.
pub fn desktop_picker(app: &AppHandle, title: &str) -> FileDialogBuilder<tauri::Wry> {
    let picker = app.dialog().file().set_title(title);
    match app.get_webview_window("main") {
        Some(window) => picker.set_parent(&window),
        None => picker,
    }
}
