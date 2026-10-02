import type { CSSProperties } from "react";
import type { FriendPresence } from "../../../shared/sharing";

const presenceLabels: Record<FriendPresence, string> = {
  online: "Online",
  offline: "Offline",
  unknown: "Hidden while you appear offline"
};

export function presenceLabel(presence: FriendPresence) {
  return presenceLabels[presence];
}

/** A stable hue per name, so a friend keeps one colour everywhere they
 * appear. The stylesheet blends it with the theme accent. */
export function friendHue(name: string) {
  let hash = 0;
  for (const char of name.trim().toLowerCase()) hash = (hash * 31 + char.charCodeAt(0)) >>> 0;
  return hash % 360;
}

export function friendStyle(name: string): CSSProperties {
  return { "--friend-hue": friendHue(name) } as CSSProperties;
}

/** Initial with an optional presence dot; the dot is decorative, so callers
 * give the presence in text too (visible or as an accessible label). */
export function FriendAvatar({ name, presence, size = 32 }: {
  name: string;
  presence?: FriendPresence;
  size?: number;
}) {
  return (
    <span className={presence ? `share-avatar ${presence}` : "share-avatar"} aria-hidden="true"
      style={{ ...friendStyle(name), width: size, height: size, fontSize: size * 0.42 }}>
      {name.trim().charAt(0).toUpperCase() || "?"}
      {presence && <span className={`share-presence-dot ${presence}`} />}
    </span>
  );
}
