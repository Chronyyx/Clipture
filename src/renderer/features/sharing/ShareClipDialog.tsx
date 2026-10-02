import { Send, Users, X } from "lucide-react";
import { useEffect, useRef, useState } from "react";
import { createPortal } from "react-dom";
import type { ClipRecord } from "../../../shared/types";
import type { SharingSnapshot } from "../../../shared/sharing";
import { useClipThumbnail } from "../../shared/clips/useClipThumbnail";
import { formatDuration } from "../../shared/clips/clipMetadata";
import { FriendPicker, sendHint } from "./FriendPicker";
import { SendStatusView } from "./SendStatusView";
import { describeSend } from "./sendStatus";
import { useNow } from "./useNow";

/** Pick a friend, then follow the send: their answer, and what they read. */
export function ShareClipDialog({
  clip,
  snapshot,
  onShare,
  onClose,
  onOpenFriends
}: {
  clip: ClipRecord;
  snapshot?: SharingSnapshot;
  /** Resolves to the share id, or undefined when it failed (already said). */
  onShare: (friendId: string) => Promise<string | undefined>;
  onClose: () => void;
  onOpenFriends: () => void;
}) {
  const dialog = useRef<HTMLDialogElement>(null);
  const [chosen, setChosen] = useState<string>();
  const [sending, setSending] = useState<{ friendId: string; shareId?: string }>();
  const thumbnailUrl = useClipThumbnail(clip.filePath);
  const now = useNow();
  const title = clip.title === "Clipture clip" ? "Clipture" : clip.title;
  const friends = snapshot?.friends.filter((friend) => friend.status === "accepted") ?? [];
  const chosenFriend = friends.find((friend) => friend.id === chosen);

  useEffect(() => {
    const element = dialog.current;
    if (element && !element.open) element.showModal();
  }, []);

  const send = async () => {
    if (!chosenFriend || sending) return;
    setSending({ friendId: chosenFriend.id });
    const shareId = await onShare(chosenFriend.id);
    setSending(shareId ? { friendId: chosenFriend.id, shareId } : undefined);
  };
  const openFriends = () => { onClose(); onOpenFriends(); };

  const close = (
    <button className="icon-button share-dialog-close" type="button" aria-label="Close" onClick={onClose}>
      <X size={18} />
    </button>
  );

  let content;
  if (sending) {
    const friend = snapshot?.friends.find((candidate) => candidate.id === sending.friendId);
    const name = friend?.name ?? chosenFriend?.name ?? "your friend";
    const share = sending.shareId ? snapshot?.outbox.find((row) => row.shareId === sending.shareId) : undefined;
    const status = describeSend(share, friend?.presence, name, now);
    content = (
      <>
        {close}
        <SendStatusView status={status} clip={share} title={title} friendName={name} thumbnailUrl={thumbnailUrl} />
        <div className="share-dialog-actions">
          {status.tone === "declined" && (
            <button className="secondary-button" type="button" onClick={() => { setSending(undefined); setChosen(undefined); }}>
              Send to someone else
            </button>
          )}
          <button className="secondary-button share-dialog-done" type="button" onClick={onClose}>Close</button>
        </div>
      </>
    );
  } else {
    const ready = snapshot?.enabled && friends.length > 0;
    content = (
      <>
        <header className="share-dialog-head">
          <span className="share-dialog-thumb" aria-hidden="true">
            {thumbnailUrl && <img src={thumbnailUrl} alt="" />}
            <span>{formatDuration(clip.durationSeconds)}</span>
          </span>
          <span className="share-dialog-heading">
            <h2 id="share-dialog-title">Send to a friend</h2>
            <p className="share-dialog-clip">{title}</p>
          </span>
          {close}
        </header>
        {!snapshot ? (
          <div className="share-pick-list" aria-busy="true">
            {[0, 1, 2].map((row) => <span key={row} className="skeleton share-pick-skeleton" />)}
          </div>
        ) : !ready ? (
          <div className="share-dialog-empty">
            <Users size={32} aria-hidden="true" />
            <p>{snapshot.enabled ? "Add a friend with their code before sending clips." : "Turn on friend sharing to send clips."}</p>
            <button className="secondary-button" type="button" onClick={openFriends}>Open Friends</button>
          </div>
        ) : (
          <FriendPicker friends={friends} chosen={chosen} onChoose={setChosen} />
        )}
        {ready && (
          <footer className="share-dialog-footer">
            <p className="share-hint">{sendHint(chosenFriend, snapshot?.appearOffline)}</p>
            <button className="share-send" type="button" disabled={!chosenFriend} onClick={() => void send()}>
              <Send size={16} aria-hidden="true" />
              <span>{chosenFriend ? `Send to ${chosenFriend.name}` : "Choose a friend"}</span>
            </button>
          </footer>
        )}
      </>
    );
  }

  return createPortal(
    <dialog ref={dialog} className={sending ? "modal share-dialog share-send-dialog sending" : "modal share-dialog share-send-dialog"}
      aria-labelledby={sending ? undefined : "share-dialog-title"} aria-label={sending ? "Sending clip" : undefined}
      onCancel={onClose} onClick={(event) => { if (event.target === dialog.current) onClose(); }}>
      {content}
    </dialog>,
    document.body
  );
}
