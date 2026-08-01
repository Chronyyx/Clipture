# Clipture

Clipture is a Windows replay-buffer application built around low-latency NVIDIA NVENC capture. It continuously keeps compressed video and audio in RAM, then writes the selected time window to MP4 when a clip is saved.

## Current Release

Version `1.1.4` is a capture-stability and save-performance release focused on smoother high-refresh-rate recording and predictable clip finalization.

## What's New in 1.1.4

- Adds a phase-locked target-FPS capture sampler so 144 Hz and 210 Hz displays do not alias below the requested 60 FPS.
- Introduces a dedicated DXGI Desktop Duplication backend with Windows.Graphics.Capture fallback, recovery diagnostics, and consistent cursor handling.
- Pipelines NVENC submission and asynchronous output draining with bounded queues, direct native-size BGRA input when supported, and detailed frame-loss diagnostics.
- Preserves real capture timing through the MP4 timeline instead of shortening clips when a frame deadline is missed.
- Separates software encoder backlog from normal NVENC in-flight work when pacing saves.
- Caps critical save pacing to one millisecond per bounded write, preventing sustained pressure from turning a normal save into a long recovery loop.
- Improves startup behavior and development launching so stale packaged binaries do not interfere with local builds.

## 1.1.3 Highlights

- Keeps live AAC timelines continuous through quiet periods to prevent repeated audio in saved clips.
- Normalizes rare AAC gaps and overlaps before MP4 muxing so audio timestamps remain monotonic.
- Removes the temporary mixed-video diagnostic export control.

## 1.1.2 Highlights

- Bugfix: restores dragging from the very top of the app without covering the Clipture icon or window controls.

## 1.1.1 Highlights

- Removes the full-width titlebar layer that could cover the top of the Clipture icon and sidebar branding.
- Keeps only the update, minimize, maximize, and close control area at the top right.
- Places the Save last button back at the right edge, directly below the window controls.

## 1.1.0 Highlights

- Applies engine settings before capture is armed, avoiding an unnecessary default capture startup and restart.
- Staggers optional app-audio capture and game detection after the core video, system-audio, and microphone paths are running.
- Replaces PowerShell/WMI startup process scans with native Windows process snapshots and queries executable paths only when needed.
- Indexes imported directories asynchronously without copying videos or creating preview files in those folders.
- Loads clip thumbnails only near the viewport, limits concurrent extraction, and keeps a bounded RAM cache of compressed 480x270 JPEG thumbnails.
- Adds library tabs, folder-based filters, imported-video management, filtered Select all, and easier clip selection/deletion.
- Streams seekable video from the original file and uses bounded rolling audio chunks for mixed multi-track previews.
- Adds video click-to-play, double-click fullscreen, hour/day duration formatting, and accelerated five-second keyboard seeking with animated cumulative feedback.
- Makes clip length and bitrate fields editable as drafts so values can be cleared and replaced normally before validation.

## Features

- DXGI Desktop Duplication capture with automatic Windows.Graphics.Capture fallback.
- Direct NVIDIA NVENC H.264 encoding with runtime API compatibility checks.
- Configurable 24, 30, or 60 FPS capture and up to ten minutes of replay history.
- System, microphone, detected game/app, and explicit per-app audio capture.
- Separate AAC tracks with silent-track omission and short PCM recovery coverage.
- Shared packet payloads that avoid copying the full replay buffer while saving.
- MP4 muxing directly from buffered H.264 and AAC packets.
- Background-priority saves with preallocated files and writes capped at 4 MB.
- Resolution-change segmentation and stream-copy stitching when compatible.
- HDR-to-SDR tonemapping on supported HDR capture paths.
- Searchable clip library with folder filters, multi-select deletion, renaming, and non-copying imported video directories.
- Range-buffered playback with rolling mixed-audio chunks, fullscreen controls, and accelerated keyboard seeking.
- Viewport-aware 480x270 thumbnails with bounded extraction concurrency and compressed RAM caching.
- Global save hotkey, tray operation, startup-on-login, notifications, and automatic updates.

## Architecture

```text
Video
DXGI Desktop Duplication (WGC fallback)
  -> D3D11 texture
  -> phase-locked target-FPS sampler
  -> bounded pipelined BGRA-to-NV12 conversion
  -> NVENC H.264
  -> shared packet payload + cached NAL metadata
  -> RAM video ring

Audio
WASAPI capture
  -> short PCM recovery ring
  -> live routing/mixing coordinator
  -> Media Foundation AAC
  -> RAM AAC ring

Save
select packet ranges
  -> metadata-only MP4 plan
  -> bounded background-priority file writer
  -> final MP4
```

The normal save path does not re-encode video, rescan the full H.264 stream, or copy the entire clip into a second buffer. PCM-to-AAC encoding remains available as automatic recovery when live AAC coverage has a gap.

Startup follows a configure-before-arm sequence. Core capture starts first; optional app loopback workers and game detection begin after a short delay so opening Clipture does not launch every expensive subsystem at once.

The library reads saved clips and imported directories in place. Imported files are not duplicated, and renaming or deleting an imported card changes the original file. Thumbnail previews are generated in RAM and never stored beside the source video.

## Memory Use

The replay buffer intentionally lives in RAM and does not continuously write temporary recordings to disk. Compressed video is the largest component:

```text
video RAM bytes ~= bitrate in Mb/s * clip seconds / 8
```

For example, two minutes at 80 Mb/s is approximately 1.2 GB of compressed video before capture surfaces, packet capacity, audio recovery data, and normal process overhead. Memory should level off after the configured replay duration. Saving should not create another full-size copy of the buffered video.

## Requirements

- 64-bit Windows 10 or Windows 11.
- An NVIDIA GPU with NVENC H.264 support.
- An NVIDIA driver compatible with the NVENC API used by the build.
- CMake and Visual Studio C++ build tools when compiling the native engine.
- Node.js and npm when building the Electron application.

## Build

Install dependencies and build the native engine and UI:

```powershell
npm.cmd install
npm.cmd run build
```

Build only one side:

```powershell
npm.cmd run build:engine
npm.cmd run build:ui
```

Build the Windows installer:

```powershell
npm.cmd run dist:win
```

Build an unpacked Windows application:

```powershell
npm.cmd run pack:win
```

Outputs are written to:

```text
build/engine/Release/clipture_engine.exe
dist/
release/Clipture-Setup-<version>.exe
release/win-unpacked/
```

## Run

Run the development server with hot reload:

```powershell
npm.cmd run dev
```

Run an already-built Electron application directly:

```powershell
npm.cmd run start:dev
```

Build the engine and UI, then run Electron directly:

```powershell
npm.cmd start
```

`npm.cmd start` does not rewrite `release/win-unpacked`, so a running packaged or tray instance cannot lock development startup. Use `npm.cmd run start:packaged` only when testing the unpacked packaged application itself; exit any copy running from `release/win-unpacked` before rebuilding that directory.

Development builds can temporarily use substantial CPU, disk, and memory and should not be used to judge installed-app startup performance.

Installed startup with `--hidden` opens Clipture in the tray while the native capture engine begins filling the replay buffer.

## Library Controls

- Click a thumbnail to open a clip.
- Click the video to play or pause and double-click it to toggle fullscreen.
- Press Left or Right to seek five seconds. Hold either key for more than 300 ms to accelerate seeking.
- Imported Videos reads videos directly from selected folders. It does not copy them into Clipture storage.
- Rename and delete actions affect the underlying file for both saved and imported clips.

## Diagnostics

Application data and logs are stored under:

```text
%APPDATA%\Clipture\data
```

Useful files include `settings.json`, `clips.json`, `save-timing.log`, and `updates.log`.

Run a native engine smoke test with:

```powershell
'{"id":1,"type":"configure","fps":30,"bitrateMbps":40,"clipLengthSeconds":30,"monitorId":"primary"}' | .\build\engine\Release\clipture_engine.exe
```

Diagnostics reports the requested and active capture backends, measured display refresh rate, fresh-frame rate, repeats, queue drops, and encoder-stage latency. If DXGI Desktop Duplication cannot start or recover, automatic mode quarantines it for that monitor and falls back to Windows.Graphics.Capture. A forced WGC failure such as `CreateForMonitor failed: HRESULT 0x80070424` is reported as degraded capture instead of silently treating the engine as armed.
