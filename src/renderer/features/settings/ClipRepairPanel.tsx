import type { ClipRepairStatus } from '../../../shared/types';
import { SettingRow } from './SettingsSection';
import { useClipRepair } from './useClipRepair';

function plural(count: number, noun: string) {
  return count + ' ' + noun + (count === 1 ? '' : 's');
}

function describe(status: ClipRepairStatus) {
  switch (status.phase) {
    case 'checking':
      return 'Checking ' + status.checked + ' of ' + status.total + ' clips…';
    case 'checked':
      return status.needsRepair === 0
        ? 'All ' + plural(status.total, 'clip') + ' play smoothly.'
        : plural(status.needsRepair, 'clip') + ' (' + (status.needsRepairBytes / 1e9).toFixed(1) + ' GB) can start slowly, show a black screen, or take up more space than they need. ' +
          'Fixing rewrites how each file is stored, without re-encoding, so quality is unchanged.';
    case 'repairing':
      return 'Fixing ' + Math.min(status.repaired + status.failed + 1, status.needsRepair) + ' of ' + status.needsRepair +
        (status.currentTitle ? ': ' + status.currentTitle : '') + '. You can keep using Clipture.';
    case 'done':
      return 'Fixed ' + plural(status.repaired, 'clip') + '.' +
        (status.failed ? ' ' + plural(status.failed, 'clip') + ' could not be fixed and were left unchanged.' : '');
    default:
      return status.message ?? 'Some clips can start slowly, show a black screen, or take up to twice the space they need. Checking changes nothing.';
  }
}

function progress(status: ClipRepairStatus) {
  if (status.phase === 'checking') return status.total ? status.checked / status.total : 0;
  if (status.phase === 'repairing') return status.needsRepair ? (status.repaired + status.failed) / status.needsRepair : 0;
  return undefined;
}

export function ClipRepairPanel() {
  const { status, error, running, check, fix } = useClipRepair();
  const canFix = status.phase === 'checked' && status.needsRepair > 0;
  const fraction = progress(status);

  return (
    <SettingRow
      title='Fix clips'
      description={
        <>
          <span aria-live='polite'>{error || describe(status)}</span>
          {status.phase === 'done' && status.message && <span className='setting-row-note'>{status.message}</span>}
          {fraction !== undefined && (
            <span className='progress-track' role='progressbar' aria-valuemin={0} aria-valuemax={100} aria-valuenow={Math.round(fraction * 100)}>
              <span className='progress-fill' style={{ transform: 'scaleX(' + fraction + ')' }} />
            </span>
          )}
        </>
      }
      control={(id) => (
        <span className='setting-row-buttons'>
          <button id={id} className='secondary-button' type='button' disabled={running} onClick={check}>
            {status.phase === 'idle' ? 'Check clips' : 'Check again'}
          </button>
          {canFix && <button className='primary' type='button' onClick={fix}>Fix {plural(status.needsRepair, 'clip')}</button>}
        </span>
      )}
    />
  );
}
