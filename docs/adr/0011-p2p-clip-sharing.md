# ADR 0011: Friend-to-friend clip sharing over iroh

- Status: Implemented; opt-in, off by default
- Date: 2026-09-30
- Scope: New `sharing` domain, one media route, an additive `SharingApi`
  client and a Friends tab. No capture, engine, encoder or settings changes.

## Context

Users want to send clips to friends and watch a friend's clip without first
downloading it, with no Clipture-operated server storing clips. Hand-written
peer-to-peer networking (NAT traversal, transport encryption, peer
authentication) is a security risk, so it must come from a maintained library.

## Decision

Use [iroh](https://github.com/n0-computer/iroh) 1.x (MIT/Apache-2.0) in the
resident Rust controller. iroh gives QUIC connections authenticated by Ed25519
public keys over TLS 1.3, hole punching, and relay fallback. A peer's public key
is its friend code; `Connection::remote_id()` is cryptographically bound to the
handshake and is the only identity the protocol trusts. BLAKE3 (`blake3` crate)
verifies whole-file integrity for kept copies. We do not use `iroh-blobs`
(pre-1.0, adds a database) because one bounded request/response ALPN
(`clipture/share/1`) is enough.

The node starts only after the user turns sharing on, and it lives in the
controller. Friend requests, offer delivery and "Add to library" downloads
therefore continue with no WebView (ADR 0002/0004).

### Protocol (`src-tauri/src/sharing/wire.rs`)

One bidirectional stream per request, carrying a u32-length-prefixed JSON
message (at most 16 KiB, unknown types rejected). Messages: `hello` (request,
accept, rename), `offer` (clip metadata and its BLAKE3), `answer` (the
recipient accepts or declines an offer), `range` (bytes of an offered clip,
with an optional `purpose` of `watch` or `keep`), `presence`
(online/heartbeat/leaving), `goodbye` (unfriend).

### Authorization

- Anyone who is not a friend gets exactly one request per connection, and only
  `hello` has an effect. At most 32 unanswered requests are kept. A declined
  request is blocked. Ignored hellos still answer `ok`, so strangers learn nothing.
- `offer` requires an accepted friend. `range` requires an accepted friend *and*
  a share offered to that exact friend. Removing a friend or revoking a share
  stops reads immediately.
- The sender serves bytes by opaque 128-bit share id. Local paths never leave
  the machine. A clip whose size changed since sharing is refused.
- Every limit is bounded: 32 connections, 8 concurrent streams per friend,
  read/idle timeouts, 16 GiB maximum clip size, and cleaned and length-limited
  text fields. No-op peer requests do not write to disk.

### Presence and appearing offline

Friends know who is online without polling everyone. On start, a visible node
sends `presence { online: true }` to every accepted friend (16 at a time). Any
answer, or any request from a friend, marks that friend online. A newly online
friend immediately receives everything waiting for them: our acceptance and
undelivered clip offers. Online friends heartbeat every 60 s, and silence for
150 s means offline (crash or lost network). A clean exit or disable sends
`presence { online: false }` within a 1.5 s budget. Presence is runtime-only and
never persisted. Friend requests to people who have not accepted us cannot use
presence, so they retry on the heartbeat.

"Appear offline" restarts the node without publishing its address (resolvers
only, no pkarr publisher) and refuses every incoming connection in
`on_accepting`, before the TLS handshake completes. Nothing is received, and no
presence is sent. Senders keep what they wanted to deliver (offers stay
`delivered: false`) and send it the moment we announce ourselves again.
Deliberate outgoing actions (watching a clip already in the inbox, removing a
friend) still work, so they reveal us to that one friend. Relays and resolvers
may still observe that the device is connected.

Presence evidence is timestamped by when a request *started*: an answer that
raced a goodbye cannot revive a departed friend, and a slow failed dial cannot
mark offline a friend who announced themselves meanwhile.

The client connection cache is never locked while dialing. One unreachable
friend (up to a 20 s dial) cannot stall requests to others, including stream reads.

### Removing a friend who is offline

`goodbye` used to be sent once and forgotten, so a friend removed while
offline kept us on their list. Removal is now settled from both ends:

- **Owed goodbyes:** removing an accepted friend (or one we asked) records
  them in `state.json` (`goodbyes`). The goodbye is retried on start, on
  every delivery pass and on the heartbeat, and is cleared once they hear it.
- **"Not friends" answers:** a peer not on our list at all that makes a
  request only a friend would make (`presence` online, `offer`, `answer`,
  `range`) gets `Response::NotFriends`. So does a `hello` from someone we
  still owe a goodbye: that is their old acceptance being retried, not a new
  request. Replying settles the goodbye.
- **On the other side:** when a friend answers `NotFriends` to our presence,
  offers or hello, we drop them without blocking (`forget_unfriended`).
  Pending requests and declines are never touched, so this cannot reveal a
  declined request. A `NotFriends` reply does not count as the friend coming
  online, so no deliveries start toward someone being dropped.

Strangers who were never friends learn nothing new: friend requests are
still answered `ok`. Older peers do not parse `NotFriends` and see a failed
request, as before; they still get the owed goodbye.

### Invite links

The friend code is the full 32-byte public key (52 z-base-32 characters) and
cannot be shortened without a lookup service. Invites make it easy to pass on
instead:

- **Clickable link:** `https://clipture.app/invite/#c=<code>&n=<name>`.
  Chat apps only make `https` clickable. `web/` is a static site served by
  Cloudflare Pages (free tier), built from this repository with no build step.
  The page makes no requests, has a strict CSP (`web/_headers`), and reads the
  code from the fragment, which browsers never send to the host. It hands off
  to the app link below.
- **App link:** `clipture://add/<code>?name=<name>`. It is registered by the
  installer's existing deep-link block (`plugins.deep-link.desktop.schemes` in
  `tauri.conf.json`) and arrives as a launch argument through the
  single-instance handler. The runtime handoff (ADR 0009) forwards one
  `clipture:` argument of at most 512 bytes verbatim to the runtime and to
  recovery.
- **Paste:** Add a friend accepts either link or a bare code.

A link can come from anyone, so it never adds a friend by itself. It fills one
pending-invite slot, and the UI asks for confirmation ("Not now" is the default
focus), labelling the name as the link's claim and showing part of the code.
Confirming turns sharing on if needed and sends a normal friend request, which
the other side still has to see before any clips flow. Links to yourself are
ignored, and parsing accepts only `add` links with a valid key.

Anything queued while the node was starting is delivered once the node is online,
not at the next heartbeat.

### Asking first, and what the sender sees

An offer no longer lands in the inbox as a playable clip. It arrives with
`awaitingAnswer` set, the receiver's controller plays a cue, and the Friends
badge counts it next to friend requests. Nothing streams or downloads until
the user accepts: `open_stream` and `save_shared_clip` refuse a clip that is
still waiting. Accepting or declining queues an `answer` in `state.json`
(`answers`), delivered like offers (now, or when the sender is next seen).
A declined clip leaves the inbox; dismissing a waiting clip declines it.
Re-sending a clip the friend already accepted is answered again
automatically, so the sender is never left waiting on a duplicate.

The sender keeps the answer on the share (`answer`: pending, accepted,
declined). A declined share stops serving ranges at once. Sharing the same
clip again resets the share to pending and asks again.

Progress is counted on the sender as bytes leave (`transfers.rs`), so it
needs nothing extra from the receiver and works with any reader. Each share
records the union of byte spans sent by any stream, a recent rate, and
whether the friend is watching or keeping (`purpose` on `range`; `keep` wins
once seen). A stream that ends because the friend stopped reading pauses the
transfer. A stream that fails because the whole connection closed while
bytes were still going out marks it `interrupted`, which clears when they
reconnect. Counts are runtime only; the one fact kept across restarts is
`receivedWhole`, set once every byte has been read.

A share closes once the friend has kept it. After their "Add to library"
copy passes its BLAKE3 check, the receiver sends `answer` again with
`kept: true`. The sender marks the share `kept`, refuses every later range
(watching or keeping), deletes any stream copy an older build made for it,
and shows the check mark.
The check mark means a confirmed copy, not just bytes sent: a copy that
failed its check can still be fetched again. Sharing the clip again on
purpose reopens it and asks the friend again. An older receiver never
confirms, so its shares stay open as before.

Access is also limited in time. A friend has 15 minutes (`SHARE_WINDOW_MS`)
from accepting to watch the clip or start keeping it; the sender records
`acceptedAtMs` and refuses every read that starts later (`admit_read`). A
download that began inside the window (`keepStarted`) may finish and resume
after it, so a large clip is never cut off near the end; watching may not.
Older friends that never answer start the window with their first read, and
shares from before the window count from when they were sent. Both sides
show `availableUntilMs`; the sender's clock decides. Sharing again opens a
new window.

Cues are synthesized in the controller (`sounds/cues.rs`): incoming,
accepted, declined, complete (keep only) and interrupted. They play with no
WebView, never queue, and are silent in test mode.

Compatibility: fields added to `range` and the store default when absent.
Records written before this change read as accepted. An older sender rejects
the unknown `answer` type; the receiver drops an answer that a reachable
friend refused rather than retrying it forever, so the sender simply shows
"waiting". An older receiver never answers, so the sender treats the first
range it reads as acceptance.

### Streaming without saving

`sharing_stream_url` creates an owner-bound, 192-bit opaque session. The
existing media protocol serves `/v1/remote/<id>/video` through the narrow
`media::RemoteMediaSource` trait, so `media` stays unaware of peers.
Ranges are fetched in 1 MiB blocks by runways (see "Streaming runway"). They
are kept in a temporary file in the system temp folder, deleted with the
session and swept at start; nothing reaches the library until the viewer
keeps the clip. Releasing the player (or closing the UI) releases the
sessions like other playback sessions.

### Keeping a copy

"Add to library" streams the whole file to a temporary file in
`<saveFolder>\Shared`. It hashes while writing, checks the size, the ISO-BMFF
`ftyp` signature and the offer's BLAKE3, and only then publishes to a unique,
non-clobbering name. It then appends a normal clip record (`folderName`
"From <friend>"). A mismatch deletes the temporary file.

### Persistence (ADR 0003 compatible)

`%APPDATA%\Clipture\data\sharing\identity.key` holds the 32-byte device secret
key, protected by the user profile ACL. `state.json` (friends, blocks, outbox,
inbox) is written atomically. A corrupt state file is set aside, not overwritten.

### Transfer speed and local networks

Transfers are bounded only by the two PCs' connections. QUIC flow-control
windows are raised to 64 MiB per stream and 256 MiB per connection, so the
window never limits throughput (window / RTT is about 2.5 Gbit/s at 200 ms).
Senders read clips in 1 MiB pieces.

"Add to library" is resumable, as in DashBeam. Received bytes go to
`.clipture-incoming-<share id>.part` in the shared clips folder. A dropped
connection reconnects with backoff (1 s to 30 s, reset by progress) and asks
for the remaining range only; "Try adding again", even after a restart,
re-hashes the partial and continues. The whole-file BLAKE3 check still
decides: a mismatch deletes the partial so a retry starts clean. Cancelling
or dismissing deletes it too. We use plain range requests rather than
iroh-blobs: the wire protocol and serving code stay unchanged, and BLAKE3
over the finished file gives the same guarantee for one-file transfers.

Like LocalSend, Clipture finds friends on the same network with mDNS
(`iroh-mdns-address-lookup`, service `clipture`), so they connect over the LAN
without the internet or a relay. Only the device key and addresses are
announced, and only while visible; appearing offline listens without
announcing. A connection starts on a relay and moves to a direct path once
hole punching succeeds; when both NATs prevent that, it stays on the
rate-limited public relay, and "Add to library" shows "via relay".

### Streaming runway

Streaming follows the torrent "runway" model: sequential fetches run ahead
of the player in order, each on its own QUIC stream, at full link speed, into
a temporary file per session (three sessions at most). Players read
from several places at once (Clipture stores audio apart from video, and the
MP4 index sits at the end), so up to four runways run side by side: a read
just ahead of one waits for it, a read anywhere else starts another, and the
least recently needed one gives way. A runway that reaches another merges
into it; one that reaches the end fills the remaining gaps. A single runway
thrashed between video and audio and fetched only slivers.

The link is shared by priority ("background yields to foreground"): a
runway less than 32 MiB ahead of what the player last read through it is
urgent, and while any runway is urgent the others wait, so the watched
position gets the whole upload. Runways fetch in 8 MiB requests rather than
one request to the end, so waiting actually stops the sender instead of
leaving a 64 MiB flow window in flight. Equal sharing between four unbounded
runways left the watched one below the bitrate of high-quality clips. Replies carry up
to 3 MiB of contiguous cached bytes, under the UI process pipe's 4 MiB frame
limit (a larger frame closes the window process). Nothing streams until the
viewer presses play. Blocks a kept clip or an "Add to library"
partial already holds are read from disk, never fetched twice. The stream
bar draws the host's own record of streamed ranges (`streamed` on each inbox
clip), not the browser's buffered estimate.

Clipture records each audio source as its own track, and a video element
plays only the first. Streams therefore use Clipture's player (the
`MediaPlayer` from the player feature): once every block of the session's
temporary file (`clipture-stream-*.mp4`) has arrived, `/v1/remote/<id>/audio`
mixes every track from it with the same FFmpeg mixer as the library.

Shared clips are sent as a lossless stream copy when they would stream badly
(padding over about 12%, or a track stored out of time order, as in-place
recordings are): FFmpeg remuxes them in time order with the index first into
`<save folder>/.clipture-sharing`, and the copy is verified to hold the same
video samples. The benchmark below measured the effect: a 121 MB clip became
61 MB and started in 0.7 s instead of 1.6 s without its stall; a 719 MB
50 Mbit/s clip started in 1.6 s instead of 8.8 s. Copies are deleted when
their share ends, are capped at 10 GB together (oldest shares end first) and
expire after 30 days.

Since 1.6.5 no copy is written. Such a clip is offered through its linear
view (`media/linear_view.rs`): the original `ftyp`, the original `moov` with
only its chunk offset tables rewritten, an `mdat` header, then every chunk in
playing order, gathered from the clip as ranges are requested. The offer's
size and BLAKE3 digest are the view's, so receivers verify exactly what was
sent, and the file they keep is a normal, time-ordered MP4 with no padding.
Sending no longer waits for a remux or uses extra disk; the share records
`linear: true`. Shares from older builds keep their copies, which are still
tidied as above. The library player serves such clips through the same view.

"Fix clips" in Settings also offers padded clips,
which can halve their size on disk. "Add to library" labels
every audio track found in the downloaded file's `moov`, not only the
sender's list, so the library player never silences a track.

### Sparse ranges and the streaming benchmark

Clipture's in-place recordings can be about half zero padding (48% of a
measured 2-minute clip). A `Range` request carries `sparse: true`; a sender
that understands it answers `SparseRange` and elides zero runs (see
`range_body.rs`), so padding costs a few bytes instead of upload time. Older
senders ignore the field and send plain bytes, and older receivers never ask,
so mixed versions keep working. Kept clips are still verified whole-file.

`service_bench.rs` (ignored by default) measures streaming end to end: two
real nodes over QUIC, the sender's upload throttled (for example 56 Mbit/s),
and the receiver's stream played by headless Edge, which reports first-frame
time and stalls. On 56 Mbit/s: a 50 Mbit/s 1440p clip starts in about 9 s and
then plays without stalls; an 8.5 Mbit/s clip starts in about 1.6 s with one
short early stall, because in-place recordings are not stored in time order.
Fetching in playback order from the MP4 index is the next step.

## Consequences and trade-offs

- **Not zero infrastructure.** With the `N0` preset, peers publish signed
  address records to n0's DNS/pkarr service, and connect through n0's public
  relays until hole punching succeeds (or permanently, behind strict NATs).
  Relays see only ciphertext and endpoint ids, but they do learn IP addresses
  and connection timing. This is disclosed on the opt-in screen. A self-hosted
  `iroh-relay` can replace n0's later through `RelayMode::Custom`.
- **Both peers must be online.** There is no store-and-forward. Offers and
  acceptances go out when a friend is next seen online; requests to people who
  have not added us yet retry on the 60 s heartbeat.
- The sender's upload bandwidth bounds playback quality.
- Test profiles (`CLIPTURE_TEST_MODE`) use `Network::Disabled` and never open
  sockets. Unit tests use loopback endpoints with an in-memory address book.

## Verification

- `cargo test --lib sharing`: store policy, wire bounds, identity and codes,
  stream cache bounds, plus three end-to-end tests between real loopback
  endpoints. These cover befriend, offer, owner-bound streaming, a verified keep,
  stranger denial, revoke, and a tampered same-size file rejected by BLAKE3 with
  no leftovers.
- `media::protocol` test for the remote route.
- `node scripts/migration/test-sharing-contract.cjs` (run by the host contract):
  `SharingApi`, adapters, command registry, UI-worker allowlist and event relay agree.
