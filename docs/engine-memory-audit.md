# Engine memory audit — 2026-09-10

## Result and scope

Read-only audit of the already-running engine (PID 20740), guided by the
performance-analysis skill. No capture restart, settings changes, heap trimming,
process suspension, new recording, debugger injection or optimization was performed.
Windows queries inspected memory metadata, not captured frame/audio contents.

The executable at `release/tauri-unpacked/clipture_engine.exe` matches the current
Release engine SHA-256:
`4846A62168ACB7C42E53AABD5EDC7A4720219D7D52EDAE9E93B7AE569F8ED651`.
Saved configuration has Save in place enabled, 120 seconds, 60 fps, 40 Mbps,
NVENC P3, and six enabled logical audio rules. Enabled rules are not proof that
all six sources currently produce audio. Runtime protocol diagnostics were not
queried: the normal engine's stdio belongs to its controller.

## Measurements

Five one-second WMI samples returned the same values (short observation only,
not a long-run leak test):

| Counter | Bytes | Interpretation |
| --- | ---: | --- |
| Private working set | 304,316,416 | About 290.2 MiB of private resident RAM; consistent with the screenshot. |
| Total working set | 358,555,648 | About 342.0 MiB, including shareable resident pages. |
| Private bytes | 1,349,611,520 | About 1.26 GiB committed, not 1.26 GiB physically resident. |

An independent `VirtualQueryEx` + `QueryWorkingSet` metadata walk shortly before
these samples classified approximately 286.0 MiB as resident MEM_PRIVATE,
54.3 MiB as resident executable-image pages (4.2 MiB private), and 1.7 MiB as
resident mapped-file pages. Values are separate snapshots. Most displayed private
RAM is therefore not explained by ordinary shared file mappings. This does not
identify which allocator or driver created each private allocation.

Windows GPU process counters separately reported 974.4 MiB dedicated usage and
194.2 MiB shared usage for this PID. These are graphics-accounting views, not
an additive breakdown of the private working set; do not subtract 194 MiB from
290 MiB or add them as independent physical allocations. Per-process GPU counters
can include shared resources and have documented accuracy caveats. Treat them as
a reason to investigate the GPU pipeline, not exact allocation attribution.

Microsoft distinguishes working set from private commit in
[PROCESS_MEMORY_COUNTERS_EX](https://learn.microsoft.com/en-us/windows/win32/api/psapi/ns-psapi-process_memory_counters_ex).
See also [GPU accounting](https://devblogs.microsoft.com/directx/gpus-in-the-task-manager/)
and [GPU counter caveats](https://learn.microsoft.com/en-us/troubleshoot/windows-client/performance/gpu-process-memory-counters-report-wrong-value).

## Source findings, ranked for follow-up

### 1. Encoder/capture resource retention — highest-priority measurement target

`EncoderWorker.cpp:56` configures 32 output slots in both headroom modes.
`createOutputSlots` allocates an NVENC bitstream buffer for every slot. Slots can
also retain encoder-owned D3D textures/views; registered-input and input-view
caches have limits of 64 and 16 respectively. These are not 32 CPU copies of a
whole clip. Native driver allocation sizes and residency are not exposed here.

`CaptureBackend.cpp:567` warms four capture textures, grows the pool when leases
overlap, and keeps grown slots until reset or a generation change. There is no
steady-state shrink of unused slots. A transient queue stall can therefore leave
a larger graphics-resource high-water mark afterward. The frame queue is bounded
at 20, the encoder pending queue at 64, with additional latency admission policy;
those capacities do not prove that the queues are currently full.

Recommendation: measure actual allocated/in-flight/free slot counts and texture
dimensions, then benchmark a smaller/adaptive slot budget and safe idle retirement.
Do not blindly cut the queue: the headroom supports capture during encoder stalls.
NVIDIA documents input/output buffering and memory tradeoffs in its
[NVENC guide](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-video-encoder-api-prog-guide/index.html).
No exact RAM/VRAM saving or safe replacement slot count is established by this audit.

### 2. Recycled packet capacity remains after payload lifetime — bounded, reducible

`Engine.cpp:764` constructs video, PCM and AAC packet pools with respective
16/4/4 MiB recycle limits. `PacketRingBuffer.hpp` retains vector capacity after
clearing contents. This is up to 24 MiB of reusable capacity, not a measured
24 MiB currently occupied, and excludes live packets and allocator overhead.
`clearPool()` exists but has no call sites. Recycling avoids repeated allocation;
it should not be removed wholesale from the capture path.

Recommendation: expose live versus recycled capacity, use a smaller measured
disk-first budget, and retire excess recycled capacity after idle periods.

### 3. AAC has a redundant resident history; video does not

`EncoderWorker.cpp:3041` pushes full video packets into ReplaySegmentStore but
only payload-free metadata into its short hot ring. The disk worker replaces
resident payload with a file reader. With Save in place ON, video/AAC archive
resident budgets are zero (`Engine.cpp:1338` onward): spill is scheduled promptly.
There is no deliberate 120-second in-RAM video payload cache in this path.

`AudioReplayCoordinator.cpp:174`, unlike video, pushes a full AAC payload into
both the replay store and the hot AAC ring. Initially they share the allocation;
after disk persistence, the hot ring still pins the original RAM payload. Engine
configuration retains 12 seconds there. The hot/store merge provides transition
and recovery behavior, so replacing it requires commit-aware fallback tests.

Raw PCM retains up to five seconds, with normal pruning toward the two-second
repair window (`AudioReplayCoordinator.cpp:27`, `pruneRawPcm`, `setRetention`).
At 48 kHz/stereo/16-bit this is about 0.92 MiB per source for five seconds,
excluding metadata/pools/queues. Actual source count and formats vary.

Recommendation: make AAC hot history payload-free once persistence is confirmed,
preserving late/failed-spill fallback. Keep the PCM repair window until audio
recovery tests establish a safe smaller bound. These are likely smaller wins than
GPU resource retention, not an explanation for all 290 MiB.

### 4. Inactive audio encoders and save-time caches can stay warm

`AudioReplayCoordinator` creates TrackState/AAC encoder instances by logical ID.
The map is cleared on repair rebuild or object destruction, but has no normal
idle-track retirement; routing updates alone do not remove old TrackState entries.
Tracks retain encoder instances, vector capacities and potentially pending tail
packets. This is a lifecycle cleanup opportunity, not evidence of an observed leak.

Each of the three legacy stores has a shared 2 MiB read-ahead cache, lazily used
for reads. Closing the cached handle resets indexes but retains vector capacity
(`ReplaySegmentStore.cpp:121`). Thus up to roughly 6 MiB can remain after fallback
or recovery reads; pure same-file in-place saves need not allocate these caches.

Recommendation: retire inactive tracks only after flushing/accounting for AAC
delay and replay watermarks; add idle release for cold read caches.

## Limits and next step

This audit establishes measured process totals and concrete ownership/retention
targets, not a byte-exact heap attribution or guaranteed optimized footprint.
Existing archive diagnostics omit packet-pool capacity, hot-ring payloads,
inactive AAC encoder internals and GPU surface allocation counts. Driver images'
mapped sizes are also not their resident RAM cost.

Next implementation should add bounded per-owner counters first, then compare
one change at a time under real capture: steady state, long session, save,
post-save idle, resolution switch, audio-source removal and game-load stalls.
Preserve frame-drop, latency and audio-sync behavior. Do not use EmptyWorkingSet
or forced heap trimming to make Task Manager look smaller without releasing
unneeded resources. User approval to optimize is separate from this audit.
