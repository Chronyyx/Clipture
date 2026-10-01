import { UserPlus } from "lucide-react";
import { useEffect, useRef, useState } from "react";
import { createPortal } from "react-dom";
import type { PendingInvite } from "../../../shared/sharing";
import { FriendAvatar } from "./FriendAvatar";
import { codeGroups } from "./sharingFormat";

/** Shown when an invite link opens Clipture. Links can come from anyone, so
 * nothing is added until the user confirms, and the name is labelled as the
 * link's claim rather than a verified identity. */
export function InviteDialog({
  invite,
  sharingEnabled,
  onAccept,
  onDismiss
}: {
  invite: PendingInvite;
  sharingEnabled: boolean;
  onAccept: () => Promise<boolean>;
  onDismiss: () => void;
}) {
  const dialog = useRef<HTMLDialogElement>(null);
  const [busy, setBusy] = useState(false);
  const name = invite.name || "Someone";
  const groups = codeGroups(invite.code);

  useEffect(() => {
    const element = dialog.current;
    if (element && !element.open) element.showModal();
  }, []);

  const accept = async () => {
    setBusy(true);
    await onAccept();
    setBusy(false);
  };

  return createPortal(
    <dialog ref={dialog} className="modal share-dialog share-invite-dialog" aria-labelledby="share-invite-title"
      onCancel={(event) => { event.preventDefault(); onDismiss(); }}>
      <div className="share-invite-who">
        <FriendAvatar name={name} size={44} />
        <div>
          <h2 id="share-invite-title">
            {invite.alreadyFriends ? `You're already friends with ${name}` : `Add ${name} as a friend?`}
          </h2>
          <p className="share-hint">
            {invite.name ? "The name comes from the link they sent." : "This link didn't include a name."}
          </p>
        </div>
      </div>
      {!invite.alreadyFriends && (
        <>
          <p className="share-invite-code" aria-label={`Friend code ${invite.code}`}>
            {groups.slice(0, 3).join(" ")} … {groups.slice(-2).join(" ")}
          </p>
          <p className="share-hint">
            {sharingEnabled
              ? "They'll get your friend request and can start sharing clips once it reaches them."
              : "This turns on friend sharing and sends them your friend request."}
          </p>
        </>
      )}
      <div className="share-dialog-actions">
        {invite.alreadyFriends ? (
          <button className="primary" type="button" autoFocus onClick={onDismiss}>OK</button>
        ) : (
          <>
            <button className="secondary-button" type="button" autoFocus onClick={onDismiss}>Not now</button>
            <button className="primary" type="button" disabled={busy} onClick={() => void accept()}>
              <UserPlus size={16} /> {busy ? "Adding..." : sharingEnabled ? "Add friend" : "Turn on sharing and add"}
            </button>
          </>
        )}
      </div>
    </dialog>,
    document.body
  );
}
