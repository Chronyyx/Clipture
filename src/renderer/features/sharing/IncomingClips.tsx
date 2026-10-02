import { Check, X } from "lucide-react";
import { useState } from "react";
import type { SharedClip } from "../../../shared/sharing";
import { formatDuration } from "../../shared/clips/clipMetadata";
import { friendStyle } from "./FriendAvatar";
import { formatBytes, formatSharedAt } from "./sharingFormat";

/** Clips friends want to send, waiting for a yes or no. Nothing streams or
 * downloads until the user accepts. */
export function IncomingClips({ clips, onAnswer }: {
  clips: SharedClip[];
  onAnswer: (shareId: string, accept: boolean) => Promise<boolean>;
}) {
  const [busy, setBusy] = useState<string>();
  if (clips.length === 0) return null;

  const answer = async (shareId: string, accept: boolean) => {
    setBusy(shareId);
    await onAnswer(shareId, accept);
    setBusy(undefined);
  };

  return (
    <section className="share-incoming" aria-label="Clips waiting for you">
      {clips.map((clip) => (
        <article key={clip.shareId} className="share-incoming-item" style={friendStyle(clip.friendName)}>
          <span className="share-tile share-incoming-tile" aria-hidden="true">
            <span className="share-tile-letter">{(clip.gameOrApp || clip.title).charAt(0).toUpperCase()}</span>
            {clip.durationSeconds > 0 && <span className="share-tile-duration">{formatDuration(clip.durationSeconds)}</span>}
          </span>
          <span className="share-incoming-copy">
            <span className="share-incoming-who">{clip.friendName} wants to send you a clip</span>
            <strong>{clip.title}</strong>
            <span>{[clip.gameOrApp, formatBytes(clip.size), formatSharedAt(clip.sharedAtMs)].filter(Boolean).join(", ")}</span>
          </span>
          <span className="share-incoming-actions">
            <button className="secondary-button" type="button" disabled={busy === clip.shareId}
              onClick={() => void answer(clip.shareId, false)}>
              <X size={16} aria-hidden="true" /> Decline
            </button>
            <button className="share-send" type="button" disabled={busy === clip.shareId}
              onClick={() => void answer(clip.shareId, true)}>
              <Check size={16} aria-hidden="true" /> Accept
            </button>
          </span>
        </article>
      ))}
    </section>
  );
}
