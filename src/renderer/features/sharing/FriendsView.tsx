import { Lock, MonitorUp, RotateCcw, Users } from "lucide-react";
import type { ReactNode } from "react";
import type { SharingSnapshot } from "../../../shared/sharing";
import { AddFriendForm, FriendList, FriendRequests } from "./FriendList";
import { FriendCodeCard } from "./FriendCodeCard";
import { SharedClips } from "./SharedClips";
import { SharingSkeleton } from "./SharingSkeleton";
import type { SharingController } from "./useSharing";

const statusLabels: Record<SharingSnapshot["status"], string> = {
  off: "Off",
  starting: "Connecting",
  online: "Online",
  error: "Can't connect"
};

function Intro({ onEnable }: { onEnable: () => void }) {
  return (
    <section className="share-intro">
      <Users size={44} aria-hidden="true" />
      <h2>Send clips straight to your friends</h2>
      <ul>
        <li><MonitorUp size={18} aria-hidden="true" /> Clips go from your PC to theirs. There's no upload and no cloud copy.</li>
        <li><Lock size={18} aria-hidden="true" /> Connections are end-to-end encrypted, and only friends you accept can watch.</li>
        <li><Users size={18} aria-hidden="true" /> Friends stream a clip instantly and choose whether to keep it.</li>
      </ul>
      <p className="share-hint">
        Your friend can watch only while your PC is on with Clipture running. To help friends find
        your PC, Clipture uses iroh's public relay and lookup service, which can't read your clips.
      </p>
      <button className="primary" type="button" onClick={onEnable}>Turn on friend sharing</button>
    </section>
  );
}

export function FriendsView({
  controller,
  headerControls,
  focusFriendId,
  onClearFocus
}: {
  controller: SharingController;
  headerControls?: ReactNode;
  focusFriendId?: string;
  onClearFocus: () => void;
}) {
  const { snapshot, loadError } = controller;
  const requests = snapshot?.friends.filter((friend) => friend.status === "incoming") ?? [];
  const friends = snapshot?.friends.filter((friend) => friend.status !== "incoming") ?? [];

  let body: ReactNode;
  if (!snapshot) {
    body = loadError ? (
      <div className="share-empty">
        <p>{loadError}</p>
        <button className="secondary-button" type="button" onClick={() => void controller.retry()}>
          <RotateCcw size={16} /> Try again
        </button>
      </div>
    ) : <SharingSkeleton />;
  } else if (!snapshot.supported) {
    body = <div className="share-empty"><p>Friend sharing needs the Clipture desktop app.</p></div>;
  } else if (!snapshot.enabled) {
    body = <Intro onEnable={() => void controller.setEnabled(true)} />;
  } else {
    body = (
      <div className="share-layout">
        <SharedClips snapshot={snapshot} controller={controller} focusFriendId={focusFriendId} onClearFocus={onClearFocus} />
        <div className="share-people">
          <FriendCodeCard code={snapshot.friendCode} inviteLink={snapshot.inviteLink}
            displayName={snapshot.displayName} onRename={controller.setDisplayName} />
          <FriendRequests
            requests={requests}
            onAccept={(id) => void controller.acceptFriend(id)}
            onDecline={(id) => void controller.removeFriend(id)}
            onRename={controller.setFriendNickname}
          />
          <FriendList friends={friends} onRemove={(friend) => void controller.removeFriend(friend.id)}
            onRename={controller.setFriendNickname} />
          <AddFriendForm onAdd={controller.addFriend} startOpen={friends.length === 0} />
        </div>
      </div>
    );
  }

  return (
    <div className="share-view">
      <header className="share-header">
        <div className="share-header-title">
          <h1>Friends</h1>
          {snapshot?.enabled && (snapshot.appearOffline && snapshot.status === "online" ? (
            <span className="share-status hidden">
              <span className="share-status-dot" aria-hidden="true" /> Appearing offline
            </span>
          ) : (
            <span className={`share-status ${snapshot.status}`} title={snapshot.statusMessage ?? undefined}>
              <span className="share-status-dot" aria-hidden="true" /> {statusLabels[snapshot.status]}
            </span>
          ))}
        </div>
        <div className="share-header-actions">
          {headerControls}
          {snapshot?.supported && snapshot.enabled && (
            <label className="share-toggle">
              <span>Sharing</span>
              <input className="toggle-switch" type="checkbox" checked
                aria-label="Turn off friend sharing" onChange={() => void controller.setEnabled(false)} />
            </label>
          )}
        </div>
      </header>
      {snapshot?.status === "error" && snapshot.statusMessage && (
        <p className="share-status-error" role="alert">{snapshot.statusMessage}</p>
      )}
      {body}
    </div>
  );
}
