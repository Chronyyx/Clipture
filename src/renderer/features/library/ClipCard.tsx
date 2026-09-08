import { Check, Clock, Edit3, FolderOpen, Play } from "lucide-react";
import { useEffect, useMemo, useState } from "react";
import type { ClipRecord, ClipSettings } from "../../../shared/types";
import { useNearViewport } from "../../shared/hooks/useNearViewport";
import { clipSourceLabels, formatClipDate, formatClipTime, formatDuration, parseClipDate } from "../../shared/clips/clipMetadata";
import { useClipIconUrl } from "../../shared/clips/useClipIconUrl";
import { clipture } from "../../platform/cliptureClient";

export function ClipCard({
  clip,
  isActive,
  isSelected,
  onPlay,
  onToggleSelected,
  selectionMode,
  settings
}: {
  clip: ClipRecord;
  isActive: boolean;
  isSelected: boolean;
  onPlay: () => void;
  onToggleSelected: () => void;
  selectionMode: boolean;
  settings?: ClipSettings;
}) {
  const createdAt = parseClipDate(clip.createdAt);
  const displayTitle = clip.title === "Clipture clip" ? "Clipture" : clip.title;
  const [thumbnailUrl, setThumbnailUrl] = useState<string>("");
  const [cardRef, loadMedia] = useNearViewport<HTMLElement>();
  const [isEditingTitle, setIsEditingTitle] = useState(false);
  const [editTitle, setEditTitle] = useState(displayTitle);
  const sourceLabels = useMemo(() => clipSourceLabels(clip, settings), [clip, settings]);
  const iconUrl = useClipIconUrl(clip, sourceLabels, loadMedia);

  useEffect(() => {
    let active = true;
    if (!loadMedia) {
      setThumbnailUrl("");
      return () => {
        active = false;
      };
    }
    clipture.clipThumbnailUrl(clip.filePath).then((url) => {
      if (active && url) setThumbnailUrl(url);
    });
    return () => {
      active = false;
    };
  }, [clip.filePath, loadMedia]);

  const handleRename = async () => {
    const newTitle = editTitle.trim() || "Clipture";
    if (newTitle !== clip.title) {
      const success = await clipture.renameClip(clip.id, newTitle);
      if (success) {
        clip.title = newTitle; // optimistically update
      }
    }
    setEditTitle(newTitle);
    setIsEditingTitle(false);
  };

  useEffect(() => {
    if (isEditingTitle && editTitle !== clip.title) {
      const timer = setTimeout(() => {
        void handleRename();
      }, 3000);
      return () => clearTimeout(timer);
    }
  }, [editTitle, isEditingTitle, clip.title]);

  return (
    <article ref={cardRef} className={[isActive ? "clip-card active" : "clip-card", isSelected ? "selected" : "", selectionMode ? "selectable" : ""].filter(Boolean).join(" ")}>
      <button className="thumbnail-button" onClick={selectionMode ? onToggleSelected : onPlay}>
        {selectionMode && (
          <span className={isSelected ? "clip-select-box checked" : "clip-select-box"}>
            {isSelected && <Check size={18} />}
          </span>
        )}
        {thumbnailUrl ? <img src={thumbnailUrl} alt="" loading="eager" decoding="async" /> : <div className="thumbnail-skeleton" aria-hidden="true" />}
        {clip.durationSeconds > 0 && <span className="duration-badge">{formatDuration(clip.durationSeconds)}</span>}
        {!selectionMode && (
          <span className="play-badge">
            <Play size={18} />
          </span>
        )}
      </button>
      <div className="clip-info">
        <div className="clip-title-container">
          {isEditingTitle ? (
            <input
              type="text"
              className="clip-name-input"
              value={editTitle}
              autoFocus
              onChange={(e) => setEditTitle(e.target.value)}
              onFocus={(e) => e.target.select()}
              onBlur={handleRename}
              onKeyDown={(e) => {
                if (e.key === "Enter") handleRename();
                if (e.key === "Escape") {
                  setEditTitle(clip.title);
                  setIsEditingTitle(false);
                }
              }}
            />
          ) : (
            <div className="clip-title-display" onDoubleClick={() => setIsEditingTitle(true)}>
              {iconUrl && <img className="clip-app-icon" src={iconUrl} alt="" />}
              <span className="clip-name" title="Double click to rename">{displayTitle}</span>
              <button className="icon-button edit-title-button" title="Rename clip" onClick={(e) => { e.stopPropagation(); setIsEditingTitle(true); }}>
                <Edit3 size={15} />
              </button>
            </div>
          )}
        </div>
        <span className="clip-timestamp">{formatClipDate(createdAt)} <span>{formatClipTime(createdAt)}</span></span>
        <div className="clip-card-footer">
          <span><Clock size={14} /> {clip.fps > 0 ? `${clip.fps} FPS` : "Imported"}</span>
          <span>{clip.audioTracks.length} audio</span>
          <button className="icon-button" title="Reveal clip" onClick={(event) => { event.stopPropagation(); clipture.revealClip(clip.filePath); }}>
            <FolderOpen size={16} />
          </button>
        </div>
      </div>
    </article>
  );
}
