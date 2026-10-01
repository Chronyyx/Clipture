import { useCallback, useEffect, useState } from 'react';
import type { ClipRepairStatus } from '../../../shared/types';
import { clipture } from '../../platform';

const idle: ClipRepairStatus = { phase: 'idle', checked: 0, total: 0, needsRepair: 0, needsRepairBytes: 0, repaired: 0, failed: 0 };

// The job runs in the host; this view only polls while it is active, so it
// can be closed and reopened mid-repair without losing progress.
export function useClipRepair() {
  const [status, setStatus] = useState<ClipRepairStatus>(idle);
  const [error, setError] = useState('');
  const running = status.phase === 'checking' || status.phase === 'repairing';

  const run = useCallback((request: () => Promise<ClipRepairStatus>) => {
    setError('');
    void request().then(setStatus).catch((reason) => setError(reason instanceof Error ? reason.message : String(reason)));
  }, []);

  useEffect(() => { run(() => clipture.getClipRepairStatus()); }, [run]);

  useEffect(() => {
    if (!running) return;
    const timer = window.setInterval(() => run(() => clipture.getClipRepairStatus()), 1000);
    return () => window.clearInterval(timer);
  }, [running, run]);

  return {
    status,
    error,
    running,
    check: () => run(() => clipture.checkClipLayouts()),
    fix: () => run(() => clipture.fixClipLayouts())
  };
}
