//! Friend-to-friend clip sharing over iroh; see ADR 0011.
mod core;
mod download;
mod invite;
mod model;
mod node;
mod range_body;
mod presence;
mod server;
mod service;
mod store;
mod streams;
mod transfers;
mod wire;

pub use core::{ShareCue, SharingEvents};
pub use model::{FriendStatus, SharingSnapshot};
pub use invite::invite_argument;
pub use node::Network;
pub use service::{is_friend_id, ClipLibrary, ShareSource, SharingService};
pub use wire::valid_share_id;
