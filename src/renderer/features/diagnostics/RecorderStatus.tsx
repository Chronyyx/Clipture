import type { EngineDiagnostics } from '../../../shared/types';
import { SpookyRecorderPet } from '../../shared/halloween';
import { CafeRecorderPet } from '../../shared/maid-cafe';

interface RecorderStatusProps {
  diagnostics: EngineDiagnostics;
  hasDiagnostics: boolean;
  diagnosticsError?: string;
  clipLengthSeconds: number;
}

type State = 'live' | 'warning' | 'waiting' | 'off';

function recorderState({ diagnostics, hasDiagnostics, diagnosticsError }: RecorderStatusProps): State {
  if (!hasDiagnostics) return diagnosticsError ? 'off' : 'waiting';
  if (diagnostics.activeEncoder === 'Unavailable') return 'off';
  return diagnostics.degraded || diagnosticsError ? 'warning' : 'live';
}

function recorderDetail(state: State, clipLengthSeconds: number, diagnosticsError?: string): string {
  if (state === 'live' || state === 'warning') return 'Last ' + clipLengthSeconds + ' s ready to save';
  return diagnosticsError ? 'Waiting for the capture engine' : 'Connecting to the capture engine';
}

const headline: Record<State, string> = {
  live: 'Recording',
  warning: 'Recording with issues',
  waiting: 'Starting recorder…',
  off: 'Not recording'
};

// The sidebar tally: a camera-style "live" light plus the reach of one save.
export function RecorderStatus(props: RecorderStatusProps) {
  const { diagnostics, hasDiagnostics, diagnosticsError, clipLengthSeconds } = props;
  const state = recorderState(props);
  const detail = recorderDetail(state, clipLengthSeconds, diagnosticsError);

  return (
    <div className={'recorder-status ' + state} title={diagnosticsError} role='status' aria-live='polite'>
      <CafeRecorderPet state={state} />
      <SpookyRecorderPet state={state} />
      <div className='recorder-status-head'>
        <span className='recorder-tally' aria-hidden='true' />
        <strong>{headline[state]}</strong>
      </div>
      <span className='recorder-status-detail'>{detail}</span>
      <span className='recorder-buffer' aria-hidden='true'>
        <span className='recorder-buffer-track' />
        <span className='recorder-buffer-labels'><span>−{clipLengthSeconds} s</span><span>now</span></span>
      </span>
      {hasDiagnostics && (
        <span className='recorder-readout'>
          <span>{diagnostics.activeEncoder}{diagnostics.encoderMode ? ', ' + diagnostics.encoderMode : ''}</span>
          <span>{diagnostics.gpu}</span>
        </span>
      )}
      {diagnosticsError && hasDiagnostics && <span className='recorder-status-detail warning-text'>Showing last known details</span>}
    </div>
  );
}
