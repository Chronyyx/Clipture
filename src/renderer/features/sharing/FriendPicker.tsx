import type { KeyboardEvent } from "react";
import type { Friend } from "../../../shared/sharing";
import { FriendAvatar } from "./FriendAvatar";

const rank = { online: 0, unknown: 1, offline: 2 } as const;

/** One line under the list about what happens for the chosen friend. */
export function sendHint(friend: Friend | undefined, appearOffline?: boolean) {
  if (appearOffline) return "You appear offline, so they'll be asked when you're back online.";
  if (!friend) return "They're asked first, and can stream it or keep it once they accept.";
  if (friend.presence === "online") return `${friend.name} is asked to accept it, then can stream it right away.`;
  return `${friend.name} is offline. They'll be asked next time you're both online.`;
}

/** Choosing only selects: sending is always the footer button, so a stray
 * or repeated click on a row can never send. */
export function FriendPicker({ friends, chosen, onChoose }: {
  friends: Friend[];
  chosen?: string;
  onChoose: (friendId: string) => void;
}) {
  const sorted = [...friends].sort((left, right) =>
    rank[left.presence] - rank[right.presence] || left.name.localeCompare(right.name));
  const online = sorted.filter((friend) => friend.presence === "online");
  const away = sorted.filter((friend) => friend.presence !== "online");

  // Arrow keys move the choice, as in any radio group.
  const onKeyDown = (event: KeyboardEvent<HTMLDivElement>) => {
    if (!["ArrowDown", "ArrowUp"].includes(event.key)) return;
    event.preventDefault();
    const index = sorted.findIndex((friend) => friend.id === chosen);
    const next = sorted[(index + (event.key === "ArrowDown" ? 1 : sorted.length - 1)) % sorted.length];
    if (!next) return;
    onChoose(next.id);
    event.currentTarget.querySelector<HTMLElement>(`[data-friend="${next.id}"]`)?.focus();
  };

  const row = (friend: Friend, index: number) => {
    const active = chosen === friend.id;
    const tabbable = active || (!chosen && index === 0);
    return (
      <button key={friend.id} data-friend={friend.id} type="button" role="radio" aria-checked={active}
        tabIndex={tabbable ? 0 : -1} className={active ? "share-pick active" : "share-pick"}
        onClick={() => onChoose(friend.id)}>
        <FriendAvatar name={friend.name} presence={friend.presence} size={34} />
        <span className="share-pick-name">
          {friend.name}
          <small>{friend.presence === "online" ? "Online" : friend.presence === "offline" ? "Offline" : "Status hidden"}</small>
        </span>
        <span className="share-pick-radio" aria-hidden="true" />
      </button>
    );
  };

  return (
    <div className="share-pick-list" role="radiogroup" aria-label="Friend" onKeyDown={onKeyDown}>
      {online.length > 0 && <p className="share-pick-group">Online now</p>}
      {online.map((friend, index) => row(friend, index))}
      {away.length > 0 && <p className="share-pick-group">Away</p>}
      {away.map((friend, index) => row(friend, online.length + index))}
    </div>
  );
}
