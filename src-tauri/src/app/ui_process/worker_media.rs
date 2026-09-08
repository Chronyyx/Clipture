use super::{client::Client, wire::Message};
use std::{sync::Arc, time::Duration};
use tauri::http::{
    header::{ORIGIN, RANGE},
    Request, Response,
};

pub fn register(
    builder: tauri::Builder<tauri::Wry>,
    client: Arc<Client>,
) -> tauri::Builder<tauri::Wry> {
    builder.register_asynchronous_uri_scheme_protocol(
        "clipture-media",
        move |_context, request, responder| {
            let client = client.clone();
            tauri::async_runtime::spawn(async move {
                let response = forward(client, request).await.unwrap_or_else(|error| {
                    Response::builder()
                        .status(503)
                        .body(error.into_bytes())
                        .unwrap()
                });
                responder.respond(response);
            });
        },
    )
}

async fn forward(
    client: Arc<Client>,
    request: Request<Vec<u8>>,
) -> Result<Response<Vec<u8>>, String> {
    if !request.body().is_empty() || request.uri().to_string().len() > 1024 {
        return Err("Invalid media request".into());
    }
    let method = request.method().to_string();
    let uri = request.uri().to_string();
    let header = |name: tauri::http::header::HeaderName| {
        if request.headers().get_all(&name).iter().count() > 1 {
            return Err("Duplicate media header".to_string());
        }
        request
            .headers()
            .get(name)
            .map(|value| {
                value
                    .to_str()
                    .map(str::to_owned)
                    .map_err(|error| error.to_string())
            })
            .transpose()
    };
    let range = header(RANGE)?;
    let origin = header(ORIGIN)?;
    let frame = client
        .request(
            |id| Message::Media {
                id,
                method,
                uri,
                range,
                origin,
            },
            Duration::from_secs(45),
        )
        .await?;
    let Message::MediaReply {
        status, headers, ..
    } = frame.message
    else {
        return Err("Unexpected media reply".into());
    };
    let mut builder = Response::builder().status(status);
    for (name, value) in headers {
        builder = builder.header(name, value);
    }
    builder
        .body(frame.binary)
        .map_err(|error| error.to_string())
}
