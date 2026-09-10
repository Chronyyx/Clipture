# Loopback silence scheduling

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
endpoint. Before declaring a reported buzz resolved, record a fresh clip with
the rebuilt engine, with audible system/app audio, including silence followed
by resumed playback. Check the separate non-mic tracks in another player too.
An already saved clip cannot validate a new capture fix.

For the reported September 10 clip, all three AAC streams decoded. The Chrome
track decoded to silence; Discord and mic contained audio. This inspection does
not establish that the silence-scheduling defect caused that particular buzz.

Windows contracts: [GetNextPacketSize](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudiocaptureclient-getnextpacketsize)
and [GetBuffer](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudiocaptureclient-getbuffer).
