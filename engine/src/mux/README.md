# Experimental aligned MP4 placement

## In-place finalization

`InPlaceMp4Header.cpp` builds only the bounded reserved prefix: `ftyp`, a `free`
box, then a large-size `mdat` header. Media starts at byte 4096 and never moves.
`MuxWritePacing::experimentalInPlace` explicitly selects a frozen, caller-owned
media buffer. The existing mux builder supplies `moov` and edit/sample timing,
using validated original file offsets. The backend appends that index, patches
the prefix and renames its owned file. No copy writer or ReFS operation is used.

The engine supplies that optional pointer for single-segment Save in place.
Normalized AAC is supported, including silence/PCM repair from the existing muxer.
Same-file payloads keep their offsets; missing/resident/foreign samples are appended
before final metadata is built. Standard system/microphone ordering avoids a
reorder-only host remux. Multi-resolution stitching still uses existing processing.
The pointer defaults to null for other callers. Rolling storage and consumption
belong to `replay`, not the MP4 header builder. See milestone 6 in the checkpoint.

## Aligned placement

Keep format/layout policy here; filesystem operations belong in a separate
platform backend. The existing `Mp4Muxer.cpp` still owns its metadata builder,
audio normalization and paced writer. Do not duplicate those behaviors here.

- `PreparedVideoLayout.cpp` gates eligibility and requests a pinned region plan.
  It accepts only prepared, disk-backed H.264 samples with explicit source extents.
  Legacy, opaque or resident packets and invalid alignment select compact fallback.
- `WritePadding.hpp` streams zero bytes through the existing buffered writer, so
  its output pacing, error state and I/O accounting remain in effect.
- `replay/PayloadLayoutPlan.cpp` owns arithmetic, grouping and sample offsets.
  `AlignRegions` inserts a minimal gap before a contiguous region to match its
  source alignment residue, only when the aligned body exceeds the gap cost.
  It never changes sample lengths, reads unselected neighboring bytes, or pads
  every packet. Heads/tails still require copying.

`MuxWritePacing::experimentalPayloadAlignment` is an internal, default-zero test
option, not a persisted setting or protocol command. `Mp4Muxer` requires no audio
tracks for this path; audio-containing saves use the existing compact layout.
The fixture explicitly passes 4096 and 65536. Neither value is a volume probe.

Eligible output uses a 16-byte, large-size `mdat` header even for small fixtures.
That avoids changing header width after padding moves the output past 4 GiB.
The existing metadata builder receives the planned per-sample file offsets;
`co64` points past zero-filled gaps and `stsz` contains only original sample sizes.
Movie timing, edit lists, codec data and all other metadata retain existing logic.

The aligned mux path still copies each sample through the original paced reader.
It does NOT clone, reduce RAM budgets, change cache placement, or improve measured
save latency. Enabling it without cloning adds disk writes for the padding.
Keep it off by default until backend capability and performance gates pass.

Geometry is only one prerequisite. The standalone [Windows backend](../platform/windows/README.md)
now checks volume/file capability and implements clone/copy retry for sealed files,
but it is not integrated into this paced mux path. Successful native ReFS cloning
is still unverified on the current NTFS-only machine. See the documented
[ReFS restrictions](https://learn.microsoft.com/en-us/windows-server/storage/refs/block-cloning).
Do not infer support from `AlignmentCandidate`, extension, or shared directory.

See [tests](../../tests/replay/README.md) and
[checkpoint](../../../docs/replay-storage-progress.md).
