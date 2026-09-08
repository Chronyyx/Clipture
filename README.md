# Clipture

Clipture is a Windows replay-buffer application built around low-latency NVIDIA NVENC capture. It continuously persists compressed video and audio into a rolling replay archive, then muxes the selected time window into MP4 when a clip is saved.

Release history and patch notes live in [CHANGELOG.md](CHANGELOG.md).

## Features

- DXGI Desktop Duplication capture with automatic Windows.Graphics.Capture fallback and non-blocking frame caching.
- High-precision hybrid clock combining Windows waitable timers with sub-millisecond CPU pause loops for jitter-free 60 FPS pacing.
- Direct NVIDIA NVENC H.264 encoding with runtime API compatibility checks.
- Configurable 24, 30, or 60 FPS capture and up to ten minutes of replay history.
- Constant-frame-rate output with real encoded samples for unchanged desktop intervals, using compact repeated-frame runs instead of duplicate queued jobs.
- Unified capture and encoder D3D11 device architecture eliminating KeyedMutex cross-device stalls and GPU pipeline flushes.
- System, microphone, detected game/app, and explicit per-app audio capture.
- Separate AAC tracks with silent-track omission and short PCM recovery coverage.
- Shared packet payloads that avoid copying the full replay buffer while saving.
- Judder-free MP4 muxing with CFR-quantized sample durations directly from buffered H.264 and AAC packets.
- Adaptive storage-aware saves with preallocated files, low I/O priority, and writes capped at 512 KB.
- Hybrid RAM/disk replay payload budgeting with automatic background disk spilling to preserve memory under high load.
- Resolution-change segmentation and stream-copy stitching when compatible.
- HDR-to-SDR tonemapping on supported HDR capture paths.
- Searchable clip library with folder filters, multi-select deletion, renaming, and non-copying imported video directories.
- Range-buffered playback with rolling mixed-audio chunks, Spacebar controls, automatic resume after unloaded seeks, fullscreen controls, and accelerated keyboard seeking.
- Viewport-aware 480x270 thumbnails with bounded extraction concurrency and compressed RAM caching.
- Persistent separate-app audio capture that follows supported multi-process application trees and reconnects after restarts.
- Format-aware microphone processing that respects PCM container width and valid-bit depth while feeding RNNoise its expected S16-scaled samples.
- Native Windows Raw Input save hotkey for background and fullscreen games, plus tray operation, startup-on-login, notifications, customizable UI themes (Graphite, Light, Glitten, Milate, Custom), and capture-aware background updates.

## Architecture

The default desktop host is Tauri 2, not Electron. A small resident Rust
controller owns recording, saves, settings, hotkeys, updates and native feedback.
Opening the interface creates a disposable UI-worker process with React and
WebView2; closing it removes the worker and browser tree without stopping capture.
Node and Electron are not part of the installed runtime. Feature-owned React
modules and Rust domain services are mapped in [architecture.md](docs/architecture.md).

```text
Video
DXGI Desktop Duplication (WGC fallback)
  -> unified D3D11 capture & encoder device
  -> direct on-tick target-FPS sampler
  -> native NVENC H.264 hardware encoding with 2.0s GOP & VBV buffering
  -> shared packet payload + cached NAL metadata
  -> rolling replay archive + bounded RAM fallback

Audio
WASAPI capture
  -> short PCM recovery ring
  -> live routing/mixing coordinator
  -> Media Foundation AAC
  -> rolling AAC archive + short PCM recovery window

Save
select packet ranges from the replay archive
  -> metadata-only MP4 plan
  -> bounded adaptive low-I/O-priority file writer
  -> final MP4
```

The normal save path does not re-encode video, rescan the full H.264 stream, or copy the entire clip into a second buffer. PCM-to-AAC encoding remains available as automatic recovery when live AAC coverage has a gap.

Startup follows a configure-before-arm sequence. Core capture starts first; optional app loopback workers and game detection begin after a short delay so opening Clipture does not launch every expensive subsystem at once.

The library reads saved clips and imported directories in place. Imported files are not duplicated, and renaming or deleting an imported card changes the original file. Thumbnail previews are generated in RAM and never stored beside the source video.

The Glitten library uses an editorial in-place 16:9 player with a scrollable related-clips rail. Selection remains in that rail, and the rail stays within the video-defined height while resizing so library content cannot stretch or letterbox the player. Standard themes retain their clip grid beneath the active player.

## Storage and Memory Use

The replay archive continuously writes encoded H.264 and AAC packets to managed rolling segments. A short hot window and packets waiting for persistence remain in RAM; if archive writes fail, Clipture automatically retains affected packets in RAM until persistence recovers.

Approximate archive storage for video is:

```text
video bytes ~= bitrate in Mb/s * clip seconds / 8
```

For example, two minutes at 80 Mb/s is approximately 1.2 GB of compressed video. The rolling archive trims expired segments automatically, and saving streams selected packet ranges without constructing another full-size video copy in RAM.

Save output is paced according to storage type, observed write service, and capture pressure added after the save begins. The measured storage service establishes a throughput floor, keeping SSD saves fast without allowing a large cached write burst to be deferred to file close.

## Requirements

- 64-bit Windows 10 or Windows 11.
- An NVIDIA GPU with NVENC H.264 support.
- An NVIDIA driver compatible with the NVENC API used by the build.
- Microsoft Edge WebView2 Runtime (the Windows installer handles a missing runtime).
- For development: Node.js/npm, Rust stable with the `x86_64-pc-windows-msvc`
  toolchain, CMake, and Visual Studio C++ build tools with a Windows SDK.

The controller and engine statically link the Visual C++ runtime; a separate
VC++ redistributable is not required by these executables.

## Build

Install dependencies and build the optimized Tauri application and native engine:

```powershell
npm.cmd ci
npm.cmd run build
```

Build only one side:

```powershell
npm.cmd run build:engine
npm.cmd run build:renderer
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
dist/renderer/
release/tauri-unpacked/clipture.exe
src-tauri/target/release/bundle/nsis/Clipture_<version>_x64-setup.exe
```

`build` and `pack:win` stage only five runtime files, excluding Cargo artifacts,
debug symbols and Electron. `dist:win` additionally creates the NSIS installer.
Local builds have updates disabled. Public releases require a production updater
key and signing configuration; the checked-in development key is deliberately
rejected by the release guard. No local test signature is a production signature.

## Run

Run the development server with hot reload:

```powershell
npm.cmd run dev
```

Run the already-built optimized Tauri application:

```powershell
npm.cmd run start:built
```

Build the engine, renderer and Tauri controller, then launch:

```powershell
npm.cmd start
```

`start:dev` aliases `dev`, including Vite hot reload. `start:built` exits its Node
launcher immediately; the native tray application remains. Exit a copy running
from `release/tauri-unpacked` before rebuilding that directory. Installed copies
use their own directory, but share the normal profile and single-instance identity.

For isolated UI testing without recording or touching the daily profile, first
exit any running Tauri instance (the profile override does not change its
single-instance key):

```powershell
$env:CLIPTURE_DATA_DIR = Join-Path (Get-Location) '.cache/manual-ui-profile'
$env:CLIPTURE_TEST_MODE = '1'
npm.cmd run start:built
```

The explicit profile disables changes to daily OS startup integration. Remove these environment overrides
before a normal launch. Automated checks and their limitations are recorded in
[the migration checkpoint](docs/migration/checkpoint.md).

Electron remains an explicitly named behavior reference: `dev:legacy`,
`build:legacy`, `start:legacy`, `pack:legacy`, and `dist:legacy`. Its source and
build dependencies are retained pending native keyboard/picker UX verification;
none are copied into the Tauri payload.

Development builds can temporarily use substantial CPU, disk, and memory and should not be used to judge installed-app startup performance.

The Customize settings tab includes Glitten and Milate themes. Their personal-use demo fonts are opened from their download pages rather than bundled with Clipture. After installing a font, return to Clipture or use Refresh font; the typeface is detected and applied without restarting the app.

Installed startup with `--hidden` opens Clipture in the tray while the native capture engine begins filling the replay buffer.

## Library Controls

- Click a thumbnail to open a clip.
- Click the video to play or pause and double-click it to toggle fullscreen.
- Press Space to play or pause. Playback resumes automatically after an unloaded seek range becomes available.
- Press Left or Right to seek five seconds. Hold either key for more than 300 ms to accelerate seeking.
- Imported Videos reads videos directly from selected folders. It does not copy them into Clipture storage.
- Rename and delete actions affect the underlying file for both saved and imported clips.

## Diagnostics

Application data and logs are stored under:

```text
%APPDATA%\Clipture\data
```

Useful files include `settings.json`, `clips.json`, and diagnostic exports.
Existing `save-timing.log` and Electron `updates.log` files may remain from the
legacy host; the Tauri diagnostics view/export is the current reporting surface.

Run a native engine smoke test with:

```powershell
'{"id":1,"type":"configure","fps":30,"bitrateMbps":40,"clipLengthSeconds":30,"monitorId":"primary"}' | .\build\engine\Release\clipture_engine.exe
```

Diagnostics reports the requested and active capture backends, measured display refresh rate, fresh-frame rate, repeats, separately queued fresh/repeated encoder ticks, queue drops, and encoder-stage latency. If DXGI Desktop Duplication cannot start or recover, automatic mode quarantines it for that monitor and falls back to Windows.Graphics.Capture. A forced WGC failure such as `CreateForMonitor failed: HRESULT 0x80070424` is reported as degraded capture instead of silently treating the engine as armed.
