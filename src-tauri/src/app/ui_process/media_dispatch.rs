use super::wire::{Frame, Message};
use crate::media::MediaService;
use std::sync::Arc;
use tauri::http::{
    header::{ORIGIN, RANGE},
    Request,
};

pub fn respond(media: Arc<MediaService>, owner: &str, request: Message) -> Frame {
    let Message::Media {
        id,
        method,
        uri,
        range,
        origin,
    } = request
    else {
        unreachable!("media dispatcher only receives media messages")
    };
    if method.len() > 16
        || uri.len() > 1024
        || range.as_ref().is_some_and(|value| value.len() > 128)
        || origin.as_ref().is_some_and(|value| value.len() > 1024)
    {
        return failure(id, 400, "Invalid media request");
    }
    let mut builder = Request::builder().method(method.as_str()).uri(&uri);
    if let Some(range) = range {
        builder = builder.header(RANGE, range);
    }
    if let Some(origin) = origin {
        builder = builder.header(ORIGIN, origin);
    }
    let request = match builder.body(Vec::new()) {
        Ok(request) => request,
        Err(_) => return failure(id, 400, "Invalid media headers"),
    };
    let response = crate::media::handle_protocol_request(Some(media), owner, request);
    let (parts, binary) = response.into_parts();
    Frame {
        message: Message::MediaReply {
            id,
            status: parts.status.as_u16(),
            headers: parts
                .headers
                .iter()
                .filter_map(|(name, value)| Some((name.to_string(), value.to_str().ok()?.into())))
                .collect(),
        },
        binary,
    }
}

pub fn failure(id: u64, status: u16, message: &str) -> Frame {
    Frame {
        message: Message::MediaReply {
            id,
            status,
            headers: vec![("content-type".into(), "text/plain".into())],
        },
        binary: message.as_bytes().to_vec(),
    }
}
