//! The bytes that follow a `Range` response.
//!
//! Plain: exactly `length` raw bytes.
//!
//! Sparse (when the requester asked for it and the sender understands it):
//! segments of a one-byte kind and an eight-byte big-endian length, until
//! `length` bytes are described. Kind 0 is a run of zero bytes and carries no
//! data; kind 1 is followed by that many data bytes. Clipture's in-place
//! recordings can be half zero padding, so this roughly halves what a friend's
//! upload has to carry. Integrity is unchanged: receivers still verify the
//! whole file's BLAKE3 digest before keeping it.
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};

const ZEROS: u8 = 0;
const DATA: u8 = 1;
/// The sender inspects the file in pieces of this size for zero runs.
const PIECE: usize = 64 * 1024;
/// Data is sent in segments of at most this size.
const MAX_DATA_SEGMENT: usize = 1024 * 1024;

/// Sends `length` bytes of `source`, eliding zero runs when `sparse`.
pub async fn send_body(
    mut source: impl AsyncRead + Unpin,
    send: &mut (impl AsyncWrite + Unpin),
    length: u64,
    sparse: bool,
) -> std::io::Result<()> {
    let mut piece = vec![0_u8; PIECE];
    let mut data: Vec<u8> = Vec::with_capacity(if sparse { MAX_DATA_SEGMENT } else { 0 });
    let mut zeros = 0_u64;
    let mut sent = 0_u64;
    while sent < length {
        let wanted = PIECE.min((length - sent) as usize);
        source.read_exact(&mut piece[..wanted]).await?;
        let bytes = &piece[..wanted];
        sent += wanted as u64;
        if !sparse {
            put(send, bytes).await?;
            continue;
        }
        if bytes.iter().all(|byte| *byte == 0) {
            flush_data(send, &mut data).await?;
            zeros += wanted as u64;
            continue;
        }
        flush_zeros(send, &mut zeros).await?;
        data.extend_from_slice(bytes);
        if data.len() >= MAX_DATA_SEGMENT {
            flush_data(send, &mut data).await?;
        }
    }
    flush_zeros(send, &mut zeros).await?;
    flush_data(send, &mut data).await
}

async fn flush_zeros(send: &mut (impl AsyncWrite + Unpin), zeros: &mut u64) -> std::io::Result<()> {
    if *zeros > 0 {
        put(send, &header(ZEROS, *zeros)).await?;
        *zeros = 0;
    }
    Ok(())
}

async fn flush_data(send: &mut (impl AsyncWrite + Unpin), data: &mut Vec<u8>) -> std::io::Result<()> {
    if !data.is_empty() {
        put(send, &header(DATA, data.len() as u64)).await?;
        put(send, data).await?;
        data.clear();
    }
    Ok(())
}

fn header(kind: u8, length: u64) -> [u8; 9] {
    let mut bytes = [0_u8; 9];
    bytes[0] = kind;
    bytes[1..].copy_from_slice(&length.to_be_bytes());
    bytes
}

async fn put(send: &mut (impl AsyncWrite + Unpin), bytes: &[u8]) -> std::io::Result<()> {
    #[cfg(test)]
    super::server::test_link::pace(bytes.len()).await;
    send.write_all(bytes).await
}

/// Reads a range body, plain or sparse, as the plain bytes it stands for.
pub struct RangeBody<R> {
    recv: R,
    sparse: bool,
    /// Bytes of the range not yet returned.
    remaining: u64,
    zeros: u64,
    data: u64,
}

impl<R: AsyncRead + Unpin> RangeBody<R> {
    pub fn new(recv: R, length: u64, sparse: bool) -> Self {
        Self {
            recv,
            sparse,
            remaining: length,
            zeros: 0,
            data: 0,
        }
    }

    /// Fills some of `out`; `Ok(0)` only at the end of the range.
    pub async fn read(&mut self, out: &mut [u8]) -> std::io::Result<usize> {
        if self.remaining == 0 || out.is_empty() {
            return Ok(0);
        }
        let limit = out.len().min(self.remaining as usize);
        let read = if !self.sparse {
            self.recv.read(&mut out[..limit]).await?
        } else {
            if self.zeros == 0 && self.data == 0 {
                self.next_segment().await?;
            }
            if self.zeros > 0 {
                let count = limit.min(self.zeros as usize);
                out[..count].fill(0);
                self.zeros -= count as u64;
                count
            } else {
                let count = limit.min(self.data as usize);
                let read = self.recv.read(&mut out[..count]).await?;
                self.data -= read as u64;
                read
            }
        };
        if read == 0 {
            return Err(std::io::ErrorKind::UnexpectedEof.into());
        }
        self.remaining -= read as u64;
        Ok(read)
    }

    pub async fn read_exact(&mut self, out: &mut [u8]) -> std::io::Result<()> {
        let mut filled = 0;
        while filled < out.len() {
            match self.read(&mut out[filled..]).await? {
                0 => return Err(std::io::ErrorKind::UnexpectedEof.into()),
                read => filled += read,
            }
        }
        Ok(())
    }

    async fn next_segment(&mut self) -> std::io::Result<()> {
        let mut header = [0_u8; 9];
        self.recv.read_exact(&mut header).await?;
        let length = u64::from_be_bytes(header[1..].try_into().expect("eight bytes"));
        if length == 0 || length > self.remaining {
            return Err(invalid("a sparse segment is empty or overruns its range"));
        }
        match header[0] {
            ZEROS => self.zeros = length,
            DATA => self.data = length,
            _ => return Err(invalid("unknown sparse segment kind")),
        }
        Ok(())
    }
}

fn invalid(message: &'static str) -> std::io::Error {
    std::io::Error::new(std::io::ErrorKind::InvalidData, message)
}

#[cfg(test)]
mod tests {
    use super::*;

    async fn round_trip(source: &[u8], sparse: bool) -> (Vec<u8>, usize) {
        let mut wire = Vec::new();
        send_body(source, &mut wire, source.len() as u64, sparse)
            .await
            .unwrap();
        let carried = wire.len();
        let mut body = RangeBody::new(wire.as_slice(), source.len() as u64, sparse);
        let mut out = vec![7_u8; source.len()];
        // Odd read sizes cross segment edges.
        let mut filled = 0;
        while filled < out.len() {
            let end = (filled + 50_000).min(out.len());
            filled += body.read(&mut out[filled..end]).await.unwrap();
        }
        assert_eq!(body.read(&mut [0; 4]).await.unwrap(), 0);
        (out, carried)
    }

    #[tokio::test]
    async fn zero_runs_cost_almost_nothing_and_arrive_intact() {
        let mut clip = vec![0_u8; 3 * 1024 * 1024 + 123];
        for (index, byte) in clip.iter_mut().enumerate().take(200_000) {
            *byte = (index % 251) as u8 + 1;
        }
        clip[2_000_000..2_100_000].fill(9);
        let (plain, plain_carried) = round_trip(&clip, false).await;
        let (sparse, sparse_carried) = round_trip(&clip, true).await;
        assert_eq!(plain, clip);
        assert_eq!(sparse, clip);
        assert_eq!(plain_carried, clip.len());
        // 300 KB of data in 64 KiB pieces, instead of 3 MB.
        assert!(sparse_carried < 500_000, "carried {sparse_carried} bytes");
    }

    #[tokio::test]
    async fn malformed_segments_are_rejected() {
        let mut wire = header(ZEROS, 10).to_vec();
        let mut body = RangeBody::new(wire.as_slice(), 5, true);
        assert!(body.read(&mut [0; 5]).await.is_err(), "overruns the range");
        wire = vec![9, 0, 0, 0, 0, 0, 0, 0, 1];
        let mut body = RangeBody::new(wire.as_slice(), 1, true);
        assert!(body.read(&mut [0; 1]).await.is_err(), "unknown kind");
    }
}
