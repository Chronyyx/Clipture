import { Play } from "lucide-react";
import type { ClipRecord, ClipSettings } from "../../../shared/types";
import { formatDuration } from "../../shared/clips/clipMetadata";
import { ClipThumbnail } from "../../shared/clips/ClipThumbnail";
import { LibraryClipDetails } from "./LibraryClipDetails";
import { menuPointFor } from "./ClipContextMenu";
import { LibraryPlayerSidebar, type ClipActions } from "./LibraryPlayerSidebar";

export function LibraryClipPreview({
  clip,
  railClips,
  settings,
  onPlay,
  onSelectClip,
  onToggleSelected,
  selectedClipIds,
  selectionMode,
  onShare,
  actions
}: {
  actions?: ClipActions;
  clip: ClipRecord;
  railClips: ClipRecord[];
  settings?: ClipSettings;
  onPlay: () => void;
  onSelectClip: (clip: ClipRecord) => void;
  onToggleSelected: (clipId: string) => void;
  selectedClipIds: ReadonlySet<string>;
  selectionMode: boolean;
  onShare?: (clip: ClipRecord) => void;
}) {
  return (
    <section className="player panel library-player library-player-preview">
      <button
        className="library-player-main library-preview-main"
        type="button"
        onClick={onPlay}
        aria-label={`Play ${clip.title}`}
        onContextMenu={actions?.onMenu && ((event) => {
          event.preventDefault();
          actions.onMenu?.({ clip, ...menuPointFor(event) });
        })}
        onKeyDown={actions?.onDelete && ((event) => {
          if (event.key !== "Delete") return;
          event.preventDefault();
          actions.onDelete?.(clip);
        })}
      >
        <ClipThumbnail filePath={clip.filePath} eager />
        <span className="play-badge library-preview-play"><Play size={20} /></span>
        {clip.durationSeconds > 0 && (
          <span className="duration-badge">{formatDuration(clip.durationSeconds)}</span>
        )}
      </button>
      <LibraryClipDetails clip={clip} settings={settings} onShare={onShare} />
      <LibraryPlayerSidebar
        clip={clip}
        railClips={railClips}
        onSelectClip={onSelectClip}
        onToggleSelected={onToggleSelected}
        selectedClipIds={selectedClipIds}
        selectionMode={selectionMode}
        actions={actions}
      />
    </section>
  );
}
