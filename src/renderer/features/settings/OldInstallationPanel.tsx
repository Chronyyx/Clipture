import { useCallback, useEffect, useState } from 'react';
import type { OldInstallation, OldInstallationRemoval } from '../../../shared/types';
import { clipture } from '../../platform';
import { SettingRow } from './SettingsSection';

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

function describe(phase: OldInstallationPhase, folder: string, message: string) {
  switch (phase) {
    case 'waiting':
      return 'Confirm the Windows prompt to remove the old copy.';
    case 'removed':
      return 'Removed the old copy. Updates keep installing from your user folder.';
    case 'declined':
      return 'Not removed. The Windows prompt was declined, and you can try again anytime.';
    case 'failed':
      return 'Not removed. ' + (message || 'Something kept the old copy in place.');
    default:
      return 'Clipture runs from your user folder now, so updates need no administrator prompts. ' + folder + ' is unused.';
  }
}

// Shown only while a superseded Program Files copy exists, and only here:
// no banner, badge or reminder. Pressing the button is the consent, so
// Windows' own prompt follows directly.
export function OldInstallationPanel() {
  const { installation, phase, message, remove } = useOldInstallation();
  if (!installation) return null;
  const offered = phase === 'idle' || phase === 'declined' || phase === 'failed';

  return (
    <SettingRow
      title='Old copy in Program Files'
      description={
        <>
          <span aria-live='polite'>{describe(phase, installation.folder, message)}</span>
          {offered && <span className='setting-row-note'>Windows asks for administrator permission once.</span>}
        </>
      }
      control={(id) => phase === 'removed' ? null : (
        <button id={id} className='secondary-button' type='button' disabled={phase === 'waiting'} onClick={remove}>
          {phase === 'waiting' ? 'Waiting for Windows…' : phase === 'idle' ? 'Remove old copy' : 'Try again'}
        </button>
      )}
    />
  );
}
