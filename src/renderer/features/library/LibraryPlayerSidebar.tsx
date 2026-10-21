import { Check, FolderOpen } from "lucide-react";
import { useEffect } from "react";
import type { ClipRecord } from "../../../shared/types";
import { useNearViewport } from "../../shared/hooks/useNearViewport";
import { formatClipDate, formatClipTime, formatDuration, parseClipDate } from "../../shared/clips/clipMetadata";
import { useClipThumbnail } from "../../shared/clips/useClipThumbnail";
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
  const [itemRef, loadMedia] = useNearViewport<HTMLDivElement>(20);
  const thumbnailUrl = useClipThumbnail(clip.filePath, loadMedia);
  const createdAt = parseClipDate(clip.createdAt);
  const displayTitle = clip.title === "Clipture clip" ? "Clipture" : clip.title;

  // Scroll only the rail: scrollIntoView would also move every scrollable
  // ancestor, including a page that embeds the app.
  useEffect(() => {
    const item = itemRef.current;
    const rail = item?.closest<HTMLElement>(".clip-rail");
    if (!active || !item || !rail) return;
    const itemBox = item.getBoundingClientRect();
    const railBox = rail.getBoundingClientRect();
    const offset = itemBox.top < railBox.top
      ? itemBox.top - railBox.top
      : itemBox.bottom > railBox.bottom ? itemBox.bottom - railBox.bottom : 0;
    if (offset !== 0) rail.scrollBy({ top: offset, behavior: "smooth" });
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
          <span><span className="clip-rail-when">{formatClipDate(createdAt)},</span> <span className="clip-rail-when">{formatClipTime(createdAt)}</span></span>
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

/** Beside the video: every clip in the current view, the playing one marked. */
export function LibraryPlayerSidebar({
  clip,
  railClips,
  onSelectClip,
  onToggleSelected,
  selectedClipIds,
  selectionMode = false
}: {
  clip: ClipRecord;
  railClips: ClipRecord[];
  onSelectClip: (clip: ClipRecord) => void;
  onToggleSelected?: (clipId: string) => void;
  selectedClipIds?: ReadonlySet<string>;
  selectionMode?: boolean;
}) {
  return (
    <aside className="library-player-sidebar">
      <div className="clip-rail-heading">
        <strong>{selectionMode ? "Select clips" : "Clips"}</strong>
        <span>{selectionMode ? `${selectedClipIds?.size ?? 0} selected` : railClips.length}</span>
      </div>
      <div className="clip-rail" aria-label={selectionMode ? "Select clips" : "Clips"}>
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
