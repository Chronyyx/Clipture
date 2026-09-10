# Isolated replay tests

The native fixture executables do not start capture, a Tauri host, or a WebView. Unit tests use
exclusive temporary directories; the generated-media comparison keeps artifacts
under the workspace `.cache/mp4-ready-*` directory. No existing clips are inputs.

```powershell
cmake -S engine -B build/engine -A x64
cmake --build build/engine --config Release --target clipture_replay_tests clipture_packet_tests --parallel 4
ctest --test-dir build/engine -C Release --output-on-failure
node scripts/testing/test-mp4-ready-replay.cjs
```

## Ownership

| File | Coverage |
| --- | --- |
| `Mp4SamplePackerTests.cpp` | Exact bytes, parameters, metadata, malformed/unsupported fallback, original input unchanged. |
| `Mp4ReplayStoreTests.cpp` | Disk spill, mixed-format MP4 parity, old snapshots, accounting, audio pass-through, disk failure. |
| `FixtureParity.cpp` | Real H.264 access units through old/new disk-backed stores and the existing MP4 muxer; I/O measurements. |
| `PayloadLayoutTests.cpp` | 9,984 small alignment layouts, coalescing, sample boundaries, invalid ranges and 64-bit overflow. |
| `AlignedLayoutTests.cpp` | 9,984 region-placement layouts, zero-gap writing/failure, small-region fallback, overflow and prepared-video eligibility. |
| `ClonePolicyTests.cpp` | Capability/identity/integrity/range gates and bounded request-size policy. |
| `CloneJobTests.cpp` | Simulated cloning, unsupported ranges, partial failure, fresh-copy retry, write/finish/cleanup/create failures. |
| `WindowsCloneTests.cpp` | Real sealed-source/open-handle checks, clone-or-copy probe, collision refusal, independent output, failed-file cleanup. |
| `NativeCloneRetryTests.cpp` | Simulated partial clone damage in real private Windows output files, discard/recreate and copy-only retry. |
| `PayloadCopyTests.cpp` | Bounded I/O, read/write/partial-write failures, fresh-output retry. |
| `ExtentStoreTests.cpp` | Actual disk pins surviving store destruction, overlapping saves, last-reference reclamation. |
| `ExtentFixtureParity.cpp` | Reconstructs a real MP4 payload through extent planning/copying using existing mux metadata as an oracle. |
| `ExtentTestSupport.hpp` | Synthetic 64-bit source and bounded/failing destination doubles. |
| `TestSupport.hpp` | Small synthetic fixtures and isolated directory ownership. |
| `main.cpp` | Test composition only. |

The JS runner generates 122 seconds of 160x90 H.264 at 30fps using bundled FFmpeg.
Each of three trials produces legacy, prepared, mixed-format and extent-copy MP4s. It checks
byte equality, 3,600 decoded frames, identical hashes/timestamps, and the exact
source frames selected by a 120-second edit beginning between keyframes.
`metrics.json`, `mux.log`, `extent.log`, `summary.json`, and output MP4s remain available for
inspection. The source fixture is intentionally small and generated in memory by
the test harness; do not use these process-memory figures as production RAM data.

Timing is a fixed-order, small synthetic comparison with OS caching and possible
background recording activity. It establishes correctness and logical I/O, not
gaming performance, physical disk bandwidth, p95 latency, or a supported-player
matrix. It tests video decoding; full live multi-track A/V validation is still a
release gate. Do not enable the production default based on this test alone.

The runner also generates two aligned MP4s per trial (4 KiB and 64 KiB). These
come directly from the existing muxer using newly planned
sample offsets, not patched reference metadata. The read-only inspector
`scripts/testing/mp4-layout-fixture.cjs` checks box bounds, `co64`/`stsz`, unchanged
sample bytes, zero-filled gaps outside samples, and identical movie metadata after
normalizing only the chunk offsets. All six aligned outputs decode to the same
3,600 frames. Trial zero additionally compares seeks at 0, 0.5, 60.125 and 119.5
seconds against compact output. `aligned-summary.json` records geometry/padding.
This is FFmpeg validation, not Windows/WebView2/editor compatibility certification.

Extent reconstruction is deliberately outside the measured mux timing. It copies
all payload bytes and does not call clone APIs. Its illustrative 4 KiB alignment
is not a filesystem probe. The fixture keeps existing MP4 metadata; this proves
payload compatibility, not an independent metadata builder or production mux
integration. The separate aligned-mux tests above do exercise new metadata offsets.
The runner now additionally processes a completed MP4 through the Windows sealed
file clone-or-copy backend, for 21 outputs total, verifying byte/frame equality.
It does not assemble a live replay MP4 with clone operations. `extent.log` states
actual cloned/copied bytes; NTFS reports zero cloned bytes and an explicit ReFS
success-coverage skip. Simulated clones never establish native ReFS support.

To probe an **existing authorized scratch directory** on a suitable test volume:

```powershell
build/engine/tests/replay/Release/clipture_replay_tests.exe --clone-probe X:\ExistingScratch
```

The probe creates an exclusive child directory, writes about 256 KiB of synthetic
data, checks clone/fallback parity, later edits/deletes only its own source, and
removes its child directory. It does not format/mount disks or touch recordings.
A successful process exit can mean correct copy fallback: require nonzero actual
cloned bytes and no skip to establish successful ReFS clone coverage. The full
suite runs the same checks on the default temporary volume.

Interrupted library publication, power-loss recovery, live A/V and real ReFS
success on this NTFS-only machine remain outside the verified coverage.

## In-place finalization proof

`InPlaceTests.cpp` covers ownership/state/rename safety and creates one additional
real MP4 per fixture trial, for 24 total. The H.264 media is appended before the
Save call. Native counters assert the full mux call neither reads nor rewrites
media; file identity checks prove publication renamed the same file. The JS
inspector checks unchanged encoded samples/timing tables at the new offsets;
FFmpeg checks the expected 3600 visible frames and four seek positions.
`in-place-summary.json` records logical media/metadata counts, not a disk benchmark.

The original proof above is now extended by the default-on live implementation.

## Audio, bounded reuse and live protocol smoke

- `InPlaceArchiveTests.cpp`: pinned slot reuse, 1,000 reuse cycles, cap fallback,
  store-worker integration, detached file identity, opt-out and consumed-window policy.
- `InPlaceAudioFixture.cpp`: two real encoded AAC tracks, late start/audio gap,
  exact 120-second then 10-second presentation, copied prior-file decoder preroll.
- `InPlaceRollingFixture.cpp`: 366 seconds through a 2 MiB arena with 16-second
  retention, then a 15-second independent MP4. Live packet pins protect reuse.
- `in-place-audio-fixture.cjs`: exact source-frame hashes, two decoded audio hashes,
  and video/audio seeks against compact references. The full runner now emits
  30 MP4s; these are correctness/logical-I/O fixtures, not game-load benchmarks.

An explicitly authorized hardware test is separate:

```powershell
node scripts/testing/run-in-place-engine-smoke.cjs --record-primary-display
```

It records the primary display with audio disabled into a fresh workspace scratch
profile using only a separately spawned engine. It verifies 5-second then shorter
consume-on-save output, immediate valid-JSON failure response/retry, opt-out,
distinct rapid-save filenames and FFmpeg decode. It neither installs software nor
restarts the normal app. Do not run without permission to record the display.
Real microphone/system capture sync, game-load performance, power-loss recovery
and a supported-player matrix remain outside these checks.
