import type { TelemetryRecord } from '../../../shared/telemetry';

const reasonNames: Record<string, string> = {
  captureQueueOverflow: 'capture queue full',
  captureSlotExhaustion: 'capture slots ran out',
  encoderBackpressure: 'encoder behind',
  schedulerDeadlineMisses: 'scheduler late'
};

const num = (value: unknown) => (typeof value === 'number' ? value : 0);
const fps = (value: unknown) => `${Math.round(num(value))} fps`;
const clock = (iso: unknown) => (typeof iso === 'string' ? new Date(iso).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' }) : '');

function reasons(value: unknown) {
  if (!value || typeof value !== 'object') return '';
  const parts = Object.entries(value as Record<string, unknown>)
    .filter(([, count]) => num(count) > 0)
    .map(([name, count]) => `${reasonNames[name] ?? name} ${num(count)}`);
  return parts.length ? ` (${parts.join(', ')})` : '';
}

/** One plain sentence per record, for "Show what's sent". */
export function describeRecord(record: TelemetryRecord): string {
  const r = record as Record<string, unknown>;
  const at = clock(r.t);
  switch (r.type) {
    case 'minute': {
      const dropped = num(r.dropped);
      const drops = dropped ? `${dropped} frames dropped${reasons(r.reasons)}` : 'no dropped frames';
      return `${clock(r.minute)} — recorded ${fps(r.outputFpsMean)} (lowest ${fps(r.outputFpsMin)}) while the game ran at ${fps(r.desktopFpsMean)}; ${drops}.`;
    }
    case 'incident': {
      const samples = Array.isArray(r.samples) ? r.samples.length : 0;
      const triggers = Array.isArray(r.triggers) ? r.triggers.join(', ') : '';
      return `${clock(r.at)} — lag detected (${triggers}): ${samples} two-second samples around it.`;
    }
    case 'event': {
      const message = typeof r.message === 'string' ? `: ${r.message}` : '';
      const level = r.level === 'error' ? 'Error' : r.level === 'warn' ? 'Warning' : 'Event';
      return `${at} — ${level} ${String(r.code)}${message}`;
    }
    case 'session_start':
      return `${at} — recording started (${String(r.backend)}, ${String(r.encoder)}, ${String(r.outputResolution)}, target ${fps(r.targetFps)}).`;
    case 'session_end':
      return `${at} — recording stopped after ${num(r.minutes)} min, ${num(r.dropped)} frames dropped.`;
    case 'run_start':
      return `${at} — Clipture ${String(r.version)} started on ${String(r.gpu) || 'an unknown GPU'} (driver ${String(r.gpuDriver) || '?'}), ${String(r.cpu)}, with your recording settings.`;
    case 'run_end':
      return `${at} — Clipture closed (${String(r.reason)}).`;
    case 'gap':
      return `${at} — records ${num(r.fromSeq)}–${num(r.toSeq)} were not sent (${String(r.reason)}).`;
    default:
      return `${at} — ${String(r.type)}`;
  }
}
