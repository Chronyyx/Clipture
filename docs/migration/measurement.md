# Migration measurement

Two read-only scripts provide repeatable evidence during the port.

## Process footprint

Launch the build being measured yourself, note its controller PID, and run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/migration/measure-process-footprint.ps1 -RootProcessId 1234 -SampleCount 5 -IntervalMilliseconds 1000
```

If there is exactly one packaged instance, `-RootProcessName Clipture` can be used instead. Prefer a PID for development Electron because other Electron applications may be running.

The script emits JSON to stdout and never starts, stops, or alters a process. `privateBytes` is the primary memory comparison. Summed working sets can double-count shared pages. Record at least these states:

- tray idle after startup settles;
- main UI open and library idle;
- playback active;
- immediately after UI close;
- tray idle after 100 open/close cycles;
- save in progress.

The tray-idle gate is zero Electron, Node, WebView2, or FFmpeg descendants. A capture engine descendant is expected.

## Artifact inventory

Run from the repository root after building or packaging:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/migration/inventory-artifacts.ps1
```

It inventories existing build/package roots and hashes only same-sized candidates above the duplicate threshold. It emits JSON and makes no changes. To focus a package:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/migration/inventory-artifacts.ps1 -RootPath release\win-unpacked -MinimumDuplicateBytes 65536
```

The package gate is one `clipture_engine.exe` and one FFmpeg executable. Exact-hash duplicate groups should be reviewed; not every duplicate is removable because signed installers and archives may intentionally embed content.
