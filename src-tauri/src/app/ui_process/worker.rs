use super::{
    client::Client,
    pending::Pending,
    wire::{self, Message, PROTOCOL_VERSION},
};
use std::{sync::Arc, thread};
use tauri::{ipc::InvokeBody, Listener, Manager, WebviewUrl, WebviewWindowBuilder, WindowEvent};

pub fn run() -> Result<(), Box<dyn std::error::Error>> {
    // This role is entered before the single-instance plugin or AppState setup.
    // Its only bootstrap channel is the private stdin pipe inherited at spawn.
    let frame = wire::read_frame(&mut std::io::stdin())?.ok_or("Missing private UI bootstrap")?;
    let Message::Bootstrap {
        version,
        webview_directory,
        sounds_directory,
        smoke,
    } = frame.message
    else {
        return Err("Invalid UI bootstrap message".into());
    };
    if version != PROTOCOL_VERSION
        || !webview_directory.is_absolute()
        || !sounds_directory.is_absolute()
    {
        return Err("Unsupported UI bootstrap version or paths".into());
    }
    let client = Arc::new(Client {
        output: super::output::start(std::io::stdout()),
        pending: Arc::new(Pending::default()),
    });
    let invoke_client = client.clone();
    let setup_client = client.clone();
    let close_client = client.clone();
    let builder = super::worker_media::register(tauri::Builder::default(), client.clone());
    let application = builder
        .invoke_handler(move |invoke| {
            let command = invoke.message.command().to_owned();
            let InvokeBody::Json(args) = invoke.message.payload() else {
                invoke
                    .resolver
                    .reject("Only JSON host commands are accepted");
                return true;
            };
            let args = args.clone();
            let client = invoke_client.clone();
            tauri::async_runtime::spawn(async move {
                match client.invoke(command, args).await {
                    Ok(value) => invoke.resolver.resolve(value),
                    Err(error) => invoke.resolver.reject(error),
                }
            });
            true
        })
        .setup(move |application| {
            application
                .asset_protocol_scope()
                .allow_directory(&sounds_directory, false)?;
            let handle = application.handle().clone();
            let reader_client = setup_client.clone();
            thread::spawn(move || super::client::read(handle, reader_client));
            #[cfg(debug_assertions)]
            if smoke {
                let output = setup_client.output.clone();
                application.listen("clipture-smoke-result", move |event| {
                    if let Ok(report) = serde_json::from_str(event.payload()) {
                        let _ = output.try_send(Message::SmokeReport { report }.into());
                    }
                });
            }
            setup_client.output.send(
                Message::Ready {
                    version: PROTOCOL_VERSION,
                }
                .into(),
            )?;
            WebviewWindowBuilder::new(application, "main", WebviewUrl::App("index.html".into()))
                .title("Clipture")
                .inner_size(1240.0, 780.0)
                .min_inner_size(980.0, 640.0)
                .data_directory(webview_directory.clone())
                .on_page_load(move |window, payload| {
                    #[cfg(debug_assertions)]
                    if smoke && payload.event() == tauri::webview::PageLoadEvent::Finished {
                        let _ = window.eval(include_str!("../smoke.js"));
                    }
                    #[cfg(not(debug_assertions))]
                    let _ = (window, payload, smoke);
                })
                .build()?;
            Ok(())
        })
        .on_window_event(move |window, event| {
            if let WindowEvent::CloseRequested { api, .. } = event {
                api.prevent_close();
                let _ = close_client.output.try_send(Message::Closing {}.into());
                close_window(window.app_handle());
            }
        })
        .build(tauri::generate_context!())?;
    application.run(move |_app, event| {
        if matches!(event, tauri::RunEvent::Exit) {
            client.pending.close("UI closed");
        }
    });
    Ok(())
}

pub fn close_window(app: &tauri::AppHandle) {
    #[cfg(windows)]
    if let Some(window) = app.get_webview_window("main") {
        let closing = app.clone();
        if window
            .with_webview(move |webview| {
                let _ = unsafe { webview.controller().Close() };
                closing.exit(0);
            })
            .is_ok()
        {
            return;
        }
    }
    app.exit(0);
}
