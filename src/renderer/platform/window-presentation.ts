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
