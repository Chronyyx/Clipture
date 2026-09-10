# Replay storage implementation map

Keep one behavior per module. Capture and encoding stay outside this directory;
no Tauri, frontend, user-library or installer dependencies belong here.

## Current default: in-place media archive

- `InPlacePacketArchive.cpp`: shared video/AAC arena, bounded slot reuse, immutable
  range leases, spill-worker integration and detach-on-save. No capture-thread I/O.
- `InPlaceExtent.hpp`: a lease identifies its native file and protects one range.
- `InPlaceMediaBuffer.cpp`: adopts a sealed arena, reuses same-file sample offsets,
  and appends missing/resident/foreign decoder-preroll samples one at a time. The
  path constructor and append API remain useful for standalone fixtures.
- `ConsumedWindow.cpp`: pure visible-window selection and success-only consumption.
  Keeps necessary keyframe preroll without showing already-saved footage.

`saveInPlace` defaults to true. OFF restores overlapping saves and the existing
hybrid RAM policy. ON spills video/AAC promptly into the configured save folder's
`.clipture-replay` directory; PCM recovery remains separate. Unsupported/full/error
storage uses the legacy spill fallback. See [milestone 6](../../../docs/replay-storage-progress.md)
for lifecycle, copy/padding costs and verification limits.

## MP4-ready sample packing

- `Mp4SamplePacker.cpp` and its public header: pure conversion of one resident
  Annex B packet to MP4 sample bytes. No disk access or source mutation.
- `H264DecoderConfig.hpp`: immutable SPS/PPS bytes, independent of payload offsets.
- `../ReplaySegmentStore.cpp`: existing persistence worker invokes the packer
  only when `prepareMp4Samples` is enabled. It installs new bytes/metadata under
  its snapshot mutex only after a successful write. Source snapshots remain valid.
- `../Mp4Muxer.cpp`: consumes MP4-ready samples directly, while retaining the old
  Annex B path. Mixed-format windows are intentional, not an error.

With Save in place OFF, `CLIPTURE_REPLAY_MP4_READY=1` opts the engine into preparing video **when it
spills to disk**. Any other value keeps the old path. It does not change the RAM
budget, cache placement, audio representation, output format, or save contract.
That environment flag is separate from the default-on Save in place UI setting.

The packer strips SPS/PPS/AUD from the sample, keeps the first SPS/PPS out of band,
preserves SEI and slices, and writes four-byte big-endian NAL lengths. This matches
the existing muxer. Packet timing, keyframe and codec epoch information survives.
Unsupported/malformed/parameter-only/oversized input stays on the legacy path.
Only one packet is formatted at a time; the source and replacement can temporarily
coexist. This is not yet an aligned extent format or a zero-copy writer.

See [focused tests](../../tests/replay/README.md) and
[the architecture report](../../../docs/replay-storage-architecture-report.md).

## Implemented: pinned payload plans and portable copying

- `PayloadExtent.hpp`: a source reference plus 64-bit offset/length. A source
  guarantees immutable committed bytes for the pin lifetime, while permitting
  appends. It is not a pathname or a clone-capability claim.
- `PacketPayloadReader::extent()`: optional contiguous backing information.
  Existing opaque readers return no extent. `SegmentPayloadReader` exposes its
  already-owned backing; a plan can outlive both the reader and replay store.
- `PayloadLayoutPlan.cpp`: validates ranges and arithmetic, preserves one output
  offset per sample, and coalesces adjacent ranges on the same backing. Splits
  geometric alignment candidates from copied boundaries. Compact placement inserts
  no gaps; opt-in `AlignRegions` adds bounded gaps between source regions, outside
  samples. Neither reads neighboring bytes nor changes the media. Zero alignment
  means compact, copy-only placement.
- `CopyPayloadWriter.cpp`: reads/writes through caller-supplied scratch storage;
  no whole-save payload allocation. Copies candidates and zero-fills planned gaps,
  accounting for media and padding separately. Read/write errors stop
  immediately; a partially written destination must not be published.

The planner accepts **already-formatted** sample extents, not arbitrary Annex B
packets. The caller selects/normalizes samples and supplies the media area's start
offset. MP4 headers, edits, codec configuration, mixed resident input, durability,
and publication remain separate responsibilities. A sink must target a private
output distinct from every source; no in-place modification is permitted here.

Production readers expose extent pins; opt-out saves retain the compact mux
writer. The copy executor is exercised by unit tests and isolated reconstruction.
An internal opt-in path now feeds aligned sample offsets into the existing MP4
metadata builder and writes region padding; see [mux ownership](../mux/README.md).

## In-place ownership

`InPlaceMediaBuffer.cpp` is noncopyable and owns the synchronous finalization
session. Standalone append rejects per-track non-increasing timestamps and video
epoch/dimension changes. Same-file samples reuse their offsets; foreign samples
are copied with a bounded one-sample working set, not rejected or silently aliased.

`InPlaceIo.hpp` separates logical media writes, media reads and metadata writes.
The Windows backend owns file handles and rename; the mux module owns MP4 prefix
layout. No capture, retention or library responsibilities are added to the packer.

The archive, not this finalization session, owns retention and rolling reuse.
Save detaches before zeroing expired free slots and sealing; new writes can proceed
in another arena. Failed detached files are retained, but no recovery journal or
power-loss recovery guarantee is implemented. A/V tests are in milestone 6.

## Clone integration (deferred)

The standalone [Windows clone backend](../platform/windows/README.md) now handles
sealed files with a fresh-file copy retry after clone failure. It is not yet a
live replay source adapter or capture-aware MP4 writer integration.

Aligned placement now creates geometric candidates in ordinary MP4 output, but
still copies all bytes. Never enable padding by default without a measured benefit.
Keep filesystem-specific capability checks and clone operation limits separate
from generic indexing. `AlignmentCandidate` does not prove clone eligibility.
Do not add ring overwrite, disk-first defaults or ReFS capability policy to the
packer. Those require their own lifetime, recovery, fallback and performance tests.
