import { useCallback, useEffect, useState } from 'react';
import type { OldInstallation, OldInstallationRemoval } from '../../../shared/types';
import { clipture } from '../../platform';

export type OldInstallationPhase = 'idle' | 'waiting' | OldInstallationRemoval['outcome'];

// Asked once per Settings visit. A removal stays shown as done until the
// next visit, when the host reports no old copy and the row is gone.
export function useOldInstallation() {
  const [installation, setInstallation] = useState<OldInstallation | null>(null);
  const [phase, setPhase] = useState<OldInstallationPhase>('idle');
  const [message, setMessage] = useState('');

  useEffect(() => {
    let current = true;
    void clipture.getOldInstallation()
      .then((found) => { if (current) setInstallation(found); })
      .catch(() => { if (current) setInstallation(null); });
    return () => { current = false; };
  }, []);

  const remove = useCallback(() => {
    setPhase('waiting');
    setMessage('');
    void clipture.removeOldInstallation()
      .then((result) => {
        setPhase(result.outcome);
        setMessage(result.message ?? '');
      })
      .catch((reason) => {
        setPhase('failed');
        setMessage(reason instanceof Error ? reason.message : String(reason));
      });
  }, []);

  return { installation, phase, message, remove };
}
