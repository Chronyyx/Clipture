import { Check, Users } from "lucide-react";
import type { Friend, SharingSnapshot } from "../../../shared/sharing";
import { FriendAvatar, presenceLabel } from "./FriendAvatar";
import type { SharingController } from "./useSharing";

const rank = { online: 0, unknown: 1, offline: 2 } as const;

function byPresenceThenName(left: Friend, right: Friend) {
  return rank[left.presence] - rank[right.presence] || left.name.localeCompare(right.name);
}

function requestLabel(friend: Friend) {
  return friend.undelivered ? "Sends when they're online" : "Request sent";
}

function StatusSwitch({ snapshot, controller }: { snapshot: SharingSnapshot; controller: SharingController }) {
  const hidden = snapshot.appearOffline;
  return (
    <div className="share-side-status" role="group" aria-label="Your status">
      <button type="button" aria-pressed={!hidden} className={hidden ? "" : "active"}
        onClick={() => { if (hidden) void controller.setAppearOffline(false); }}>
        <span className="share-presence-dot online" aria-hidden="true" /> Online
      </button>
      <button type="button" aria-pressed={hidden} className={hidden ? "active" : ""}
        title="Friends see you as offline, and nothing can be sent to you until you're back online."
        onClick={() => { if (!hidden) void controller.setAppearOffline(true); }}>
        <span className="share-presence-dot hidden" aria-hidden="true" /> Appear offline
      </button>
    </div>
  );
}

export function FriendsSidebar({
  controller,
  onOpenFriend,
  onOpenFriends
}: {
  controller: SharingController;
  onOpenFriend: (friendId: string) => void;
  onOpenFriends: () => void;
}) {
  const { snapshot } = controller;
  if (snapshot && !snapshot.supported) return null;

  if (!snapshot) {
    return (
      <section className="share-side" aria-busy="true" aria-label="Loading friends">
        <span className="skeleton skeleton-text short" />
        {[0, 1, 2].map((row) => (
          <div className="share-side-row" key={row}>
            <span className="share-avatar skeleton" style={{ width: 28, height: 28 }} />
            <span className="skeleton skeleton-text medium" />
          </div>
        ))}
      </section>
    );
  }

  if (!snapshot.enabled) {
    return (
      <section className="share-side">
        <button className="share-side-invite" type="button" onClick={onOpenFriends}>
          <Users size={16} aria-hidden="true" /> Share clips with friends
        </button>
      </section>
    );
  }

  const friends = snapshot.friends.filter((friend) => friend.status === "accepted").sort(byPresenceThenName);
  const requests = snapshot.friends.filter((friend) => friend.status === "incoming");
  const pending = snapshot.friends.filter((friend) => friend.status === "outgoing");
  const online = friends.filter((friend) => friend.presence === "online").length;

  return (
    <section className="share-side" aria-labelledby="share-side-title">
      <div className="share-side-head">
        <h2 id="share-side-title">Friends</h2>
        {!snapshot.appearOffline && snapshot.status === "online" && <span>{online} online</span>}
      </div>
      <StatusSwitch snapshot={snapshot} controller={controller} />
      {snapshot.appearOffline && (
        <p className="share-side-note">You look offline. Friends will send what they share when you're back.</p>
      )}
      {friends.length + requests.length + pending.length === 0 ? (
        <button className="share-side-invite" type="button" onClick={onOpenFriends}>
          <Users size={16} aria-hidden="true" /> Add a friend
        </button>
      ) : (
        <ul className="share-side-list">
          {requests.map((friend) => (
            <li key={friend.id} className="share-side-request">
              <button type="button" className="share-side-row" onClick={onOpenFriends}
                aria-label={`${friend.name} wants to be friends. Open friend requests.`}>
                <FriendAvatar name={friend.name} size={28} />
                <span className="share-side-name">{friend.name}<small>Wants to be friends</small></span>
              </button>
              <button type="button" className="icon-button share-side-accept" title={`Accept ${friend.name}`}
                aria-label={`Accept ${friend.name}`} onClick={() => void controller.acceptFriend(friend.id)}>
                <Check size={16} />
              </button>
            </li>
          ))}
          {friends.map((friend) => (
            <li key={friend.id}>
              <button type="button" className={`share-side-row ${friend.presence}`} onClick={() => onOpenFriend(friend.id)}
                aria-label={`${friend.name}, ${presenceLabel(friend.presence)}. Show clips from ${friend.name}.`}>
                <FriendAvatar name={friend.name} presence={friend.presence} size={28} />
                <span className="share-side-name">{friend.name}</span>
              </button>
            </li>
          ))}
          {pending.map((friend) => (
            <li key={friend.id}>
              <button type="button" className="share-side-row pending" onClick={onOpenFriends}
                aria-label={`${friend.name}, ${requestLabel(friend)}`}>
                <FriendAvatar name={friend.name} size={28} />
                <span className="share-side-name">{friend.name}<small>{requestLabel(friend)}</small></span>
              </button>
            </li>
          ))}
        </ul>
      )}
    </section>
  );
}
