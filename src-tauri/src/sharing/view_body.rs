//! Reading a shared clip through its linear view (see `media::LinearView`):
//! the view gathers its bytes from all over the clip, so each piece is read
//! on a blocking thread and handed to the async sender.
use std::{
    fs::File,
    future::Future,
    io,
    pin::Pin,
    sync::Arc,
    task::{ready, Context, Poll},
};

use tokio::{io::ReadBuf, task::JoinHandle};

use crate::media::LinearView;

/// Bytes read from the view per blocking read.
const PIECE: u64 = 1024 * 1024;

type Pending = JoinHandle<io::Result<(File, Vec<u8>)>>;

pub(super) struct ViewBody {
    view: Arc<LinearView>,
    /// Parked between reads; a read in progress owns it.
    file: Option<File>,
    position: u64,
    end: u64,
    buffer: Vec<u8>,
    consumed: usize,
    pending: Option<Pending>,
}

impl ViewBody {
    /// The view's bytes from `start` up to (not including) `end`.
    pub(super) fn new(view: Arc<LinearView>, start: u64, end: u64) -> Self {
        Self {
            view,
            file: None,
            position: start,
            end,
            buffer: Vec::new(),
            consumed: 0,
            pending: None,
        }
    }
}

impl tokio::io::AsyncRead for ViewBody {
    fn poll_read(mut self: Pin<&mut Self>, context: &mut Context<'_>, out: &mut ReadBuf<'_>) -> Poll<io::Result<()>> {
        let this = &mut *self;
        loop {
            if this.consumed < this.buffer.len() {
                let count = out.remaining().min(this.buffer.len() - this.consumed);
                out.put_slice(&this.buffer[this.consumed..this.consumed + count]);
                this.consumed += count;
                return Poll::Ready(Ok(()));
            }
            if let Some(pending) = this.pending.as_mut() {
                let finished = ready!(Pin::new(pending).poll(context));
                this.pending = None;
                let (file, bytes) = finished.map_err(io::Error::other)??;
                this.file = Some(file);
                this.position += bytes.len() as u64;
                this.buffer = bytes;
                this.consumed = 0;
                continue;
            }
            if this.position >= this.end || out.remaining() == 0 {
                return Poll::Ready(Ok(()));
            }
            let (view, file, at) = (this.view.clone(), this.file.take(), this.position);
            let count = PIECE.min(this.end - at) as usize;
            this.pending = Some(tokio::task::spawn_blocking(move || {
                let mut file = match file {
                    Some(file) => file,
                    None => view.open_source()?,
                };
                let mut bytes = vec![0_u8; count];
                view.read_at(&mut file, at, &mut bytes)?;
                Ok((file, bytes))
            }));
        }
    }
}
