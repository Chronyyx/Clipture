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
