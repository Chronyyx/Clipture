import { Play } from "lucide-react";
import { useEffect, useState } from "react";
import type { ClipRecord, ClipSettings } from "../../../shared/types";
import { formatDuration } from "../../shared/clips/clipMetadata";
import { LibraryPlayerSidebar } from "./LibraryPlayerSidebar";
import { clipture } from "../../platform/cliptureClient";

export function LibraryClipPreview({
  clip,
  railClips,
  settings,
  onPlay,
  onSelectClip,
  onToggleSelected,
  selectedClipIds,
  selectionMode
}: {
  clip: ClipRecord;
  railClips: ClipRecord[];
  settings?: ClipSettings;
  onPlay: () => void;
  onSelectClip: (clip: ClipRecord) => void;
  onToggleSelected: (clipId: string) => void;
  selectedClipIds: ReadonlySet<string>;
  selectionMode: boolean;
}) {
  const [thumbnailUrl, setThumbnailUrl] = useState("");

  useEffect(() => {
    let mounted = true;
    setThumbnailUrl("");
    void clipture.clipThumbnailUrl(clip.filePath).then((url) => {
      if (mounted) setThumbnailUrl(url || "");
    });
    return () => {
      mounted = false;
    };
  }, [clip.filePath]);

  return (
    <section className="player panel library-player library-player-preview">
      <button className="library-player-main library-preview-main" type="button" onClick={onPlay} aria-label={`Play ${clip.title}`}>
        {thumbnailUrl
          ? <img src={thumbnailUrl} alt="" decoding="async" />
          : <span className="thumbnail-skeleton" aria-hidden="true" />}
        <span className="play-badge library-preview-play"><Play size={20} /></span>
        {clip.durationSeconds > 0 && (
          <span className="duration-badge">{formatDuration(clip.durationSeconds)}</span>
        )}
      </button>
      <LibraryPlayerSidebar
        clip={clip}
        railClips={railClips}
        settings={settings}
        onSelectClip={onSelectClip}
        onToggleSelected={onToggleSelected}
        selectedClipIds={selectedClipIds}
        selectionMode={selectionMode}
      />
    </section>
  );
}
