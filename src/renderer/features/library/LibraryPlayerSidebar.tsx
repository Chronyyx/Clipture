import { Check, Edit3, FolderOpen } from "lucide-react";
import { useEffect, useMemo, useState } from "react";
import type { ClipRecord, ClipSettings } from "../../../shared/types";
import { useNearViewport } from "../../shared/hooks/useNearViewport";
import { clipSourceLabels, formatClipDate, formatClipTime, formatDuration, parseClipDate } from "../../shared/clips/clipMetadata";
import { useClipIconUrl } from "../../shared/clips/useClipIconUrl";
import { clipture } from "../../platform/cliptureClient";

function ClipRailItem({
  clip,
  active,
  onSelect,
  onToggleSelected,
  selected,
  selectionMode
}: {
  clip: ClipRecord;
  active: boolean;
  onSelect: () => void;
  onToggleSelected?: () => void;
  selected: boolean;
  selectionMode: boolean;
}) {
  const [thumbnailUrl, setThumbnailUrl] = useState("");
  const [itemRef, loadMedia] = useNearViewport<HTMLDivElement>(20);
  const createdAt = parseClipDate(clip.createdAt);
  const displayTitle = clip.title === "Clipture clip" ? "Clipture" : clip.title;

  useEffect(() => {
    let mounted = true;
    if (!loadMedia) {
      setThumbnailUrl("");
      return () => {
        mounted = false;
      };
    }
    void clipture.clipThumbnailUrl(clip.filePath).then((url) => {
      if (mounted) setThumbnailUrl(url || "");
    });
    return () => {
      mounted = false;
    };
  }, [clip.filePath, loadMedia]);

  useEffect(() => {
    if (active) itemRef.current?.scrollIntoView({ block: "nearest", behavior: "smooth" });
  }, [active]);

  return (
    <div
      ref={itemRef}
      className={[
        "clip-rail-item",
        active ? "active" : "",
        selected ? "selected" : "",
        selectionMode ? "selectable" : ""
      ].filter(Boolean).join(" ")}
      aria-current={active ? "true" : undefined}
    >
      <button
        className="clip-rail-open-button"
        type="button"
        aria-pressed={selectionMode ? selected : undefined}
        onClick={selectionMode ? onToggleSelected : onSelect}
      >
        <span className="clip-rail-thumbnail">
          {selectionMode && (
            <span className={selected ? "clip-select-box checked" : "clip-select-box"} aria-hidden="true">
              {selected && <Check size={18} />}
            </span>
          )}
          {thumbnailUrl
            ? <img src={thumbnailUrl} alt="" loading="lazy" decoding="async" />
            : <span className="thumbnail-skeleton" aria-hidden="true" />}
          {clip.durationSeconds > 0 && (
            <span className="clip-rail-duration">{formatDuration(clip.durationSeconds)}</span>
          )}
        </span>
        <span className="clip-rail-copy">
          <strong>{displayTitle}</strong>
          <span>{formatClipDate(createdAt)} | {formatClipTime(createdAt)}</span>
        </span>
      </button>
      <button
        className="icon-button clip-rail-folder-button"
        type="button"
        title="Reveal clip in folder"
        aria-label={`Reveal ${displayTitle} in folder`}
        onClick={() => clipture.revealClip(clip.filePath)}
      >
        <FolderOpen size={17} />
      </button>
    </div>
  );
}

export function LibraryPlayerSidebar({
  clip,
  railClips,
  settings,
  onSelectClip,
  onToggleSelected,
  selectedClipIds,
  selectionMode = false
}: {
  clip: ClipRecord;
  railClips: ClipRecord[];
  settings?: ClipSettings;
  onSelectClip: (clip: ClipRecord) => void;
  onToggleSelected?: (clipId: string) => void;
  selectedClipIds?: ReadonlySet<string>;
  selectionMode?: boolean;
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
    <aside className="library-player-sidebar">
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
        <div className="library-player-side-meta">
          <span>{formatClipDate(createdAt)} | {formatClipTime(createdAt)}</span>
          <span>{formatDuration(clip.durationSeconds)} | {clip.resolution} | {clip.fps} FPS</span>
          <span>{clip.audioTracks.length} audio</span>
        </div>
      </div>
      <div className="clip-rail-heading">
        <strong>{selectionMode ? "Select clips" : "More clips"}</strong>
        <span>{selectionMode ? `${selectedClipIds?.size ?? 0} selected` : railClips.length}</span>
      </div>
      <div className="clip-rail" aria-label={selectionMode ? "Select clips" : "More clips"}>
        {railClips.map((candidate) => (
          <ClipRailItem
            active={candidate.id === clip.id}
            clip={candidate}
            key={candidate.id}
            onSelect={() => onSelectClip(candidate)}
            onToggleSelected={() => onToggleSelected?.(candidate.id)}
            selected={selectedClipIds?.has(candidate.id) ?? false}
            selectionMode={selectionMode}
          />
        ))}
      </div>
    </aside>
  );
}
