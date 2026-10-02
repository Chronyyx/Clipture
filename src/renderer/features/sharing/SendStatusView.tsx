import { Check, Clock, Download, Play, Send, TimerOff, WifiOff, X } from "lucide-react";
import type { CSSProperties } from "react";
import type { SharedClip } from "../../../shared/sharing";
import { friendStyle } from "./FriendAvatar";
import type { SendStatus } from "./sendStatus";

const RING = 2 * Math.PI * 24;

function BadgeIcon({ status, clip }: { status: SendStatus; clip?: SharedClip }) {
  const size = 22;
  switch (status.tone) {
    case "done": return <Check size={size} strokeWidth={3} />;
    case "declined": return <X size={size} strokeWidth={3} />;
    case "ended": return <TimerOff size={size - 2} strokeWidth={2.5} />;
    case "trouble": return <WifiOff size={size - 2} strokeWidth={2.5} />;
    case "waiting":
      return clip?.transfer?.state === "complete"
        ? <Play size={size - 3} strokeWidth={2.5} />
        : <Clock size={size - 2} strokeWidth={2.5} />;
    case "working":
      if (!clip?.transfer) return <Send size={size - 3} strokeWidth={2.5} />;
      return clip.transfer.purpose === "keep" ? <Download size={size - 2} strokeWidth={2.5} /> : <Play size={size - 3} strokeWidth={2.5} />;
  }
}

/** The clip, a status badge on its lower edge, and one sentence on what is
 * happening. The ring around the badge fills as bytes leave your PC. */
export function SendStatusView({ status, clip, title, friendName, thumbnailUrl }: {
  status: SendStatus;
  clip?: SharedClip;
  title: string;
  friendName: string;
  thumbnailUrl?: string;
}) {
  const measured = status.progress !== null;
  const ring = { "--ring-gap": `${RING * (1 - (status.progress ?? 0.28))}` } as CSSProperties;
  return (
    <div className="send-status" data-tone={status.tone}>
      <div className="send-art" style={friendStyle(friendName)}>
        {thumbnailUrl
          ? <img src={thumbnailUrl} alt="" />
          : <span className="send-art-letter" aria-hidden="true">{(clip?.gameOrApp || title).charAt(0).toUpperCase()}</span>}
        <span className={measured ? "send-badge" : "send-badge spinning"} aria-hidden="true">
          <svg className="send-ring-svg" viewBox="0 0 56 56">
            <circle className="send-ring-track" cx="28" cy="28" r="24" />
            <circle className="send-ring" cx="28" cy="28" r="24" strokeDasharray={RING} style={ring} />
          </svg>
          <BadgeIcon status={status} clip={clip} />
        </span>
      </div>
      <div className="send-copy" aria-live="polite">
        <h2>{status.headline}</h2>
        <p className="send-clip-title">{title}</p>
        <p className="send-detail">{status.detail}</p>
      </div>
      {measured && (
        <div className="send-meter" role="progressbar" aria-label="Sent" aria-valuemin={0} aria-valuemax={100}
          aria-valuenow={Math.floor((status.progress ?? 0) * 100)}>
          <span style={{ transform: `scaleX(${status.progress})` }} />
        </div>
      )}
    </div>
  );
}
