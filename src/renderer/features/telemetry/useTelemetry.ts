import { useCallback, useEffect, useState } from 'react';
import type { TelemetryRecord, TelemetryStatus } from '../../../shared/telemetry';
import { telemetry } from '../../platform';

const POLL_MS = 5000;

/** Telemetry status while the panel is open, and its three actions. */
export function useTelemetry() {
  const [status, setStatus] = useState<TelemetryStatus | null>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');
  const [batch, setBatch] = useState<TelemetryRecord[] | null>(null);

  useEffect(() => {
    let active = true;
    const refresh = () => void telemetry.getStatus().then((next) => { if (active) setStatus(next); }).catch(() => {});
    refresh();
    const timer = window.setInterval(refresh, POLL_MS);
    return () => {
      active = false;
      window.clearInterval(timer);
    };
  }, []);

  const run = useCallback(async (action: () => Promise<TelemetryStatus>) => {
    setBusy(true);
    setError('');
    try {
      setStatus(await action());
      setBatch(null);
      return true;
    } catch (failure) {
      setError(failure instanceof Error ? failure.message : String(failure));
      return false;
    } finally {
      setBusy(false);
    }
  }, []);

  const enroll = useCallback((code: string) => run(() => telemetry.enroll(code)), [run]);
  const stop = useCallback(() => run(() => telemetry.stop()), [run]);
  const toggleBatch = useCallback(async () => {
    if (batch) {
      setBatch(null);
      return;
    }
    try {
      setBatch(await telemetry.lastBatch());
    } catch (failure) {
      setError(failure instanceof Error ? failure.message : String(failure));
    }
  }, [batch]);

  return { status, busy, error, batch, enroll, stop, toggleBatch };
}
