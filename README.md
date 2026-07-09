# Clipture

Clipture is a Windows clipping app split into:

- Electron + TypeScript for the library, settings, diagnostics, and tray-facing UI.
- A native C++20 engine for capture, encoding, packet buffers, audio, and muxing.

The intended v1 capture path is:

```text
Windows.Graphics.Capture
-> ID3D11Texture2D
-> direct NVENC through NVIDIA Video Codec SDK
-> encoded packet ring buffer
-> MP4 mux on hotkey
```

## Current Implementation

- Electron/React settings, diagnostics, and library UI.
- Native C++ engine process launched by Electron.
- JSON-lines IPC between Electron and the native engine.
- DXGI NVIDIA GPU detection.
- Runtime NVENC DLL/API availability detection.
- NVENC-first diagnostics surfaced in the UI.
- Customizable global hotkey registration through Electron.
- Settings pushed into the native engine at startup and after edits.
- Native D3D11 initialization on the NVIDIA adapter before the engine is considered armed.
- Windows.Graphics.Capture primary-monitor session setup using `CreateForMonitor`.
- Free-threaded WGC frame pool wiring with captured-frame diagnostics.
- Packet-level ring buffer primitives for encoded video/audio packets.
- Close-to-tray behavior and silent startup setting wiring.

Direct NVENC encode sessions, WASAPI process-tree audio capture, and MP4 muxing are the remaining native-engine implementation slice.

If diagnostics reports `CreateForMonitor failed: HRESULT 0x80070424`, Windows rejected monitor capture creation before frames could arrive. The app surfaces that as degraded instead of silently pretending capture is active.

## Build

```powershell
npm.cmd install
npm.cmd run build
```

## Make the Windows Installer

```powershell
npm.cmd run dist:win
```

The installer is written to:

```text
release/Clipture-Setup-0.1.0.exe
```

For an unpacked app folder without an installer:

```powershell
npm.cmd run pack:win
```

The native engine is built to:

```text
build/engine/Release/clipture_engine.exe
```

The Electron output is built to:

```text
dist/
```

## Run

```powershell
npm.cmd start
```

After installing, run Clipture from the Start Menu or desktop shortcut. Startup mode launches with `--hidden`, so it opens into the tray and starts the native engine without showing the main window.

For development:

```powershell
npm.cmd run dev
```

## Engine Smoke Test

```powershell
'{"id":1,"type":"getDiagnostics"}' | .\build\engine\Release\clipture_engine.exe
```
