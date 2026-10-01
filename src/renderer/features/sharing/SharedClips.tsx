import { Check, Clapperboard, X } from "lucide-react";
import { useEffect, useState } from "react";
import type { SharedClip, SharingSnapshot } from "../../../shared/sharing";
import { formatDuration } from "../../shared/clips/clipMetadata";
import { StreamPlayer } from "./StreamPlayer";
import { formatSharedAt } from "./sharingFormat";
import type { SharingController } from "./useSharing";

function ClipTile({ clip }: { clip: SharedClip }) {
  return (
    <span className="share-tile" aria-hidden="true">
      <span className="share-tile-letter">{(clip.gameOrApp || clip.title).charAt(0).toUpperCase()}</span>
      {clip.durationSeconds > 0 && <span className="share-tile-duration">{formatDuration(clip.durationSeconds)}</span>}
    </span>
  );
}

export function SharedClips({
  snapshot,
  controller,
  focusFriendId,
  onClearFocus
}: {
  snapshot: SharingSnapshot;
  controller: SharingController;
  focusFriendId?: string;
  onClearFocus: () => void;
}) {
  const [selectedId, setSelectedId] = useState<string>();
  const focused = snapshot.friends.find((friend) => friend.id === focusFriendId);
  const clips = snapshot.inbox.filter((clip) => !focused || clip.friendId === focused.id);
  const selected = clips.find((clip) => clip.shareId === selectedId) ?? clips[0];
  const hasFriends = snapshot.friends.some((friend) => friend.status === "accepted");

  useEffect(() => { setSelectedId(undefined); }, [focusFriendId]);

  return (
    <section className="share-stage" aria-label="Shared clips">
      <div className="share-stage-main">
        {selected ? (
          <StreamPlayer
              clip={selected}
              download={snapshot.downloads.find((row) => row.shareId === selected.shareId)}
              streamUrl={controller.streamUrl}
              onSave={() => void controller.saveSharedClip(selected.shareId)}
              onCancel={() => void controller.cancelDownload(selected.shareId)}
              onDismiss={() => void controller.dismissSharedClip(selected.shareId)}
            />
        ) : (
          <div className="share-empty">
            <Clapperboard size={52} aria-hidden="true" />
            <h2>{focused ? `${focused.name} hasn't shared any clips` : "Nothing shared with you yet"}</h2>
            <p>
              {focused
                ? "Clips they share will play here right away."
                : hasFriends
                  ? "When a friend shares a clip, it shows up here and plays right away."
                  : "Add a friend with their code. Clips they share will play here."}
            </p>
          </div>
        )}
      </div>
      <aside className="share-rail">
        <h2 className="share-rail-head">Shared with you <span>{snapshot.inbox.length}</span></h2>
        {focused && (
          <div className="share-rail-filter">
            <span>Only {focused.name}</span>
            <button className="icon-button" type="button" aria-label="Show clips from everyone" onClick={onClearFocus}>
              <X size={14} />
            </button>
          </div>
        )}
        <ul className="share-rail-list">
          {clips.map((clip) => (
            <li key={clip.shareId}>
              <button type="button" className={clip.shareId === selected?.shareId ? "share-rail-item active" : "share-rail-item"}
                aria-current={clip.shareId === selected?.shareId ? "true" : undefined}
                onClick={() => setSelectedId(clip.shareId)}>
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
    </section>
  );
}
