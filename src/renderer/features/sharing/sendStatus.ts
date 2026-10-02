import type { FriendPresence, SharedClip } from "../../../shared/sharing";
import { formatBytes } from "./sharingFormat";
import { minutesLeft, windowClosed } from "./useNow";

/** `working`: something is happening now. `waiting`: on the friend.
 * `done` (only once they saved a verified copy), `declined` and `ended`
 * (their time ran out) are endings; `trouble` needs nothing from the user
 * but explains a stall. */
export type SendTone = "working" | "waiting" | "done" | "declined" | "ended" | "trouble";

export interface SendStatus {
  tone: SendTone;
  headline: string;
  detail: string;
  /** 0 to 1 when bytes are known; `null` for a step with no measure. */
  progress: number | null;
}

const percent = (fraction: number) => `${Math.floor(fraction * 100)}%`;

/** What a clip you sent is doing, from your side, in plain words. */
export function describeSend(
  clip: SharedClip | undefined,
  presence: FriendPresence | undefined,
  name: string,
  now = Date.now()
): SendStatus {
  if (!clip) {
    return { tone: "working", headline: "Getting it ready", detail: `Preparing the clip for ${name}.`, progress: null };
  }
  if (!clip.delivered) {
    return presence === "online"
      ? { tone: "working", headline: `Sending to ${name}`, detail: "Asking them to accept it.", progress: null }
      : { tone: "waiting", headline: `${name} is offline`, detail: "They'll be asked as soon as they're online. You can close this.", progress: null };
  }
  if (clip.answer === "declined") {
    return { tone: "declined", headline: `${name} declined`, detail: "They didn't take this clip. Nothing was sent.", progress: null };
  }
  if (clip.answer === "pending") {
    return { tone: "waiting", headline: `Waiting for ${name}`, detail: "They've been asked to accept it.", progress: null };
  }
  if (clip.saved) {
    return {
      tone: "done", headline: `Saved to ${name}'s library`,
      detail: "They have their own copy, so it no longer plays or downloads from your PC.", progress: 1
    };
  }
  const transfer = clip.transfer;
  // A download that began in time may finish; everything else stops.
  const finishing = transfer?.purpose === "keep" && transfer.state !== "complete";
  if (windowClosed(clip.availableUntilMs, now) && !finishing) {
    return {
      tone: "ended", headline: "Time's up",
      detail: `${name}'s 15 minutes to watch or keep it have passed. Send it again to give them another 15.`,
      progress: transfer && transfer.totalBytes > 0 ? Math.min(1, transfer.sentBytes / transfer.totalBytes) : null
    };
  }
  const left = clip.availableUntilMs != null ? ` They have ${minutesLeft(clip.availableUntilMs, now)}.` : "";
  if (!transfer) {
    return {
      tone: "waiting", headline: `${name} accepted`,
      detail: `Nothing has moved yet. It streams or downloads from your PC when they open it.${left}`, progress: null
    };
  }
  const fraction = transfer.totalBytes > 0 ? Math.min(1, transfer.sentBytes / transfer.totalBytes) : 0;
  const keep = transfer.purpose === "keep";
  const rate = transfer.bytesPerSecond > 0 ? ` at ${formatBytes(transfer.bytesPerSecond)}/s` : "";
  switch (transfer.state) {
    case "complete":
      // The check mark waits for their verified copy (`saved`), which closes the share.
      return keep
        ? { tone: "working", headline: `Checking the copy on ${name}'s PC`, detail: "Every byte has arrived. It's verified before it joins their library.", progress: 1 }
        : { tone: "waiting", headline: `${name} watched all of it`, detail: `They can watch it again or add it to their library.${left}`, progress: 1 };
    case "interrupted":
      return {
        tone: "trouble", headline: "Connection lost",
        detail: `The connection to ${name} closed at ${percent(fraction)}. It picks up where it stopped when they reconnect.`,
        progress: fraction
      };
    case "paused":
      return keep
        ? { tone: "waiting", headline: `Paused at ${percent(fraction)}`, detail: `${name} stopped the download.`, progress: fraction }
        : { tone: "waiting", headline: `${name} stopped watching`, detail: `${percent(fraction)} has streamed so far.${left}`, progress: fraction };
    case "active":
      return keep
        ? { tone: "working", headline: `Sending to ${name}`, detail: `Adding it to their library: ${percent(fraction)}${rate}.`, progress: fraction }
        : { tone: "working", headline: `${name} is watching`, detail: `${percent(fraction)} streamed${rate}.`, progress: fraction };
  }
}
