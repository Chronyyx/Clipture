# Clipture 1.4.0: Smooth 60 FPS Capture Engine & OBS-Aligned Frame Pacing

## Highlights

- **OBS Studio Direct Sampling:** Re-architected desktop duplication and frame delivery to match OBS Studio's direct on-tick sampling model, eliminating thread-hop pre-arm races, queue latency, and 3:2 cadence judder.
- **Unified D3D11 Pipeline:** Unified the DirectX 11 capture, color conversion, and NVENC encoding devices (`kPreferSharedCaptureDevice = true`), completely eliminating cross-device KeyedMutex `AcquireSync`/`ReleaseSync` synchronization stalls and GPU latency spikes.
- **WDDM Realtime GPU Priority & Video Acceleration:** Elevated capture and encoder D3D11 thread priority to maximum realtime (`SetGPUThreadPriority(7)`) and enabled `D3D11_CREATE_DEVICE_VIDEO_SUPPORT`, guaranteeing 60 FPS capture execution even under 99–100% GPU loads during in-game smoke and particle bursts.
- **Precision Hybrid Scheduler Clock:** Integrated high-resolution hybrid timer (`PrecisionTimer`) combining Windows `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`, `timeBeginPeriod(1)`, and sub-millisecond CPU pause/yield spin loops to eliminate standard Windows sleep latency jitter.
- **NVENC VBV & 2.0s GOP Alignment:** Configured 1-second VBV buffer provisioning (`vbvBufferSize = averageBitRate`) to absorb high-entropy particle bursts without encoder stalling, and standardized keyframe intervals to 2.0-second GOP cadence without manual IDR collision pulses.
- **Constant Frame Rate (CFR) MP4 Muxer:** Quantized video sample durations in `Mp4Muxer` to exact integer multiples of `10,000,000 / fps`, eliminating duration variance in the MP4 `stts` atom for flawless 60 Hz playback across all media players.
- **Diagnostics & Telemetry:** Added full frame drop diagnostics and telemetry reporting granular capture and encoder drop metrics.
