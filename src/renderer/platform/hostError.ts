export type HostKind = 'tauri' | 'electron' | 'mock';

function detailFrom(error: unknown) {
  if (error instanceof Error) return error.message;
  if (typeof error === 'string') return error;
  try {
    return JSON.stringify(error);
  } catch {
    return String(error);
  }
}

export class HostCapabilityError extends Error {
  readonly cause: unknown;
  readonly retryable: boolean;
  /** The host's own reason, without the capability and route, for people. */
  readonly detail: string;

  constructor(
    readonly host: HostKind,
    readonly capability: string,
    error: unknown,
    readonly command?: string
  ) {
    const hostLabel = host === 'tauri' ? 'Tauri' : host === 'electron' ? 'Electron' : 'Browser mock';
    const route = command ? ' through ' + command : '';
    const busy = detailFrom(error) === 'UI host is busy; retry shortly';
    super(
      busy ? 'Clipture is busy preparing media. Please try again shortly.' :
        hostLabel + ' capability ' + capability + ' failed' + route + ': ' + detailFrom(error)
    );
    this.name = 'HostCapabilityError';
    this.cause = error;
    this.retryable = busy;
    this.detail = busy ? this.message : detailFrom(error);
  }
}

/** A message fit to show: the host's reason as a sentence, not the
 * diagnostic form with host, capability and command names. */
export function userMessage(error: unknown, fallback: string): string {
  const text = error instanceof HostCapabilityError ? error.detail : error instanceof Error ? error.message : '';
  const trimmed = text.trim();
  if (!trimmed) return fallback;
  const sentence = trimmed.charAt(0).toUpperCase() + trimmed.slice(1);
  return /[.!?]$/.test(sentence) ? sentence : sentence + '.';
}

export function isHostBusyError(error: unknown): boolean {
  return error instanceof HostCapabilityError && error.retryable;
}
