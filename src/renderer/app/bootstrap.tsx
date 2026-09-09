import { StrictMode, useEffect, type ReactNode } from 'react';
import { createRoot } from 'react-dom/client';
import { clipture, revealMainWindow } from '../platform';
import { applyCachedUiTheme, cacheAndApplyUiTheme } from '../theme';
import { App } from './App';

function ReadyWindow({ children }: { children: ReactNode }) {
  useEffect(() => {
    // Effects run after the DOM commit. A task boundary also lets layout settle;
    // don't await rAF, which can be suspended while a native window is hidden.
    const timer = window.setTimeout(() => {
      void revealMainWindow().catch(error => console.error('Could not show Clipture', error));
    }, 0);
    return () => window.clearTimeout(timer);
  }, []);
  return children;
}

export async function mountMainApp(): Promise<void> {
  applyCachedUiTheme();
  const root = createRoot(document.getElementById('root') as HTMLElement);
  let timeout: number | undefined;
  try {
    // Saved appearance is authoritative, not potentially stale WebView storage.
    const initialSettings = await Promise.race([
      clipture.getSettings(),
      new Promise<never>((_, reject) => {
        timeout = window.setTimeout(() => reject(new Error('Settings did not respond. Please retry.')), 10_000);
      })
    ]);
    cacheAndApplyUiTheme(initialSettings);
    root.render(<StrictMode><ReadyWindow><App initialSettings={initialSettings} /></ReadyWindow></StrictMode>);
  } catch (error) {
    root.render(<ReadyWindow><main className='startup-error'>
      <h1>Could not open Clipture</h1>
      <p>{error instanceof Error ? error.message : 'Could not load settings.'}</p>
      <button className='primary' onClick={() => window.location.reload()}>Retry</button>
    </main></ReadyWindow>);
  } finally {
    window.clearTimeout(timeout);
  }
}
