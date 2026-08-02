# Changelog

## [1.2.2] - 2026-08-02

### Performance

- Add storage-aware adaptive save pacing: SSD output remains high-throughput while bounded sustained writes prevent large dirty-cache bursts, and slower or unknown storage learns a safer rate dynamically.
- Apply low Windows I/O priority to SSD output, very-low priority to seek-heavy storage, and reserve full background thread mode for sustained critical capture pressure.
- Stream replay archive ranges directly into the mux buffer, removing an extra disk-read scratch copy while retaining the 512 KB maximum output request.
- Move final directory creation, file placement, and clip-index persistence off Electron's synchronous main-thread path.
- Insert newly saved clips into the library incrementally instead of rescanning the full library twice at save completion.

### Audio Reliability

- Respect PCM container width, valid-bit depth, and block alignment when converting WASAPI input, including microphones that expose 24 valid bits inside 32-bit containers.
- Support packed 24-bit, 32-bit integer, and floating-point PCM conversion without misaligned samples that can sound like hiss or static.
- Run RNNoise processing only for its supported 48 kHz input rate while preserving normal capture at other device sample rates.

### UI And Diagnostics

- Make status notices dismiss automatically and keep settings notices scoped to the Settings tab.
- Add source-read, output-write, adaptive-rate, I/O-priority, and finalization-stage timing details for diagnosing slow saves without blocking capture.

### Validation

- Added native coverage for adaptive write-rate behavior, capture-pressure hysteresis, 512 KB write bounds, and PCM container conversion edge cases.
- Verified the native engine, packet architecture tests, Electron type checks, and production renderer build.

## [1.2.1] - 2026-08-02

### Performance

- Pipeline more converted NVENC input surfaces ahead as quality presets increase, giving P3 and higher enough conversion lead time to sustain the requested frame rate under game GPU load.
- Use non-blocking bitstream locks after asynchronous NVENC completion events, with bounded retries for transient driver contention instead of stalling encoder submissions.

### Audio Reliability

- Keep configured separate-app audio supervisors alive when an application is closed, launched late, or restarted, and automatically rebind capture to the new process.
- Select the root covering the largest matching process tree so Chromium-style applications include their helper audio processes while retaining one logical app track.
- Close process-loopback activation events and monitor target process lifetime so sessions restart cleanly without leaking native handles.

### Validation

- Added native coverage for the deterministic process-tree root selection used when app audio binds or rebinds.
- Verified sustained 60.00 FPS capture at 2560x1440 using NVENC P3 in the previously failing game workload.

## [1.2.0] - 2026-08-02

### New Features

- Added a rolling disk replay archive for encoded H.264 video and AAC audio, substantially reducing RAM usage for long replay windows.
- Added exact clip-window presentation: saves retain the preceding keyframe for reliable decoding while presenting precisely the requested duration.
- Added replay archive health, queued data, disk usage, RAM fallback, segment, failure, and maximum-write diagnostics.

### Performance

- Streamed disk-backed packets directly into the MP4 muxer without rebuilding the complete video in memory.
- Kept save writes capped at 512 KB and added adaptive pressure pacing that backs off when the live encoder queue needs time to recover.
- Moved normal replay persistence onto low-priority background workers while retaining a short hot packet window in RAM.

### Reliability

- Retain replay packets in RAM automatically if the disk archive cannot write, then retry without blocking capture threads.
- Keep active archive segments alive for in-progress saves and safely clean up abandoned replay sessions on the next launch.
- Prevent closed development console pipes from crashing Electron with an `EPIPE` main-process error.
- Added native coverage for disk persistence, retention, RAM fallback, bounded writes, snapshot lifetime, disk-backed MP4 muxing, and exact presentation edits.
