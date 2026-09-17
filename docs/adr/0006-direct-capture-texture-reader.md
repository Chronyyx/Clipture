# ADR 0006: Direct capture texture reads without producer-side GPU waits

Date: 2026-09-13

## Decision and scope

Replace ADR 0005's default reusable prepared-frame bridge with direct reads of
leased capture textures on the isolated encoder device. This supersedes only
the two-way bridge handoff decision; deferred preparation, isolated NVENC,
canonical conversion and rational cadence remain intact. The bridge remains a
capability fallback and the `CLIPTURE_DIRECT_TEXTURE_READ=0` comparison path.

The synchronized September 13 smoke trial localized long sampled GPU intervals
around handoff waits. In particular, the producer's wait for the consumer was
queued on the same immediate context as subsequent capture copies. Removing a
CPU blocking call was insufficient: GPU work behind that wait still depended
on encoder progress. See `../cs2-capture-theory-tests.md` for evidence and limits.

## Ownership and ordering

1. Capture copies into its existing owned texture pool and releases DXGI.
   Prepared BGRA textures are shareable; raw HDR inputs remain private.
2. The encoder submit worker selects a frame and performs once-only HDR/cursor
   preparation. Preparation still uses the protected capture context, not the
   cadence scheduler. No shader pass is added for discarded or repeated frames.
3. `SharedTextureReader` opens the source on the encoder device, caching the
   alias by source lifetime. It signals readiness on the producer and waits for
   readiness **only on the consumer**. There is no producer wait or bridge copy.
4. The encoder converts/copies directly into its existing canonical image,
   signals consumer completion, and continues with existing NVENC input slots.
5. `GpuTextureReadState` retains that completion fence in the source pool slot.
   CPU lease release alone cannot permit reuse. Capture checks completion
   nonblockingly and chooses another free slot; it never queues a consumer wait.

Completion guards survive reader/session destruction and retain all outstanding
reader fences, not just the current session. Pool recreation retires old slots;
old leased frames stay valid. Errors fail closed: a slot with an uncompleted
read cannot be overwritten merely because a submission or session failed.

## Resource and compatibility policy

No replay RAM limit, job queue (64), NVENC output-slot count (32), resolution,
bitrate, audio setting, or rational 100 ns cadence changes. The existing pool
warms four slots and can grow under CPU lease pressure as before. It does not
grow solely because GPU readers are pending: if no slot is usable, capture
records an owned-slot drop instead of waiting or allocating around GPU backlog.
This trades freshness under extreme backlog for bounded, corruption-free reuse;
it does not guarantee 120 distinct images from a saturated physical GPU.

Shared texture aliases are not additional pixel buffers, but sharing/fence
metadata and driver allocations still have costs. HDR needs the independent
raw inputs introduced in ADR 0005. Do not claim zero RAM/VRAM.

If shareable allocation, source opening or reader initialization is unsupported,
retain the old bridge. Existing shared-device NV12, isolated BGRA and engineering
keyed-mutex paths remain available. A successful startup must log
`handoff=direct-texture-read producerWait=false` before claiming the new path is
active. Mid-read errors propagate rather than switching paths unsafely.

## Verification and remaining validation

`SharedTextureReaderTests` parks the consumer GPU, queues four distinct images,
releases their CPU leases and verifies that capture-side GPU work still finishes.
It rejects premature slot reuse even after reader destruction, then releases the
consumer, checks every output pixel and verifies original-slot reuse. Resources
are preallocated before parking the consumer: driver resource creation itself
may wait for outstanding work. Shared HDR tests verify tonemap/cursor pixels.

`probe-direct-reader.cjs` exercises direct and bridge 120 FPS, traced direct,
scaled NV12/BGRA, keyed/shared-device fallbacks, and direct 144/210/240 FPS. It
requires correct mode activation, decoding, throughput, zero reported drops and
clean EOF shutdown. These desktop tests do not prove smoke improvement; a
matched clear/smoke/clear run with aligned GPU sampling remains necessary.

## API basis

- [D3D11 context Wait](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_3/nf-d3d11_3-id3d11devicecontext4-wait): queues a GPU wait for future work.
- [Fence completion value](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_3/nf-d3d11_3-id3d11fence-getcompletedvalue): checks progress without waiting.
- [Shared resource flags](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_resource_misc_flag): shared 2D texture restrictions. Legacy shared texture handles are not NT handles and are not closed; shared fence handles are closed after opening.
