# Changelog

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
