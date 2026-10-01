// Keep host validation and the contract fixture in sync with these choices.
export const CAPTURE_FPS_OPTIONS = [24, 30, 60, 120] as const;
export type CaptureFps = typeof CAPTURE_FPS_OPTIONS[number];

export function normalizeCaptureFps(value: unknown): CaptureFps {
  const fps = Number(value);
  return CAPTURE_FPS_OPTIONS.includes(fps as CaptureFps) ? fps as CaptureFps : 30;
}
