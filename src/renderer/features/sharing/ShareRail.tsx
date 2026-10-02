import { Check, X } from "lucide-react";
import type { CSSProperties } from "react";
import type { SharedClip, SharingSnapshot } from "../../../shared/sharing";
import { formatDuration } from "../../shared/clips/clipMetadata";
import { friendStyle } from "./FriendAvatar";
import { describeSend } from "./sendStatus";
import { formatSharedAt } from "./sharingFormat";
import { useNow } from "./useNow";

export type RailView = "received" | "sent";

export function ClipTile({ clip }: { clip: SharedClip }) {
  return (
    <span className="share-tile" aria-hidden="true" style={friendStyle(clip.friendName)}>
      <span className="share-tile-letter">{(clip.gameOrApp || clip.title).charAt(0).toUpperCase()}</span>
      {clip.durationSeconds > 0 && <span className="share-tile-duration">{formatDuration(clip.durationSeconds)}</span>}
    </span>
  );
}

function SentRow({ clip, snapshot, onOpen }: { clip: SharedClip; snapshot: SharingSnapshot; onOpen: () => void }) {
  const now = useNow();
  const friend = snapshot.friends.find((candidate) => candidate.id === clip.friendId);
  const status = describeSend(clip, friend?.presence, friend?.name ?? clip.friendName, now);
  const meter = { "--sent": status.progress ?? 0 } as CSSProperties;
  return (
    <li>
      <button type="button" className="share-rail-item" onClick={onOpen}>
        <ClipTile clip={clip} />
        <span className="share-rail-copy">
          <strong>{clip.title}</strong>
          <span>To {clip.friendName}, {formatSharedAt(clip.sharedAtMs)}</span>
          <span className="share-rail-state" data-tone={status.tone}>{status.headline}</span>
          {status.progress !== null && !["done", "ended"].includes(status.tone) && <span className="share-rail-meter" style={meter} aria-hidden="true" />}
        </span>
      </button>
    </li>
  );
}

export function ShareRail({
  view, onView, snapshot, received, sent, selectedId, onSelect, onOpenSent, focusedName, onClearFocus
}: {
  view: RailView;
  onView: (view: RailView) => void;
  snapshot: SharingSnapshot;
  received: SharedClip[];
  sent: SharedClip[];
  selectedId?: string;
  onSelect: (shareId: string) => void;
  onOpenSent: (shareId: string) => void;
  focusedName?: string;
  onClearFocus: () => void;
}) {
  const tab = (id: RailView, label: string, count: number) => (
    <button type="button" role="tab" id={`share-rail-${id}`} aria-selected={view === id}
      aria-controls="share-rail-panel" className={view === id ? "active" : ""} onClick={() => onView(id)}>
      {label} <span>{count}</span>
    </button>
  );
  return (
    <aside className="share-rail">
      <div className="share-rail-tabs" role="tablist" aria-label="Shared clips">
        {tab("received", "Shared with you", received.length)}
        {tab("sent", "Sent", sent.length)}
      </div>
      {focusedName && (
        <div className="share-rail-filter">
          <span>Only {focusedName}</span>
          <button className="icon-button" type="button" aria-label="Show clips from everyone" onClick={onClearFocus}>
            <X size={14} />
          </button>
        </div>
      )}
      <ul className="share-rail-list" id="share-rail-panel" role="tabpanel" aria-labelledby={`share-rail-${view}`}>
        {view === "sent" && sent.length === 0 && <li className="share-rail-empty">Clips you send show up here with their progress.</li>}
        {view === "sent" && sent.map((clip) => (
          <SentRow key={clip.shareId} clip={clip} snapshot={snapshot} onOpen={() => onOpenSent(clip.shareId)} />
        ))}
        {view === "received" && received.map((clip) => (
          <li key={clip.shareId}>
            <button type="button" className={clip.shareId === selectedId ? "share-rail-item active" : "share-rail-item"}
              aria-current={clip.shareId === selectedId ? "true" : undefined} onClick={() => onSelect(clip.shareId)}>
              <ClipTile clip={clip} />
              <span className="share-rail-copy">
                <strong>{clip.title}</strong>
                <span>{clip.friendName}, {formatSharedAt(clip.sharedAtMs)}</span>
                {clip.saved && <span className="share-rail-state"><Check size={12} /> In your library</span>}
              </span>
            </button>
          </li>
        ))}
      </ul>
    </aside>
  );
}
