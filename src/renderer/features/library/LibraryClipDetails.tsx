import { Edit3, Send } from "lucide-react";
import { useEffect, useMemo, useState } from "react";
import type { ClipRecord, ClipSettings } from "../../../shared/types";
import { clipSourceLabels, formatClipDate, formatClipTime, formatDuration, parseClipDate } from "../../shared/clips/clipMetadata";
import { useClipIconUrl } from "../../shared/clips/useClipIconUrl";
import { clipture } from "../../platform/cliptureClient";

/** Under the video: the clip's title (renamable), where it came from, its
    details, and sending it to a friend. */
export function LibraryClipDetails({
  clip,
  settings,
  onShare
}: {
  clip: ClipRecord;
  settings?: ClipSettings;
  onShare?: (clip: ClipRecord) => void;
}) {
  const createdAt = parseClipDate(clip.createdAt);
  const displayTitle = clip.title === "Clipture clip" ? "Clipture" : clip.title;
  const [isEditingTitle, setIsEditingTitle] = useState(false);
  const [editTitle, setEditTitle] = useState(displayTitle);
  const sourceLabels = useMemo(() => clipSourceLabels(clip, settings), [clip, settings]);
  const sourceText = sourceLabels.length > 0 ? sourceLabels.join(", ") : clip.gameOrApp;
  const iconUrl = useClipIconUrl(clip, sourceLabels);

  useEffect(() => {
    if (!isEditingTitle) setEditTitle(displayTitle);
  }, [clip.id, displayTitle, isEditingTitle]);

  const handleRename = async () => {
    const newTitle = editTitle.trim() || "Clipture";
    if (newTitle !== displayTitle) {
      const success = await clipture.renameClip(clip.id, newTitle);
      if (success) clip.title = newTitle;
      else setEditTitle(displayTitle);
    } else {
      setEditTitle(displayTitle);
    }
    setIsEditingTitle(false);
  };

  return (
    <div className="library-player-details">
      <div className="library-player-side-header">
        <div className="library-player-side-title-row">
          <h2 className="library-player-side-title">
            {iconUrl && <img className="clip-app-icon" src={iconUrl} alt="" />}
            {isEditingTitle ? (
              <input
                className="library-player-title-input"
                type="text"
                value={editTitle}
                autoFocus
                aria-label="Clip title"
                onChange={(event) => setEditTitle(event.target.value)}
                onFocus={(event) => event.currentTarget.select()}
                onBlur={() => void handleRename()}
                onKeyDown={(event) => {
                  if (event.key === "Enter") {
                    event.preventDefault();
                    event.currentTarget.blur();
                  }
                  if (event.key === "Escape") {
                    event.preventDefault();
                    setEditTitle(displayTitle);
                    setIsEditingTitle(false);
                  }
                }}
              />
            ) : (
              <span className="library-player-title-copy">
                <span>{displayTitle}</span>
                <button
                  className="icon-button library-player-title-edit-button"
                  type="button"
                  title="Rename clip"
                  aria-label={`Rename ${displayTitle}`}
                  onClick={() => setIsEditingTitle(true)}
                >
                  <Edit3 size={15} />
                </button>
              </span>
            )}
          </h2>
        </div>
        <p>{sourceText}</p>
      </div>
      {onShare && (
        <button className="library-send-button" type="button" onClick={() => onShare(clip)}
          aria-label={`Send ${displayTitle} to a friend`}>
          <Send size={16} aria-hidden="true" /> Send to a friend
        </button>
      )}
      <dl className="library-player-side-meta">
        <div><dt>Recorded</dt><dd>{formatClipDate(createdAt)}, {formatClipTime(createdAt)}</dd></div>
        <div><dt>Length</dt><dd>{formatDuration(clip.durationSeconds)}</dd></div>
        <div><dt>Video</dt><dd>{clip.resolution}, {clip.fps} FPS</dd></div>
        <div><dt>Audio</dt><dd>{clip.audioTracks.length} {clip.audioTracks.length === 1 ? "track" : "tracks"}</dd></div>
      </dl>
    </div>
  );
}
