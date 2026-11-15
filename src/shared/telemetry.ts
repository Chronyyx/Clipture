/**
 * Invite-only telemetry (docs/adr/0035-invite-only-telemetry.md). A separate,
 * additive client beside the v1 `CliptureApi` facade, like sharing: only the
 * Tauri host implements it; other hosts report `supported: false`.
 */
export type TelemetryState = "off" | "active" | "revoked";

export interface TelemetryStatus {
  supported: boolean;
  /** `off`: nothing is recorded or sent. `revoked`: the server refused this PC. */
  state: TelemetryState;
  server: string | null;
  /** The name the server's owner gave this invite. */
  friendName: string | null;
  deviceId: string | null;
  enrolledAt: string | null;
  /** Recorded but not yet uploaded. */
  pendingBytes: number;
  lastUploadAt: string | null;
  lastUploadRecords: number;
  lastError: string | null;
}

/** One record exactly as it was uploaded. */
export type TelemetryRecord = Record<string, unknown>;

export interface TelemetryApi {
  getStatus(): Promise<TelemetryStatus>;
  /** Redeems an invite code: creates this PC's key and registers it. */
  enroll(code: string): Promise<TelemetryStatus>;
  /** Stops telemetry and deletes the key and anything unsent. */
  stop(): Promise<TelemetryStatus>;
  /** The records of the last batch the server accepted. */
  lastBatch(): Promise<TelemetryRecord[]>;
}

/** The server an invite code points to, for the confirmation step; null
 * when the text is not an invite code. The host validates it again. */
export function inviteServer(code: string): string | null {
  const parts = code.trim().split(".");
  if (parts.length !== 3 || parts[0] !== "ctel1") return null;
  try {
    const base64 = parts[1].replace(/-/g, "+").replace(/_/g, "/");
    const server = atob(base64 + "=".repeat((4 - (base64.length % 4)) % 4));
    return /^https?:\/\/[^\s]+$/.test(server) ? server : null;
  } catch {
    return null;
  }
}
