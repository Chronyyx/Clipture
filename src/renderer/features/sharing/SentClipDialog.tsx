import { X } from "lucide-react";
import { useEffect, useRef } from "react";
import { createPortal } from "react-dom";
import type { SharingSnapshot } from "../../../shared/sharing";
import { SendStatusView } from "./SendStatusView";
import { describeSend } from "./sendStatus";
import { useNow } from "./useNow";

/** The same live status the send dialog shows, for a clip sent earlier. */
export function SentClipDialog({ shareId, snapshot, onStopSharing, onClose }: {
  shareId: string;
  snapshot: SharingSnapshot;
  onStopSharing: (shareId: string) => void;
  onClose: () => void;
}) {
  const dialog = useRef<HTMLDialogElement>(null);
  const now = useNow();
  const clip = snapshot.outbox.find((row) => row.shareId === shareId);
  const friend = snapshot.friends.find((candidate) => candidate.id === clip?.friendId);

  useEffect(() => {
    const element = dialog.current;
    if (element && !element.open) element.showModal();
  }, []);
  useEffect(() => { if (!clip) onClose(); }, [clip, onClose]);
  if (!clip) return null;

  const name = friend?.name ?? clip.friendName;
  const status = describeSend(clip, friend?.presence, name, now);
  return createPortal(
    <dialog ref={dialog} className="modal share-dialog share-send-dialog sending" aria-label={`${clip.title}, sent to ${name}`}
      onCancel={onClose} onClick={(event) => { if (event.target === dialog.current) onClose(); }}>
      <button className="icon-button share-dialog-close" type="button" aria-label="Close" onClick={onClose}>
        <X size={18} />
      </button>
      <SendStatusView status={status} clip={clip} title={clip.title} friendName={name} />
      <div className="share-dialog-actions">
        {!["declined", "ended"].includes(status.tone) && !clip.saved && (
          <button className="secondary-button share-danger" type="button" onClick={() => onStopSharing(clip.shareId)}
            title={`${name} can no longer stream or download it`}>
            Stop sharing
          </button>
        )}
        <button className="secondary-button share-dialog-done" type="button" onClick={onClose}>Close</button>
      </div>
    </dialog>,
    document.body
  );
}
