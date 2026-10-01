import type { FriendPresence } from "../../../shared/sharing";

const presenceLabels: Record<FriendPresence, string> = {
  online: "Online",
  offline: "Offline",
  unknown: "Hidden while you appear offline"
};

export function presenceLabel(presence: FriendPresence) {
  return presenceLabels[presence];
}

/** Initial with an optional presence dot; the dot is decorative, so callers
 * give the presence in text too (visible or as an accessible label). */
export function FriendAvatar({ name, presence, size = 32 }: {
  name: string;
  presence?: FriendPresence;
  size?: number;
}) {
  return (
    <span className="share-avatar" aria-hidden="true" style={{ width: size, height: size, fontSize: size * 0.42 }}>
      {name.trim().charAt(0).toUpperCase() || "?"}
      {presence && <span className={`share-presence-dot ${presence}`} />}
    </span>
  );
}
