import { Check, Clock, UserMinus, UserPlus, X } from "lucide-react";
import { useState, type FormEvent } from "react";
import type { Friend } from "../../../shared/sharing";
import { FriendAvatar, presenceLabel } from "./FriendAvatar";
import { formatSharedAt } from "./sharingFormat";

export function AddFriendForm({ onAdd }: { onAdd: (code: string, name: string) => Promise<boolean> }) {
  const [code, setCode] = useState("");
  const [name, setName] = useState("");
  const [busy, setBusy] = useState(false);

  const submit = async (event: FormEvent) => {
    event.preventDefault();
    if (!code.trim() || busy) return;
    setBusy(true);
    if (await onAdd(code.trim(), name.trim())) {
      setCode("");
      setName("");
    }
    setBusy(false);
  };

  return (
    <form className="share-add-form" onSubmit={(event) => void submit(event)}>
      <h2>Add a friend</h2>
      <label>
        <span>Invite link or friend code</span>
        <input value={code} onChange={(event) => setCode(event.target.value)} spellCheck={false}
          autoComplete="off" maxLength={512} placeholder="Paste what your friend sent" />
      </label>
      <label>
        <span>Name</span>
        <input value={name} onChange={(event) => setName(event.target.value)} maxLength={40}
          placeholder={/^https?:|^clipture:/i.test(code.trim()) ? "From the link" : "Optional"} />
      </label>
      <button className="primary" type="submit" disabled={!code.trim() || busy}>
        <UserPlus size={17} /> {busy ? "Sending request..." : "Send request"}
      </button>
    </form>
  );
}

export function FriendRequests({
  requests,
  onAccept,
  onDecline
}: {
  requests: Friend[];
  onAccept: (id: string) => void;
  onDecline: (id: string) => void;
}) {
  if (requests.length === 0) return null;
  return (
    <section className="share-requests" aria-labelledby="share-requests-title">
      <h2 id="share-requests-title">Requests</h2>
      <ul className="share-friend-list">
        {requests.map((friend) => (
          <li key={friend.id} className="share-friend-row">
            <FriendAvatar name={friend.name} />
            <span className="share-friend-copy">
              <strong>{friend.name}</strong>
              <span title={friend.id}>Asked {formatSharedAt(friend.addedAtMs)}</span>
            </span>
            <span className="share-row-actions">
              <button className="icon-button share-accept" type="button" title={`Accept ${friend.name}`}
                aria-label={`Accept ${friend.name}`} onClick={() => onAccept(friend.id)}>
                <Check size={17} />
              </button>
              <button className="icon-button" type="button" title={`Decline ${friend.name}`}
                aria-label={`Decline ${friend.name}`} onClick={() => onDecline(friend.id)}>
                <X size={17} />
              </button>
            </span>
          </li>
        ))}
      </ul>
    </section>
  );
}

export function FriendList({ friends, onRemove }: { friends: Friend[]; onRemove: (friend: Friend) => void }) {
  const [confirming, setConfirming] = useState<string>();
  return (
    <section className="share-friends" aria-labelledby="share-friends-title">
      <h2 id="share-friends-title">Friends <span>{friends.filter((friend) => friend.status === "accepted").length}</span></h2>
      {friends.length === 0 ? (
        <p className="share-hint">No friends yet. Add someone with their code.</p>
      ) : (
        <ul className="share-friend-list">
          {friends.map((friend) => (
            <li key={friend.id} className="share-friend-row">
              <FriendAvatar name={friend.name} presence={friend.status === "accepted" ? friend.presence : undefined} />
              <span className="share-friend-copy">
                <strong>{friend.name}</strong>
                {friend.status === "outgoing" ? (
                  <span><Clock size={12} aria-hidden="true" /> {friend.undelivered ? "Request waits until they're online" : "Waiting for them to accept"}</span>
                ) : (
                  <span>{presenceLabel(friend.presence)}</span>
                )}
              </span>
              <span className="share-row-actions">
                {confirming === friend.id ? (
                  <>
                    <button className="secondary-button share-danger" type="button" onClick={() => { setConfirming(undefined); onRemove(friend); }}>
                      Remove
                    </button>
                    <button className="icon-button" type="button" aria-label="Keep friend" onClick={() => setConfirming(undefined)}>
                      <X size={16} />
                    </button>
                  </>
                ) : (
                  <button className="icon-button" type="button" title={`Remove ${friend.name}`}
                    aria-label={`Remove ${friend.name}`} onClick={() => setConfirming(friend.id)}>
                    <UserMinus size={16} />
                  </button>
                )}
              </span>
            </li>
          ))}
        </ul>
      )}
    </section>
  );
}
