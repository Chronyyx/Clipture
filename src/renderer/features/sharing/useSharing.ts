import { useCallback, useEffect, useRef, useState } from "react";
import type { SharingSnapshot } from "../../../shared/sharing";
import { sharing } from "../../platform";
import { useIncomingClipNotice } from "./useIncomingClipNotice";

type Notify = (message: string, durationMs?: number) => void;

function messageOf(error: unknown) {
  return error instanceof Error ? error.message : "Sharing is unavailable right now.";
}

/** The host owns sharing state; this hook mirrors its latest snapshot.
 * Change events are hints, so every one triggers a fresh read. */
export function useSharing(notify: Notify) {
  const [snapshot, setSnapshot] = useState<SharingSnapshot>();
  const [loadError, setLoadError] = useState<string>();
  const sequence = useRef(0);
  const shown = useRef(0);
  const reading = useRef(false);
  const stale = useRef(false);

  // Any reply newer than the one on screen wins. (Requiring the newest
  // request instead starves the UI while events outpace replies.)
  const accept = useCallback((next: SharingSnapshot, issued: number) => {
    if (issued > shown.current) {
      shown.current = issued;
      setSnapshot(next);
    }
  }, []);

  // One read at a time; events during a read cause exactly one more.
  const refresh = useCallback(async () => {
    if (reading.current) {
      stale.current = true;
      return;
    }
    reading.current = true;
    try {
      do {
        stale.current = false;
        const issued = ++sequence.current;
        accept(await sharing.getState(), issued);
        setLoadError(undefined);
      } while (stale.current);
    } catch (error) {
      setLoadError(messageOf(error));
    } finally {
      reading.current = false;
    }
  }, [accept]);

  useEffect(() => {
    void refresh();
    return sharing.onChanged(() => void refresh());
  }, [refresh]);

  const run = useCallback(async (action: () => Promise<SharingSnapshot | unknown>, success?: string) => {
    const issued = ++sequence.current;
    try {
      const result = await action();
      if (result && typeof result === "object" && "friends" in result) accept(result as SharingSnapshot, issued);
      else void refresh();
      if (success) notify(success);
      return true;
    } catch (error) {
      notify(messageOf(error), 6000);
      return false;
    }
  }, [accept, notify, refresh]);

  // Stable so a stream is not reopened every time the snapshot changes.
  const streamUrl = useCallback((shareId: string) => sharing.streamUrl(shareId), []);
  useIncomingClipNotice(snapshot, notify);

  /** Resolves to the new share's id, to follow the friend's answer. */
  const shareClip = useCallback(async (friendId: string, filePath: string) => {
    const issued = ++sequence.current;
    try {
      const result = await sharing.shareClip(friendId, filePath);
      accept(result.snapshot, issued);
      return result.shareId;
    } catch (error) {
      notify(messageOf(error), 6000);
      return undefined;
    }
  }, [accept, notify]);

  return {
    snapshot,
    loadError,
    retry: refresh,
    setEnabled: (enabled: boolean) => run(() => sharing.setEnabled(enabled)),
    setAppearOffline: (appearOffline: boolean) => run(() => sharing.setAppearOffline(appearOffline),
      appearOffline ? "You now appear offline." : "You're online."),
    setDisplayName: (name: string) => run(() => sharing.setDisplayName(name), "Name updated."),
    addFriend: (code: string, name: string) => run(async () => {
      const status = await sharing.addFriend(code, name);
      notify(status === "accepted" ? "You're now friends." : "Friend request sent.");
      return undefined;
    }),
    acceptInvite: (name: string) => run(() => sharing.acceptInvite(), name ? `Friend request sent to ${name}.` : "Friend request sent."),
    dismissInvite: () => run(() => sharing.dismissInvite()),
    acceptFriend: (friendId: string) => run(() => sharing.acceptFriend(friendId), "Request accepted."),
    setFriendNickname: (friendId: string, nickname: string) =>
      run(() => sharing.setFriendNickname(friendId, nickname), nickname ? "Nickname saved. Only you see it." : "Back to the name they chose."),
    removeFriend: (friendId: string) => run(() => sharing.removeFriend(friendId)),
    shareClip,
    answerSharedClip: (shareId: string, accept: boolean) =>
      run(() => sharing.answerSharedClip(shareId, accept), accept ? undefined : "Declined."),
    revokeShare: (shareId: string) => run(() => sharing.revokeShare(shareId), "Stopped sharing."),
    dismissSharedClip: (shareId: string) => run(() => sharing.dismissSharedClip(shareId)),
    saveSharedClip: (shareId: string) => run(() => sharing.saveSharedClip(shareId)),
    cancelDownload: (shareId: string) => run(() => sharing.cancelDownload(shareId)),
    streamUrl
  };
}

export type SharingController = ReturnType<typeof useSharing>;
