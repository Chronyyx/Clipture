# Clipture recorder branding & icons

- `clipture-logo.png`: Transparent 1254px master asset rendered from the Clipture hardware recorder design.
- `clipture-logo-ui.png`: 128px UI and favicon asset (~15 KB) optimized for renderer display without bundle bloat.
- `icon.ico`: Windows application, tray, shortcut, and installer icon containing 16, 24, 32, 48, 64, and 256px multi-resolution layers.
- `clipture-logo-source.svg`: Original vector SVG asset retained as editable design source.
- `svgviewer-output.svg`: Legacy logo retained for historical reference.

## Asset details

The production icon features true alpha transparency around the metallic bevel, matte dial, orange indicator, and vent geometry. Small mipmap sizes (16–32px) emphasize high-contrast hardware contours for crisp Windows taskbar and system tray visibility.

## Regenerating platform icons

```powershell
npm run tauri -- icon assets/clipture-logo.png --output .cache/clipture-icons
Copy-Item .cache/clipture-icons/icon.ico assets/icon.ico
Copy-Item .cache/clipture-icons/128x128.png assets/clipture-logo-ui.png
```

The installed executable embeds its icon: rebuild and reinstall to update Windows Explorer and taskbar caches.
