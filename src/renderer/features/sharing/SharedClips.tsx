import { Clapperboard } from "lucide-react";
import { useEffect, useState } from "react";
import type { SharingSnapshot } from "../../../shared/sharing";
import { IncomingClips } from "./IncomingClips";
import { SentClipDialog } from "./SentClipDialog";
import { ShareRail, type RailView } from "./ShareRail";
import { StreamPlayer } from "./StreamPlayer";
import type { SharingController } from "./useSharing";

export function SharedClips({
  snapshot,
  controller,
  focusFriendId,
  onClearFocus
}: {
  snapshot: SharingSnapshot;
  controller: SharingController;
  focusFriendId?: string;
  onClearFocus: () => void;
}) {
  const [selectedId, setSelectedId] = useState<string>();
  const [view, setView] = useState<RailView>("received");
  const [openSent, setOpenSent] = useState<string>();
  const focused = snapshot.friends.find((friend) => friend.id === focusFriendId);
  const mine = <T extends { friendId: string }>(clip: T) => !focused || clip.friendId === focused.id;
  const waiting = snapshot.inbox.filter((clip) => clip.answer === "pending" && mine(clip));
  const clips = snapshot.inbox.filter((clip) => clip.answer !== "pending" && mine(clip));
  const sent = snapshot.outbox.filter(mine);
  const selected = clips.find((clip) => clip.shareId === selectedId) ?? clips[0];
  const hasFriends = snapshot.friends.some((friend) => friend.status === "accepted");

  useEffect(() => { setSelectedId(undefined); }, [focusFriendId]);

  const answer = async (shareId: string, accept: boolean) => {
    const answered = await controller.answerSharedClip(shareId, accept);
    if (answered && accept) {
      setView("received");
      setSelectedId(shareId);
    }
    return answered;
  };

  return (
    <div className="share-main">
      <IncomingClips clips={waiting} onAnswer={answer} />
      <section className="share-stage" aria-label="Shared clips">
        <div className="share-stage-main">
          {selected ? (
            <StreamPlayer
              clip={selected}
              download={snapshot.downloads.find((row) => row.shareId === selected.shareId)}
              streamUrl={controller.streamUrl}
              onSave={() => void controller.saveSharedClip(selected.shareId)}
              onCancel={() => void controller.cancelDownload(selected.shareId)}
              onDismiss={() => void controller.dismissSharedClip(selected.shareId)}
            />
          ) : (
            <div className="share-empty">
              <Clapperboard size={52} aria-hidden="true" />
              <h2>{focused ? `${focused.name} hasn't shared any clips` : "Nothing shared with you yet"}</h2>
              <p>
                {focused
                  ? "Clips they send ask you first, then play here right away."
                  : hasFriends
                    ? "When a friend sends a clip, you're asked to accept it, and then it plays here right away."
                    : "Add a friend with their code. Clips they send will play here."}
              </p>
            </div>
          )}
        </div>
        <ShareRail view={view} onView={setView} snapshot={snapshot} received={clips} sent={sent}
          selectedId={selected?.shareId} onSelect={setSelectedId} onOpenSent={setOpenSent}
          focusedName={focused?.name} onClearFocus={onClearFocus} />
      </section>
      {openSent && (
        <SentClipDialog shareId={openSent} snapshot={snapshot} onClose={() => setOpenSent(undefined)}
          onStopSharing={(shareId) => { setOpenSent(undefined); void controller.revokeShare(shareId); }} />
      )}
    </div>
  );
}
