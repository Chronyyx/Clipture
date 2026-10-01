import { Check, Send, Users } from "lucide-react";
import { useEffect, useRef, useState } from "react";
import { createPortal } from "react-dom";
import type { ClipRecord } from "../../../shared/types";
import type { SharingSnapshot } from "../../../shared/sharing";
import { FriendAvatar } from "./FriendAvatar";

export function ShareClipDialog({
  clip,
  snapshot,
  onShare,
  onClose,
  onOpenFriends
}: {
  clip: ClipRecord;
  snapshot?: SharingSnapshot;
  onShare: (friendId: string, friendName: string) => Promise<boolean>;
  onClose: () => void;
  onOpenFriends: () => void;
}) {
  const dialog = useRef<HTMLDialogElement>(null);
  const [chosen, setChosen] = useState<string>();
  const [sending, setSending] = useState(false);
  const rank = { online: 0, unknown: 1, offline: 2 } as const;
  const friends = (snapshot?.friends.filter((friend) => friend.status === "accepted") ?? [])
    .sort((left, right) => rank[left.presence] - rank[right.presence] || left.name.localeCompare(right.name));
  const chosenFriend = friends.find((friend) => friend.id === chosen);

  useEffect(() => {
    const element = dialog.current;
    if (element && !element.open) element.showModal();
  }, []);

  const send = async () => {
    if (!chosenFriend || sending) return;
    setSending(true);
    const sent = await onShare(chosenFriend.id, chosenFriend.name);
    setSending(false);
    if (sent) onClose();
  };

  const openFriends = () => { onClose(); onOpenFriends(); };

  let body;
  if (!snapshot) {
    body = (
      <div className="share-pick-list" aria-busy="true">
        {[0, 1, 2].map((row) => <span key={row} className="skeleton share-pick-skeleton" />)}
      </div>
    );
  } else if (!snapshot.enabled || friends.length === 0) {
    body = (
      <div className="share-dialog-empty">
        <Users size={32} aria-hidden="true" />
        <p>{snapshot.enabled ? "Add a friend with their code before sending clips." : "Turn on friend sharing to send clips."}</p>
        <button className="secondary-button" type="button" onClick={openFriends}>Open Friends</button>
      </div>
    );
  } else {
    body = (
      <div className="share-pick-list" role="radiogroup" aria-label="Friend">
        {friends.map((friend) => (
          <button key={friend.id} type="button" role="radio" aria-checked={chosen === friend.id}
            className={chosen === friend.id ? "share-pick active" : "share-pick"} onClick={() => setChosen(friend.id)}>
            <FriendAvatar name={friend.name} presence={friend.presence} />
            <span className="share-pick-name">
              {friend.name}
              {friend.presence === "offline" && <small>Gets it when they're online</small>}
            </span>
            {chosen === friend.id && <Check size={17} aria-hidden="true" />}
          </button>
        ))}
      </div>
    );
  }

  return createPortal(
    <dialog ref={dialog} className="modal share-dialog" aria-labelledby="share-dialog-title" onCancel={onClose}
      onClick={(event) => { if (event.target === dialog.current) onClose(); }}>
      <h2 id="share-dialog-title">Send to a friend</h2>
      <p className="share-dialog-clip">{clip.title}</p>
      {body}
      <p className="share-hint">
        {snapshot?.appearOffline
          ? "You appear offline, so this will be sent when you're back online."
          : "They can stream it right away while you're both online, and add it to their library if they want to keep it."}
      </p>
      <div className="share-dialog-actions">
        <button className="secondary-button" type="button" onClick={onClose}>Cancel</button>
        <button className="primary" type="button" disabled={!chosenFriend || sending} onClick={() => void send()}>
          <Send size={16} /> {sending ? "Preparing clip..." : chosenFriend ? `Send to ${chosenFriend.name}` : "Send"}
        </button>
      </div>
    </dialog>,
    document.body
  );
}
