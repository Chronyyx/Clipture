# Loopback silence scheduling & PCM mixer quantization

## Recurring 100 Hz mixer artifacts

The initial silence-delivery grace addressed poll starvation, but waveform inspection revealed a separate recurring 100 Hz audio buzz caused by mixer block truncation. Verification requires inspection across extended capture sessions rather than clean startup clips alone.

`AudioReplayCoordinator` truncated signed timestamp offsets toward zero when placing PCM in 10 ms mix blocks. A fractional positive phase puts the previous packet at -479.995 frames and the next at +0.005 (48 kHz). Truncation places both in output frame zero: one sample is doubled every 480 frames. This introduces 100 Hz artifacts and can activate the block peak limiter. It can appear after an initially aligned stream receives a clock correction.

`PcmBlockMixer.hpp` now rounds both sides to one nearest-sample grid, with consistent half-frame ties. Genuine separate sources still sum normally. No capture buffers, limiter settings, retention, or sample rates were changed.

Waveform analysis of multi-track captures (mic, Chrome, game, and Discord streams) confirmed strong 480-sample-period artifacts in all non-mic tracks (peak phase second-difference energy 116–154 times the phase average; mic about 1.3 times). This isolates the defect to mixer block quantization after clock corrections.

Regression coverage:

- `clipture_pcm_block_mixer`: phase sweep across a sample in both directions,
  including exact half-frame ties at 8 kHz, plus 44.1/48/96 kHz and source mixing.
  The original arithmetic fails the boundary continuity assertion.
- `testAudioMixerClockPhaseDoesNotChangeAac`: three minutes through the real
  coordinator and AAC encoder, with +/-100 ns phase introduced after minute
  one. Corrected output must match an aligned reference byte-for-byte. This
  advances synthetic timestamps without sleeping; it is not a physical
  three-minute WASAPI capture. Input is paced by coordinator watermarks.

## Empty-poll fallback

System and per-process WASAPI capture share `LoopbackSilencePolicy.hpp`. The
active microphone path does not use this fallback. Keep the policy shared when
editing either loopback path in `AudioCaptureWorker.cpp`.

`GetNextPacketSize() == 0` means **no packet is ready**, not that the next audio
interval is silent. The former empty-poll fallback fabricated 10 ms of silence
up to the current wall clock. A real packet arriving on the next poll was then
shifted after those zeros by the forward-only timestamp aligner. Continuous
audio could acquire gaps and an advancing timestamp offset.

Only fabricate a complete silence packet when its end is at least 200 ms behind
the wall clock (two requested 100 ms WASAPI periods). Real packets are still
drained immediately. This is a bounded delivery tolerance, not a guarantee
against arbitrarily long driver stalls. Do not remove it to make an idle audio
watermark reach the wall clock. Do not change capture buffers to fix this issue.
Actual WASAPI packets marked SILENT must still be processed normally.

## Regression checks

`clipture_loopback_silence` covers continuous streams with empty polls, 5–100 ms
packets, batched/jittered delivery, idle-source advancement, clock boundaries,
and resuming captured audio. It exercises the real forward-only aligner. The
original fallback fails the continuous-stream test. CTest includes this target
alongside the existing PCM/AAC/MP4 tests:

```powershell
cmake --build build/engine --config Release --parallel 4
ctest --test-dir build/engine -C Release --output-on-failure
```

These tests model delivery scheduling; they do not capture from a physical
endpoint. Verification protocol requires capturing new audio clips across
active games and background applications, validating silence intervals and
resumed playback across individual extracted AAC streams.

Windows contracts: [GetNextPacketSize](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudiocaptureclient-getnextpacketsize)
and [GetBuffer](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudiocaptureclient-getbuffer).
