import { getCurrentWindow } from '@tauri-apps/api/window';
import { hostKind } from './client';

/** UI-worker presentation only; never starts or owns recording services.
 * Electron's legacy window and browser previews are already visible. */
export async function revealMainWindow(): Promise<void> {
  if (hostKind !== 'tauri') return;
  const window = getCurrentWindow();
  await window.show();
  await window.setFocus();
}

/** Reports whether the window fills the screen (maximized or fullscreen),
 * now and whenever that changes; returns a function that stops watching.
 * Browser previews and Electron's framed window always count as filling. */
export function watchWindowFill(onChange: (fills: boolean) => void): () => void {
  if (hostKind !== 'tauri') {
    onChange(true);
    return () => undefined;
  }
  const window = getCurrentWindow();
  let stopped = false;
  let last: boolean | undefined;
  const check = async () => {
    const fills = (await window.isMaximized()) || (await window.isFullscreen());
    if (!stopped && fills !== last) {
      last = fills;
      onChange(fills);
    }
  };
  void check().catch(() => onChange(true));
  const listening = window.onResized(() => void check().catch(() => undefined));
  return () => {
    stopped = true;
    void listening.then((unlisten) => unlisten());
  };
}

export type ResizeEdge = 'North' | 'South' | 'East' | 'West' | 'NorthEast' | 'NorthWest' | 'SouthEast' | 'SouthWest';

/** Starts the system's own resize from `edge`, for windows whose drawn edge
 * is not the window's rectangle. Does nothing outside the Tauri host. */
export async function startWindowResize(edge: ResizeEdge): Promise<void> {
  if (hostKind !== 'tauri') return;
  await getCurrentWindow().startResizeDragging(edge);
}

export type WindowAction = 'close' | 'minimize' | 'maximize';

/** Whether the UI may draw its own window controls: the Tauri window drops
 * its system frame for themes that do (browser previews show them too).
 * Electron's legacy window keeps its frame. */
export const drawsWindowControls = hostKind !== 'electron';

/** Closing goes through the window's normal close request, so it hides to
 * the tray exactly as the system button did. */
export async function controlMainWindow(action: WindowAction): Promise<void> {
  if (hostKind !== 'tauri') return;
  const window = getCurrentWindow();
  if (action === 'close') await window.close();
  else if (action === 'minimize') await window.minimize();
  else await window.toggleMaximize();
}
