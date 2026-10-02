import { useEffect, useRef } from "react";
import type { SharingSnapshot } from "../../../shared/sharing";

type Notify = (message: string, durationMs?: number) => void;

/** Says when a friend wants to send a clip, wherever the user is in the app.
 * Clips already waiting when the UI opens are not announced again: the
 * Friends badge shows them, and the host already played the sound. */
export function useIncomingClipNotice(snapshot: SharingSnapshot | undefined, notify: Notify) {
  const seen = useRef<Set<string>>();

  useEffect(() => {
    if (!snapshot) return;
    const waiting = snapshot.inbox.filter((clip) => clip.answer === "pending");
    if (!seen.current) {
      seen.current = new Set(waiting.map((clip) => clip.shareId));
      return;
    }
    const fresh = waiting.filter((clip) => !seen.current?.has(clip.shareId));
    for (const clip of fresh) seen.current.add(clip.shareId);
    if (fresh.length === 1) notify(`${fresh[0]!.friendName} wants to send you "${fresh[0]!.title}". Open Friends to accept it.`, 6000);
    else if (fresh.length > 1) notify(`${fresh.length} clips are waiting for you in Friends.`, 6000);
  }, [snapshot, notify]);
}

/** How many things in Friends wait for the user: requests and clips. */
export function friendsWaiting(snapshot: SharingSnapshot | undefined) {
  const requests = snapshot?.friends.filter((friend) => friend.status === "incoming").length ?? 0;
  const clips = snapshot?.inbox.filter((clip) => clip.answer === "pending").length ?? 0;
  const parts = [
    requests > 0 && `${requests} friend ${requests === 1 ? "request" : "requests"}`,
    clips > 0 && `${clips} ${clips === 1 ? "clip" : "clips"} to accept`
  ].filter(Boolean);
  return { count: requests + clips, label: parts.join(", ") };
}
