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

  constructor(
    readonly host: HostKind,
    readonly capability: string,
    error: unknown,
    readonly command?: string
  ) {
    const hostLabel = host === 'tauri' ? 'Tauri' : host === 'electron' ? 'Electron' : 'Browser mock';
    const route = command ? ' through ' + command : '';
    super(
      hostLabel + ' capability ' + capability + ' failed' + route + ': ' + detailFrom(error) + '. ' +
      'Check that the matching desktop command is implemented and registered.'
    );
    this.name = 'HostCapabilityError';
    this.cause = error;
  }
}
