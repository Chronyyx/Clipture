import { useState } from 'react';
import { inviteServer, type TelemetryStatus } from '../../../shared/telemetry';
import { describeRecord } from './describeRecord';
import { useTelemetry } from './useTelemetry';
import './telemetry.css';

const SENT = 'Recording health only: frame rates, dropped frames and why, errors, saves, your PC\'s hardware and recording settings. Never clips, audio, file names, folders or device names.';

function when(iso: string | null) {
  return iso ? new Date(iso).toLocaleString() : 'not yet';
}

function kilobytes(bytes: number) {
  return bytes < 1024 ? `${bytes} B` : `${(bytes / 1024).toFixed(0)} KB`;
}

function JoinForm({ busy, onEnroll }: { busy: boolean; onEnroll: (code: string) => Promise<boolean> }) {
  const [code, setCode] = useState('');
  const [confirming, setConfirming] = useState(false);
  const server = inviteServer(code);
  if (confirming && server) {
    return (
      <div className='telemetry-confirm'>
        <p>Send recording health to <strong>{server}</strong>? Only do this if you trust whoever runs it.</p>
        <div className='telemetry-actions'>
          <button className='primary' type='button' disabled={busy} onClick={() => void onEnroll(code).then((ok) => { if (ok) setCode(''); setConfirming(false); })}>
            {busy ? 'Connecting…' : 'Start sending'}
          </button>
          <button className='secondary-button' type='button' disabled={busy} onClick={() => setConfirming(false)}>Cancel</button>
        </div>
      </div>
    );
  }
  return (
    <form className='telemetry-join' onSubmit={(event) => { event.preventDefault(); if (server) setConfirming(true); }}>
      <input aria-label='Telemetry invite code' placeholder='Paste an invite code' value={code} spellCheck={false}
        autoComplete='off' onChange={(event) => setCode(event.target.value)} />
      <button className='secondary-button' type='submit' disabled={!server}>Join</button>
      {code.trim() !== '' && !server && <span className='telemetry-hint'>That is not a Clipture telemetry invite code.</span>}
    </form>
  );
}

function ActiveDetails({ status }: { status: TelemetryStatus }) {
  return (
    <dl className='telemetry-facts'>
      <dt>Server</dt><dd>{status.server}</dd>
      <dt>Joined as</dt><dd>{status.friendName}</dd>
      <dt>Since</dt><dd>{when(status.enrolledAt)}</dd>
      <dt>Last upload</dt><dd>{status.lastUploadAt ? `${when(status.lastUploadAt)} · ${status.lastUploadRecords} records` : 'not yet'}</dd>
      <dt>Waiting to send</dt><dd>{kilobytes(status.pendingBytes)}</dd>
    </dl>
  );
}

/** Telemetry to a friend's server (ADR 0035). Off unless an invite is used. */
export function TelemetryPanel() {
  const { status, busy, error, batch, enroll, stop, toggleBatch } = useTelemetry();
  const [confirmStop, setConfirmStop] = useState(false);
  if (!status?.supported) return null;
  return (
    <section className='telemetry-panel' aria-labelledby='telemetry-heading'>
      <header>
        <h2 id='telemetry-heading'>Telemetry</h2>
        <span className={`telemetry-state ${status.state}`}>{status.state === 'active' ? 'Sending' : status.state === 'revoked' ? 'Refused by server' : 'Off'}</span>
      </header>
      {status.state === 'off' && (
        <>
          <p>Off: Clipture sends nothing. If a friend gave you an invite code, you can send recording health to their server.</p>
          <p className='telemetry-muted'>{SENT}</p>
          <JoinForm busy={busy} onEnroll={enroll} />
        </>
      )}
      {status.state === 'active' && (
        <>
          <ActiveDetails status={status} />
          <p className='telemetry-muted'>{SENT}</p>
          <div className='telemetry-actions'>
            <button className='secondary-button' type='button' onClick={() => void toggleBatch()}>{batch ? 'Hide what was sent' : 'Show what\'s sent'}</button>
            {confirmStop ? (
              <>
                <button className='secondary-button danger' type='button' disabled={busy} onClick={() => void stop().then(() => setConfirmStop(false))}>Stop and delete the key</button>
                <button className='secondary-button' type='button' onClick={() => setConfirmStop(false)}>Keep sending</button>
              </>
            ) : (
              <button className='secondary-button' type='button' onClick={() => setConfirmStop(true)}>Stop telemetry</button>
            )}
          </div>
        </>
      )}
      {status.state === 'revoked' && (
        <>
          <p>{status.server} no longer accepts this PC, so nothing is sent. Paste a new invite to continue, or remove it.</p>
          <JoinForm busy={busy} onEnroll={enroll} />
          <div className='telemetry-actions'>
            <button className='secondary-button' type='button' disabled={busy} onClick={() => void stop()}>Remove</button>
          </div>
        </>
      )}
      {(error || status.lastError) && <p className='telemetry-error' role='status'>{error || status.lastError}</p>}
      {batch && (batch.length === 0 ? <p className='telemetry-muted'>Nothing has been uploaded yet.</p> : (
        <div className='telemetry-batch' aria-label='Last batch sent'>
          <ol>{batch.map((record, index) => <li key={index}>{describeRecord(record)}</li>)}</ol>
          <details>
            <summary>Exactly as sent (JSON)</summary>
            <pre>{batch.map((record) => JSON.stringify(record)).join('\n')}</pre>
          </details>
        </div>
      ))}
    </section>
  );
}
