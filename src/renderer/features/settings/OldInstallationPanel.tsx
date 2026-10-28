import { SettingRow } from './SettingsSection';
import { useOldInstallation, type OldInstallationPhase } from './useOldInstallation';

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
