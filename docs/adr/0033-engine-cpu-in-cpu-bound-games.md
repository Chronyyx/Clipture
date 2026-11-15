# ADR 0033: Engine CPU in CPU-bound games

Date: 2026-11-14

Status: implemented on `perf/integrated`; the GPU raise policy is covered by
tests only and waits for a starved session to confirm it live.

## Context

ADRs 0015 to 0032 tuned the frame path against GPU-bound load. Deadlock on the
development PC (Ryzen 9 3900X, RTX 3080, 1440p HDR, recording 60 fps P3 with
the microphone and game tracks) is CPU-bound: PresentMon showed about 10 ms
of CPU per frame against 4.6 ms of GPU, and the 3D engine ran at about a third
of capacity. There the engine's CPU, not its GPU work, is what it costs the
game.

Per-thread CPU from cycle counts (`QueryThreadCycleTime` by thread name,
sampled from outside the engine) during a live match:

| Thread | ms per second | Priority |
| --- | ---: | ---: |
| audio-mic | 124-138 | 24 (MMCSS Pro Audio) |
| audio-replay | 50-55 | 13 |
| replay-store (three) | 41-44 | 13, background mode |
| encoder-submit | 28-29 | 21 |
| game-detection | 25 | 13 |
| encoder-scheduler | 19-20 | 18 |
| everything | 371-396 | |

The microphone thread alone was a third of the engine, above the game's own
threads in priority.

## Decisions

- **RNNoise is built as upstream builds it** (`clipture_rnnoise`): with
  `DISABLE_DEBUG_FLOAT`, the int8 network instead of the float debug copy of
  its weights, and with run-time selected SSE4.1/AVX2 kernels
  (`RNN_ENABLE_X86_RTCD`; MSVC defines no `__SSE2__` on x64, so every kernel was
  scalar). The float build streamed 11 MB of weights per 10 ms frame. Per
  frame: 1,110 us before, 150 us after (7.4x). Output against the float build:
  correlation 0.98, mean VAD 0.804 against 0.808 on voice and 0.528 against
  0.526 on noise, noise attenuation -35.4 dB both. The engine binary shrinks
  from 16 MB to 5 MB. `clipture_rnnoise_kernel` checks the int8 network is
  built and that noise is attenuated while a voiced signal passes.
- **System loopback and the microphone are captured only while a track
  records them** (`AudioCaptureWorker::setCoreSources`). Their packets were
  dropped when the track was off.
- **Packets in flight are read only when a pass can act on them**: while
  raised, or for a game in front that dropped frames since the previous pass.
  Below a raise only the queued-drops rule uses the queue, and it needs a drop
  in the same pass. game-detection: 25 to 9 ms per second in a match that
  recorded cleanly.
- **No busy-wait before encoder ticks** (`CLIPTURE_ENCODER_SPIN_TAIL_100NS`,
  default 0, was 0.2 ms). Output timestamps come from the cadence. Saved about
  1.5 ms per second, less than the spin's length suggested: the
  high-resolution timer usually fires inside the tail.
- **Live in-place packets are written in runs** (`ReplaySegmentStore`,
  `InPlacePacketArchive::persistRun`). With "save in place" every video and AAC
  packet had its own `WriteFile`, about 155 a second at about 170 us. Packets
  now gather for up to 200 ms, resident and readable meanwhile, and up to 64
  are written at once through the arena's run write, each with its own lease.
  A `waitUntilIdle` caller ends the gathering at once. The audio load bench
  (`clipture_audio_load_bench`): replay-store 11.7 to 2.3 ms per second.

### GPU priority raises (ADR 0029)

The raise rules moved into a pure `GpuRaisePolicy` with tests
(`clipture_gpu_raise_policy`), and three cases changed:

- A remembered game raised the engine while it merely ran. With such a game
  left in the background, another game or the desktop paid for realtime. A
  remembered game now raises when it comes to the front.
- A raised game kept realtime after the player switched to another game. It
  now steps down after three passes (about 6 s) out of the front, and raises
  again at once when it returns, without waiting for new drops.
- A raise that does not help is undone. After 15 passes at realtime, if
  recording still loses at least 2.5 % of its frames and at least half as many
  as at normal, the GPU was not what starved it (CPU contention, a full disk);
  that game session is not raised again. A raise from memory has no rate at
  normal to compare and is not judged.

## Evidence

Live Deadlock, engines swapped in the developer's own Clipture during one
match, ABBA, 30 s settle and 150 s measured per block, PresentMon and
per-thread CPU:

| | Baseline | Integrated (all but the replay runs) |
| --- | ---: | ---: |
| Engine CPU, ms per second | 396 / 392 / 392 | 283 / 280 / 275 |
| audio-mic | 131 / 138 / 133 | 43 / 44 / 43 |
| game-detection | 26 / 25 / 25 | 9 (two blocks) |
| Game fps | 111 / 92 / 93 | 103 / 95 / 87 |
| Game 1 % low fps | 51 / 39 / 42 | 42 / 43 / 43 |

The match moved the game's frame rate by 20 fps between blocks of the same
engine; these blocks cannot show a game fps difference of the size 0.11 core
could make, in either direction.

Stress window, CS2 setup of the frame-stability skill with the injected
encoder stall (`40,700`), engines alternated, runs out of focus excluded:

| | Baseline (4) | Integrated with replay runs (5) |
| --- | ---: | ---: |
| Stutter frames | 1.3 ± 1.9 | 1.6 ± 1.8 |
| Missed frames | 18.8 ± 19.9 | 13.6 ± 13.3 |
| Longest freeze | 37.5 ms | 36.7 ms |
| Window fps lost, smoke / molly / spike | 12.2 / 8.4 / 7.5 % | 13.4 / 6.8 / 6.8 % |
| Engine CPU (process) | 0.21 cores | 0.13 cores |

No recording change; every saved clip, written through the in-place runs,
decoded with no unreadable frame. The window-cost differences change sign
between phases and stay within the spread between runs.

## Investigated, not changed

- The AAC encoder is 78 % of audio-replay: about 9 ms per second per stereo
  track (Media Foundation). Mixing, sorting and pruning are a few ms.
- `PacketRingBuffer`'s payload pool scans its recycled list on every acquire:
  4.6 us with the audio pool full, about 2 ms per second in all.
- The remaining audio-mic time (about 43 ms per second) is WASAPI wakeups every
  10 ms, conversion, publishing and RNNoise.

## Consequences

- With the microphone track on, the engine uses about 0.11 of a core less in
  a CPU-bound game, most of it from a thread that outranks the game's.
- A game session found not to benefit from realtime stays at normal until it
  restarts, even if its load changes later in the session.
- Live packets reach the in-place arena up to 200 ms later. They are resident
  and saved from memory meanwhile, as before for the last few milliseconds.
