# Clipture Architecture

Clipture is split into two processes:

- Electron + TypeScript owns the UI, tray menu, settings, diagnostics, and clip library.
- `clipture_engine.exe` owns screen capture, NVENC encoding, audio capture, packet buffers, and muxing.

The process boundary is JSON-lines over stdio. Electron sends commands such as `getDiagnostics` and `saveClip`; the engine returns JSON payloads.

The intended capture path is:

```text
Windows.Graphics.Capture
-> ID3D11Texture2D
-> direct NVENC through NVIDIA Video Codec SDK
-> encoded packet ring buffer
-> MP4 mux on hotkey
```

The current engine implements the NVENC availability probe, D3D11 adapter/device setup, WGC primary-monitor session creation, frame-arrival counting, and packet-ring-buffer primitives. It intentionally treats missing NVENC or failed WGC startup as degraded because the MVP is NVIDIA/NVENC-first.
