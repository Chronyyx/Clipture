//! The iroh endpoint and client-side requests. Peers are addressed only by
//! public key; iroh authenticates the key during the TLS 1.3 handshake, so a
//! connection's `remote_id` cannot be forged. Relays forward only ciphertext.
use std::{
    collections::HashMap,
    fs,
    io::Write,
    path::Path,
    sync::Arc,
    time::{Duration, Instant},
};

use iroh::{
    address_lookup,
    endpoint::{presets, Connection, QuicTransportConfig, RecvStream, VarInt},
    protocol::Router,
    Endpoint, EndpointId, SecretKey,
};
use tokio::{sync::Mutex, time::timeout};

use crate::error::{AppError, AppResult};

use super::{
    range_body::RangeBody,
    server::{PeerContext, ShareProtocol},
    wire::{self, Request, Response, ALPN},
};

/// mDNS service name; only Clipture devices answer to it.
const LAN_SERVICE: &str = "clipture";
const CONNECT_TIMEOUT: Duration = Duration::from_secs(20);
const REQUEST_TIMEOUT: Duration = Duration::from_secs(20);
/// Flow-control windows, sized so they never limit a transfer: throughput
/// is capped at window / round-trip time, and 64 MiB allows about 2.5 Gbit/s
/// even at 200 ms. The QUIC default (1.25 MB) capped a clip near 100 Mbit/s.
/// Memory is used only for data received but not yet read, and transfers
/// are read as fast as they arrive.
const STREAM_WINDOW: u32 = 64 * 1024 * 1024;
const CONNECTION_WINDOW: u32 = 256 * 1024 * 1024;

fn transport() -> QuicTransportConfig {
    QuicTransportConfig::builder()
        .stream_receive_window(VarInt::from_u32(STREAM_WINDOW))
        .receive_window(VarInt::from_u32(CONNECTION_WINDOW))
        .send_window(u64::from(CONNECTION_WINDOW))
        .build()
}

/// How the endpoint reaches peers.
#[derive(Clone)]
pub enum Network {
    /// n0 relays for NAT traversal/fallback plus DNS/pkarr address lookup.
    Internet,
    /// Test profiles never touch the network.
    Disabled,
    /// Loopback only, addresses shared in memory (tests).
    #[cfg(test)]
    Local(iroh::address_lookup::MemoryLookup),
}

pub struct Node {
    router: Router,
    context: Arc<PeerContext>,
    connections: Mutex<HashMap<EndpointId, Connection>>,
}

impl Node {
    /// `visible == false` is "appear offline": the address is not published
    /// for lookup and every incoming connection is refused. Outgoing requests
    /// still work, because the user chose to make them.
    pub async fn start(
        context: Arc<PeerContext>,
        secret: SecretKey,
        network: Network,
        visible: bool,
    ) -> AppResult<Self> {
        let builder = match &network {
            Network::Internet if visible => Endpoint::builder(presets::N0),
            Network::Internet => Endpoint::builder(presets::Minimal)
                .address_lookup(address_lookup::PkarrResolver::n0_dns())
                .address_lookup(address_lookup::DnsAddressLookup::n0_dns())
                .relay_mode(iroh::endpoint::default_relay_mode()),
            Network::Disabled => {
                return Err(AppError::Path("sharing is disabled in test mode".into()))
            }
            #[cfg(test)]
            Network::Local(lookup) => Endpoint::builder(presets::Minimal)
                .relay_mode(iroh::RelayMode::Disabled)
                .address_lookup(lookup.clone()),
        };
        let endpoint = builder
            .transport_config(transport())
            .secret_key(secret)
            .alpns(vec![ALPN.to_vec()])
            .bind()
            .await
            .map_err(|error| AppError::Integration(format!("could not start sharing: {error}")))?;
        if matches!(network, Network::Internet) {
            add_local_network_lookup(&endpoint, visible);
        }
        #[cfg(test)]
        if let Network::Local(lookup) = &network {
            if visible {
                lookup.add_endpoint_info(endpoint.addr());
            } else {
                lookup.remove_endpoint_info(endpoint.id());
            }
        }
        let router = Router::builder(endpoint)
            .accept(ALPN, ShareProtocol::new(context.clone(), visible))
            .spawn();
        Ok(Self {
            router,
            context,
            connections: Mutex::new(HashMap::new()),
        })
    }

    /// An answer proves a friend is online; an unreachable friend is not.
    fn note_outcome(&self, peer: EndpointId, reached: bool, started: Instant) {
        let id = friend_code(&peer);
        if reached {
            self.context.contact_if_friend_since(&id, started);
        } else if self.context.presence.unreachable_since(&id, started) {
            self.context.core.events.changed();
        }
    }

    /// Whether traffic to `peer` currently goes through a relay (slower)
    /// rather than directly; `None` without a live connection.
    pub async fn relayed(&self, peer: EndpointId) -> Option<bool> {
        let connection = self.connections.lock().await.get(&peer)?.clone();
        let paths = connection.paths();
        let selected = paths.iter().find(|path| path.is_selected());
        selected.map(|path| path.is_relay())
    }

    pub async fn shutdown(&self) {
        self.connections.lock().await.clear();
        let _ = self.router.shutdown().await;
    }

    /// Reuses a live connection or dials a new one. The cache lock is never
    /// held while dialing: an unreachable friend must not stall requests to
    /// everyone else (including stream reads for a clip being watched).
    async fn connection(&self, peer: EndpointId) -> AppResult<Connection> {
        if let Some(existing) = self.connections.lock().await.get(&peer) {
            if existing.close_reason().is_none() {
                return Ok(existing.clone());
            }
        }
        let connection = timeout(CONNECT_TIMEOUT, self.router.endpoint().connect(peer, ALPN))
            .await
            .map_err(|_| offline())?
            .map_err(|error| {
                tracing::debug!(%error, "peer connection failed");
                offline()
            })?;
        self.connections
            .lock()
            .await
            .insert(peer, connection.clone());
        Ok(connection)
    }

    /// Sends one request and returns its response plus the stream, which
    /// carries raw bytes after a `Range` response.
    pub async fn request(
        &self,
        peer: EndpointId,
        request: &Request,
    ) -> AppResult<(Response, RecvStream)> {
        let started = Instant::now();
        let connection = match self.connection(peer).await {
            Ok(connection) => connection,
            Err(error) => {
                self.note_outcome(peer, false, started);
                return Err(error);
            }
        };
        let exchange = async {
            let (mut send, mut recv) = connection.open_bi().await.map_err(peer_error)?;
            wire::write_message(&mut send, request)
                .await
                .map_err(peer_error)?;
            send.finish().map_err(peer_error)?;
            let response: Response = wire::read_message(&mut recv).await.map_err(peer_error)?;
            Ok::<_, AppError>((response, recv))
        };
        let result = match timeout(REQUEST_TIMEOUT, exchange).await {
            Ok(Ok(result)) => Ok(result),
            Ok(Err(error)) => {
                // A broken connection is re-dialed on the next request.
                self.connections.lock().await.remove(&peer);
                Err(error)
            }
            Err(_) => Err(offline()),
        };
        self.note_outcome(peer, result.is_ok(), started);
        result
    }

    /// Sends a request that expects a plain acknowledgement.
    pub async fn notify(&self, peer: EndpointId, request: &Request) -> AppResult<()> {
        match self.request(peer, request).await?.0 {
            Response::Ok {} => Ok(()),
            Response::Denied { reason } => Err(AppError::Path(format!(
                "your friend's Clipture declined: {}",
                wire::clean_text(&reason, 120)
            ))),
            Response::Range { .. } | Response::SparseRange { .. } => {
                Err(AppError::Path("unexpected peer response".into()))
            }
        }
    }

    /// Opens a byte range of a clip that was offered to us.
    pub async fn open_range(
        &self,
        peer: EndpointId,
        share_id: &str,
        start: u64,
        length: u64,
    ) -> AppResult<(u64, RangeBody<RecvStream>)> {
        let request = Request::Range {
            share_id: share_id.into(),
            start,
            length,
            sparse: true,
        };
        // Older senders ignore `sparse` and answer with plain bytes.
        match self.request(peer, &request).await? {
            (Response::Range { total, length: sent }, recv) if sent == length => {
                Ok((total, RangeBody::new(recv, length, false)))
            }
            (Response::SparseRange { total, length: sent }, recv) if sent == length => {
                Ok((total, RangeBody::new(recv, length, true)))
            }
            (Response::Denied { reason }, _) => Err(AppError::Path(format!(
                "{UNAVAILABLE}: {}",
                wire::clean_text(&reason, 120)
            ))),
            _ => Err(AppError::Path("unexpected peer response".into())),
        }
    }
}

/// Finds friends on the same network (like LocalSend does), so they connect
/// over the LAN at full local speed even without internet or a relay.
/// Only this device's key and addresses are announced, and only while
/// visible; appearing offline listens without announcing. Best effort: a
/// network that blocks multicast just falls back to internet lookup.
fn add_local_network_lookup(endpoint: &Endpoint, visible: bool) {
    let lookup = iroh_mdns_address_lookup::MdnsAddressLookup::builder()
        .service_name(LAN_SERVICE)
        .advertise(visible)
        .build(endpoint.id());
    match (lookup, endpoint.address_lookup()) {
        (Ok(lookup), Ok(services)) => services.add(lookup),
        (Err(error), _) => tracing::debug!(%error, "local network lookup unavailable"),
        (_, Err(error)) => tracing::debug!(%error, "address lookup unavailable"),
    }
}

const UNAVAILABLE: &str = "this clip is no longer available";

/// The friend refused the range (unshared, moved or edited); retrying
/// cannot help, unlike a dropped or unreachable connection.
pub fn is_unavailable(error: &AppError) -> bool {
    matches!(error, AppError::Path(message) if message.starts_with(UNAVAILABLE))
}

fn offline() -> AppError {
    AppError::Path("your friend is offline or unreachable right now".into())
}

fn peer_error(error: impl std::fmt::Display) -> AppError {
    tracing::debug!(%error, "peer request failed");
    AppError::Path("the connection to your friend was interrupted".into())
}

/// Loads the device identity, creating it on first use. The key never leaves
/// this machine; only its public half is shown as the friend code.
pub fn load_or_create_identity(directory: &Path) -> AppResult<SecretKey> {
    let path = directory.join("identity.key");
    match fs::read(&path) {
        Ok(bytes) => {
            let bytes: [u8; 32] = bytes
                .as_slice()
                .try_into()
                .map_err(|_| AppError::Path("the sharing identity file is damaged".into()))?;
            return Ok(SecretKey::from_bytes(&bytes));
        }
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
        Err(source) => {
            return Err(AppError::Io {
                action: "read sharing identity",
                path,
                source,
            })
        }
    }
    fs::create_dir_all(directory).map_err(|source| AppError::Io {
        action: "create sharing directory",
        path: directory.to_owned(),
        source,
    })?;
    let secret = SecretKey::generate();
    let mut file = fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&path)
        .map_err(|source| AppError::Io {
            action: "create sharing identity",
            path: path.clone(),
            source,
        })?;
    file.write_all(&secret.to_bytes())
        .and_then(|_| file.sync_all())
        .map_err(|source| AppError::Io {
            action: "write sharing identity",
            path,
            source,
        })?;
    Ok(secret)
}

/// Accepts a friend code with or without the `clipture:` prefix.
pub fn parse_friend_code(code: &str) -> AppResult<EndpointId> {
    let trimmed = code.trim();
    let raw = trimmed.strip_prefix("clipture:").unwrap_or(trimmed).trim();
    if raw.is_empty() || raw.len() > 80 {
        return Err(AppError::Path("that friend code is not valid".into()));
    }
    EndpointId::from_z32(raw)
        .or_else(|_| raw.parse::<EndpointId>())
        .map_err(|_| AppError::Path("that friend code is not valid".into()))
}

pub fn friend_code(id: &EndpointId) -> String {
    id.to_z32()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn identity_is_created_once_and_reloaded() {
        let directory = tempfile::tempdir().unwrap();
        let first = load_or_create_identity(directory.path()).unwrap();
        let second = load_or_create_identity(directory.path()).unwrap();
        assert_eq!(first.public(), second.public());
    }

    #[test]
    fn friend_codes_round_trip_and_reject_garbage() {
        let id = SecretKey::generate().public();
        let code = friend_code(&id);
        assert_eq!(parse_friend_code(&code).unwrap(), id);
        assert_eq!(
            parse_friend_code(&format!(" clipture:{code} ")).unwrap(),
            id
        );
        assert!(parse_friend_code("not a code").is_err());
        assert!(parse_friend_code("").is_err());
    }
}
