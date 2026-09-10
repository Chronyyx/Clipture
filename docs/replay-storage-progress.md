# Replay storage implementation checkpoint

## 2026-09-10: milestone 6 - audio, bounded storage and consume-on-save

Source implementation now exposes **Save in place**, default on for new and old
profiles unless explicitly disabled. Off restores the previous overlapping replay
window and hybrid RAM policy. Installed software and the daily profile have not
been replaced, restarted or edited by this work.

- Video and AAC spill workers share `replay/InPlacePacketArchive`: a private
  `.clipture-replay/*.recording` arena inside the configured save folder. Packet
  leases protect individual byte ranges; retired, unleased slots can be reused.
  A bitrate/window-derived hard arena cap falls back to existing segment storage
  when necessary. Capture callbacks never write these files directly.
- Save detaches the arena, allowing new persistence into another file. Existing
  video and normalized AAC offsets feed the original MP4 metadata builder. Small
  resident tails, decoder preroll from an earlier file, and repaired audio are
  appended when needed. The result is an ordinary independent MP4, not a reference
  movie. System/microphone track order avoids a host reorder-only remux.
- `ConsumedWindow` advances after engine save success, never after an engine save
  failure. Saving 120 seconds and then saving 10 seconds later presents only the
  new 10 seconds, even if decoder preroll predates the boundary. A later host
  postprocessing/publication failure does not undo an engine success; its source
  MP4 remains available. Mixing/resizing/multi-resolution stitching still uses
  existing processing and can require copies.
- Arena-backed video/AAC persistence is disk-first. PCM recovery storage remains
  unchanged. Pins keep sources alive through saving and subsequent decoder preroll.
  Unused owned arenas are deleted after their last lease; detached failed-save
  files are retained. There is no new crash-recovery journal.
- Expired free slots are zeroed when detaching, so free padding is not stale
  footage. This costs writes and the final MP4 can be larger than compact output.
  Selected media already in the file is not rewritten. Final flush may still wait
  for prior buffered disk writes; this is not an unconditional instant-save claim.
- Native error replies now escape Windows paths/quotes correctly rather than
  causing a protocol timeout. Rapid saves choose distinct numbered filenames.

Verification includes both native CTest suites, TypeScript checking, host and
engine contract checks, read-only artifact inventory, and Rust library tests
(123 passed, 3 ignored). The generated-media runner validates 30 MP4s: exact
120-second / 10-second video windows, two independently decoded AAC tracks,
audio gap repair, seeks, and 366 seconds of bounded rolling reuse. Native tests
also cover pinning, fallback, failed saves, opt-out and publication handle sharing.
Latest synthetic run: `.cache/mp4-ready-q7AU3s`. Isolated real DXGI capture passed
in `.cache/in-place-engine-AzACzd`: 5-second then 2-second in-place saves, an
intentional invalid-folder failure returning immediately, a 1-second recovery
save, then a full overlapping 5-second opt-out save. All four MP4s decode. The
last two saves occurred within one timestamp second and retained distinct paths.
The first two in-place saves wrote no media during mux; recovery copied 77,148
bytes from the preserved previous arena. Engine total save times were 33-35 ms
with zero reported dropped-frame delta, at 640x360/30fps/4 Mbps with audio disabled.
These small low-load numbers are not an end-to-end gaming or disk benchmark.

Remaining release checks: actual game-load performance, real microphone/system
audio playback and sync in the user workflow, and the supported-player matrix.
No ReFS support, power-loss recovery or end-to-end performance improvement is
claimed from the small fixtures. Historical milestones below describe their state
at the time; their video-only/default-off limitations are superseded by this entry.

## 2026-09-10: milestone 5 - in-place MP4 finalization proof

Per the user's new direction, in-place finalization is now the priority. The
separate clone backend remains available for future fallback experiments; no
ReFS volume or production clone integration is required for this proof.

Implemented an isolated append-only video buffer. Before Save it writes prepared
H.264 samples into one exclusively created `.recording` file, leaving 4096 bytes
for the MP4 prefix. Save freezes the file, reuses the existing muxer's movie
metadata/timing builder with the original sample offsets, appends `moov`, patches
`ftyp`/`free`/64-bit `mdat`, flushes, and renames the same file to `.mp4`. It never
rewrites the media range. Extension change alone is not enough before finalization.

Ownership is split across `replay/InPlaceVideoBuffer`, `mux/InPlaceMp4Header`, and
`platform/windows/InPlaceFile`; the muxer has a small opt-in dispatch branch.
`MuxWritePacing::experimentalInPlace` is a synchronous internal test hook only.
There is no setting, protocol change or live-engine caller. Existing saves and
installed software are untouched. No user testing is needed at this checkpoint.

### Verified

- Release engine and both native test executables build successfully; both CTest
  suites pass. The existing nonfatal `pwsh.exe` lookup warning remains.
- Native ownership tests: exclusive creation, frozen-file gating, epoch/dimension
  rejection, foreign extent rejection, repeat-finalize refusal, rename collision
  protection and retry, same file identity, readable pins after rename/shutdown,
  unfinished-media retention and 64-bit MP4 size encoding.
- Synthetic media runner validates 24 MP4s across three trials, including three
  in-place files. Sample bytes and movie metadata match the compact reference
  except chunk offsets. All show exactly the expected 120 seconds / 3600 frames,
  starting 1.5 seconds into the source. Four seek positions match the reference.
- Each in-place file holds 5,574,211 media bytes. Save performs **zero logical
  media reads and zero media writes**, writing only 68,686 bytes of header/index.
  Native tests measure the entire mux call, including its prepass, and verify the
  file ID survives publication. This is not merely a renamed copy.

Retained final full run: `.cache/mp4-ready-KS9XmP/in-place-summary.json`.
These are logical I/O counts, not physical SSD-write or latency benchmarks.
`FlushFileBuffers` may still wait for media written earlier to reach the disk.
The in-place file is 4052 bytes larger than compact output because of its fixed
prefix reservation. No claim of instantaneous saves or lower production RAM.

### Explicit next gates

This is **not a bounded rolling 120-second recorder**. The proof accumulates an
entire 122-second source and uses the existing edit list for visible trimming;
hidden source bytes remain in the file. It has no old-region reuse, disk-space
reclamation, crash-recovery index, audio, live rotation or overlapping-save policy.
Unfinished/failed files are retained, but they are not automatically recoverable
MP4s. The fixture cannot run indefinitely as a production replay cache.

Next work: bounded storage/index lifetime and rollover; an explicit strategy for
preserving the preceding window while recording continues after a save; audio
extents with correct sync; journal/recovery and failure injection. A same-volume
rename consumes one private file: it cannot create two independent overlapping
clips for free. Overlap and unsupported cases need a deliberate copy/clone
fallback. Integrate capture-aware I/O pacing and test real playback before rollout.

## 2026-09-10: milestone 4 - standalone clone backend and safe retry

The experimental Windows backend is implemented and compiled, with separate
modules for open-handle capability queries, sealed source ownership, private
output operations and a host-neutral clone/copy job. It is **not enabled or
connected to the live replay mux**. Installed software and recording defaults
remain untouched.

Eligibility requires ReFS and block-refcounting support, matching GUID volume,
distinct file identity, known matching integrity/cluster properties, ordinary
file attributes, aligned ranges and an extended destination. Unknown/unsupported
cases copy instead. The native IOCTL loop uses aligned requests at most 1 GiB.
No sparse/integrity settings are changed to force eligibility.

Sources are held by sealed read handles, not reopened per range. Destinations
use exclusive `CREATE_NEW`. A clone-operation failure invalidates the entire
attempt: deletion uses its owned file handle, then a fresh output is created and
retried once with cloning disabled. Failed cleanup blocks retry. Read/write/flush
failure never reports success. Completed/copied/cloned/padding bytes are separate.

### Verified

- Release engine/test builds and both native CTest suites pass.
- Capability/range policy tests cover unsupported and cross-volume files,
  identity aliases, integrity mismatches, alignment, bounds and request limits.
- Simulated clone success, unsupported ranges, partial clone failures, and
  create/write/finish/cleanup failures exercise the job lifecycle.
- Real Windows scratch files verify source write exclusion, output collision
  protection, output independence after source edits/deletion, failed-output
  cleanup and exception unwinding.
- Fault injection corrupts a real private output after a simulated first clone;
  the native discard/recreate retry produces a fully byte-correct copy, including
  bytes outside the failed range. This is simulated cloning, not ReFS evidence.
- Full media run `.cache/mp4-ready-kAGb7N` validates 21 generated MP4s, including
  three additional native sealed-file copy outputs. Encoded bytes, expected
  120-second presentation/3,600 decoded frames, aligned offsets and seek checks
  retain prior parity. The native backend copies a completed synthetic MP4 here;
  it is not yet assembling MP4 payloads directly from live replay segments.

### Explicitly unverified / remaining work

Read-only volume inventory found only NTFS on the mounted C:/D:/E: volumes and
unnamed volumes. Native probes correctly report `refs=0`, `refcounting=0`,
`cloned=0`, and successful copy fallback. **Successful native ReFS cloning is not
verified**; tests print a skip, not a false success claim. No drive was formatted
or mounted. There are no measured copy savings or reduced RAM defaults.

Next integration must expose stable closed-segment native handles, retain the
existing capture-aware I/O pacing, coordinate MP4 headers/indexes with fresh-file
retry, and preserve audio/publication behavior. `SealedPayloadFile` intentionally
cannot attach to actively written segments. Real ReFS success, full A/V, playback
matrix, recovery and performance are release gates; NTFS-specific in-place arena
work remains a separate experiment.

See [backend ownership/contracts](../engine/src/platform/windows/README.md) and
[probe instructions](../engine/tests/replay/README.md). There is nothing to enable
or manually test in the installed app at this checkpoint.

## 2026-09-09: milestone 3 - alignment-aware MP4 region placement

Implemented an opt-in aligned placement policy and connected it to the existing
MP4 metadata builder/writer. The policy groups adjacent samples from a backing
range before choosing alignment, adds minimal zero-filled gaps between regions,
and preserves original sample sizes/bytes. Small regions without a useful aligned
body remain compact. Heads and tails never borrow unselected neighboring data.

The experimental mux uses a 64-bit `mdat` header from the outset and supplies
planned sample offsets to the existing `co64` builder. Timing, edit lists and
codec metadata are unchanged. Existing read/write pacing and diagnostics remain
on the sample-copy path. The portable extent-copy writer also handles gaps with
bounded scratch memory and separate media/padding accounting.

Scope is intentionally narrow: fully prepared disk video with no output audio
tracks. Legacy/mixed/resident video, opaque readers, invalid alignment and
audio-containing saves keep the compact path. Audio fallback has byte-parity and
track-retention tests, not a claim of new aligned A/V support. The option is a
default-zero internal C++ argument, not an installed-app setting or IPC change.

### Evidence

Release engine/test builds and both CTest suites passed. Combined compact/aligned
tests cover 19,968 small layouts plus overflow, large offsets, padding failures,
source lifetime and fallback cases. Generated-media run `.cache/mp4-ready-I0vivP`
produced 18 MP4s: twelve compact/reconstructed files with byte parity and six
aligned files with unchanged encoded samples, timing metadata and decoded frames.
Each presents the same expected 120 seconds (3,600 frames). Four seek positions
also match compact output for both tested alignments.

| Illustrative alignment | Media bytes | Candidate bytes | Padding bytes |
| --- | ---: | ---: | ---: |
| 4 KiB | 5,574,211 | 5,537,792 (99.3%) | 31,482 |
| 64 KiB | 5,574,211 | 4,980,736 (89.4%) | 477,946 |

All media bytes are still copied. These are **geometric candidates, not cloned
bytes or measured savings**. Padding increases writes until a useful clone backend
exists. The small fixture makes large-cluster overhead especially visible; do not
extrapolate to gameplay bitrate or select a cluster size instead of querying it.
FFmpeg decode/seek and a structural inspector passed; Windows/WebView2/editor
playback, full A/V, recovery and real filesystem cloning remain future gates.

Next milestone: a capability-gated Windows clone backend with scratch-file probes,
bounded operations, independent-file verification and safe copy fallback. No drive
formatting, cache relocation, installed-app restart, or production enablement was
performed. Module map: [mux](../engine/src/mux/README.md).

## 2026-09-09: milestone 2 - pinned extent planning and copy fallback

Implemented the next independent layer, without enabling a new production save
path or changing any recording defaults:

- A host-neutral pinned-source interface with explicit 64-bit byte ranges.
  Existing segment readers expose their immutable committed ranges; plans keep
  those files alive independently of packet snapshots and the recording store.
- A pure payload planner: validated bounds/overflow, stable per-sample output
  offsets, contiguous-range coalescing, and alignment-compatible body detection.
- A portable copy executor using caller-owned bounded scratch memory. Failed
  reads or writes stop execution; partial outputs are never reported as complete.

Verification: Release engine/test builds succeeded; both CTest suites passed.
The added tests exercise 9,984 alignment layouts, different/overlapping source
ranges, offsets above 4 GiB, invalid bounds, read/write/partial-write failures,
retry into a fresh output, and two overlapping saves retaining disk sources after
store destruction. Owned replay files were reclaimed after the final pin ended.

Real-media run `.cache/mp4-ready-lJjKLG` produced twelve byte-identical MP4s across
three trials of legacy/prepared/mixed/extent-copy paths. All decode to the same
3,600 frames and exact expected 120-second source interval. Extent reconstruction
uses the existing muxer's headers/index as its oracle, replacing the payload
through the new planner; it is not an independent MP4 metadata implementation.

The 3,660 prepared samples coalesced to 16 transfers. **Zero bytes were geometric
alignment candidates in this compact-layout fixture**: merely preparing samples
does not align source and destination offsets. All bytes were copied; there is no
clone or measured copy-saving claim. Reconstruction runs outside mux timing.

Next: alignment-aware MP4 region placement (with correct sample offsets and gaps
outside samples), then capability-gated filesystem cloning and fallback tests.
The generic planner deliberately does not infer filesystem support or issue clone
calls. Production mux integration, live multi-track A/V and recovery remain gates.
Installed software, user data and cache placement are untouched; no user testing
is needed yet.

## 2026-09-09: opt-in MP4-ready video spill

The first foundation from the [architecture report](replay-storage-architecture-report.md)
is implemented. It is **not** a zero-copy save implementation or a production
default change. Existing installed binaries, settings, clips and recording sessions
were not changed or restarted.

### Delivered

- A pure, bounded H.264 sample packer, separate from capture, disk I/O and muxing.
- An opt-in spill-worker integration (`CLIPTURE_REPLAY_MP4_READY=1`). Only video
  packets being persisted are prepared; resident packets retain the existing format.
- MP4 muxing that accepts prepared, legacy and mixed windows. Outputs remain
  ordinary standalone MP4 files.
- Successful writes atomically replace stored packet metadata; earlier snapshots
  remain readable. Unsupported input retains the legacy representation.
- Focused tests and module ownership notes, separate from existing large sources.

See the [implementation map](../engine/src/replay/README.md) and
[test instructions](../engine/tests/replay/README.md).

### Verification and measurements

The Release engine and both native test executables built successfully. Both CTest
suites and the host-contract checks passed. The build emits an existing nonfatal
`pwsh.exe` lookup warning; it still completes with exit code zero.

The generated-media runner uses the same 122-second H.264 input for three trials of
legacy, prepared and mixed storage. All nine MP4s are byte-identical, decode to
3,600 frames, and match exactly the source interval from 1.5 to 121.5 seconds.
This exercises a 120-second presentation starting between keyframes.

Latest retained run: `.cache/mp4-ready-3pb5gl/summary.json` (local, ignored artifact).

| Measurement | Legacy | Prepared |
| --- | ---: | ---: |
| Replay payload written | 5,596,902 bytes | 5,574,211 bytes |
| Final MP4 bytes written | 5,638,845 bytes | 5,638,845 bytes |
| Mux elapsed across three trials | 28–29 ms | 26–28 ms |

These small, fixed-order, warm-cache measurements establish compatibility and
logical I/O, not a meaningful speedup. The full output is still written. No RAM
budget was lowered; conversion temporarily holds one source and replacement
packet. This is video-only synthetic validation, not live multi-track A/V or a
supported-player matrix. The feature remains off by default.

### Next implementation boundaries

1. Add a payload extent/layout planner with explicit source ranges, alignment,
   immutable ownership and portable-copy fallback, independently tested.
2. Integrate filesystem capability checks and an optional ReFS clone backend;
   do not imply NTFS supports the same operation.
3. Validate overlaps, retirement, failures and live A/V before any rollout or
   disk-first/RAM-budget policy change. Keep the NTFS rolling-file experiment
   separate until its recovery and player-compatibility requirements are proven.

Nothing needs user testing at this checkpoint: no candidate was installed or
enabled. A later live-recording candidate needs explicit playback, audio-sync and
overlapping-save checks before changing defaults.
