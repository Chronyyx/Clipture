import { useEffect, useRef, useState } from 'react';
import { clipture } from '../../platform';
import { defaultUpdateState } from './TitlebarUpdateControls';

export function useUpdates(onNotice: (message: string, durationMs?: number) => void) {
  const [updateState, setUpdateState] = useState(defaultUpdateState);
  const revision = useRef(0);
  useEffect(() => {
    let active = true;
    const request = ++revision.current;
    const unsubscribe = clipture.onUpdateStateChanged(next => {
      if (active) { ++revision.current; setUpdateState(next); }
    });
    void clipture.getUpdateState().then(next => {
      if (active && request === revision.current) setUpdateState(next);
    }).catch(console.warn);
    return () => { active = false; ++revision.current; unsubscribe(); };
  }, []);
  async function run(action: () => Promise<unknown>) {
    try { await action(); }
    catch (error) { onNotice(error instanceof Error ? error.message : 'Could not update Clipture.', 6000); }
  }
  return { updateState,
    checkForUpdatesNow: () => run(async () => {
      const request = ++revision.current;
      const next = await clipture.checkForUpdates();
      if (request === revision.current) setUpdateState(next);
    }),
    downloadUpdate: () => run(() => clipture.downloadUpdate()),
    installUpdate: () => run(() => clipture.installUpdate()) };
}
