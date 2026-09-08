import { Check, Clapperboard, Save, Search, Trash2, Upload } from "lucide-react";
import { useEffect, useMemo, useState } from "react";
import type { ClipRecord, ClipSettings } from "../../../shared/types";
import { ClipPlayer } from "../player";
import { clipSourceLabels } from "../../shared/clips/clipMetadata";
import { LibraryPlayerSidebar } from "./LibraryPlayerSidebar";
import { LibraryClipPreview } from "./LibraryClipPreview";
import { LibraryEmptyState } from "./LibraryEmptyState";
import { clipture } from "../../platform/cliptureClient";

export function LibraryView({
  clips,
  query,
  setQuery,
  selectedClip,
  setSelectedClip,
  settings,
  onSaveClip,
  onImportVideos,
  isSavingClip,
  clipLengthSeconds
}: {
  clips: ClipRecord[];
  query: string;
  setQuery: (value: string) => void;
  selectedClip: ClipRecord | undefined;
  setSelectedClip: (clip: ClipRecord | undefined) => void;
  settings?: ClipSettings;
  onSaveClip: () => void;
  onImportVideos: () => Promise<void>;
  isSavingClip: boolean;
  clipLengthSeconds: number;
}) {
  const [libraryTab, setLibraryTab] = useState<"clips" | "imported">("clips");
  const [folderFilter, setFolderFilter] = useState("");
  const [selectionMode, setSelectionMode] = useState(false);
  const [selectedClipIds, setSelectedClipIds] = useState<Set<string>>(new Set());
  const [editorialPreviewId, setEditorialPreviewId] = useState("");

  const savedClips = useMemo(() => clips.filter((clip) => clip.librarySource !== "imported"), [clips]);
  const importedClips = useMemo(() => clips.filter((clip) => clip.librarySource === "imported"), [clips]);
  const tabClips = libraryTab === "clips" ? savedClips : importedClips;
  const folderFilters = useMemo(() => {
    const seen = new Set<string>();
    return tabClips
      .map((clip) => clip.folderName || clip.gameOrApp || "Clips")
      .filter((folder) => {
        const key = folder.toLowerCase();
        if (!folder || seen.has(key)) return false;
        seen.add(key);
        return true;
      })
      .sort((a, b) => a.localeCompare(b));
  }, [tabClips]);

  useEffect(() => {
    if (folderFilter && !folderFilters.includes(folderFilter)) setFolderFilter("");
  }, [folderFilter, folderFilters]);

  useEffect(() => {
    cancelSelection();
    if (selectedClip) {
      const selectedIsImported = selectedClip.librarySource === "imported";
      if ((libraryTab === "imported") !== selectedIsImported) setSelectedClip(undefined);
    }
  }, [libraryTab]);

  const filteredClips = useMemo(() => {
    const trimmedQuery = query.trim().toLowerCase();
    return tabClips.filter((clip) => {
      const sourceLabels = clipSourceLabels(clip, settings);
      const folder = clip.folderName || clip.gameOrApp || "";
      const matchesFolder = !folderFilter || folder === folderFilter;
      const haystack = [
        clip.title,
        clip.gameOrApp,
        folder,
        clip.encoder,
        ...sourceLabels,
        ...clip.audioTracks
      ].join(" ").toLowerCase();
      return matchesFolder && (!trimmedQuery || haystack.includes(trimmedQuery));
    });
  }, [folderFilter, query, settings, tabClips]);

  const editorialPreviewClip = filteredClips.find((clip) => clip.id === editorialPreviewId)
    ?? filteredClips[0];

  const playEditorialClip = (clip: ClipRecord) => {
    setEditorialPreviewId(clip.id);
    setSelectedClip(clip);
  };

  const previewEditorialClip = (clip: ClipRecord) => {
    setEditorialPreviewId(clip.id);
    setSelectedClip(undefined);
  };

  const selectedCount = selectedClipIds.size;
  const allFilteredClipsSelected = filteredClips.length > 0
    && filteredClips.every((clip) => selectedClipIds.has(clip.id));

  const toggleClipSelection = (clipId: string) => {
    setSelectedClipIds((current) => {
      const next = new Set(current);
      if (next.has(clipId)) next.delete(clipId);
      else next.add(clipId);
      return next;
    });
  };

  const cancelSelection = () => {
    setSelectionMode(false);
    setSelectedClipIds(new Set());
  };

  const toggleSelectAllFilteredClips = () => {
    setSelectedClipIds((current) => {
      const next = new Set(current);
      if (filteredClips.every((clip) => next.has(clip.id))) {
        filteredClips.forEach((clip) => next.delete(clip.id));
      } else {
        filteredClips.forEach((clip) => next.add(clip.id));
      }
      return next;
    });
  };

  const deleteSelectedClips = async () => {
    const ids = [...selectedClipIds];
    if (ids.length === 0) return;
    const deleted = await clipture.deleteClips(ids);
    if (deleted) {
      if (selectedClip && selectedClipIds.has(selectedClip.id)) setSelectedClip(undefined);
      cancelSelection();
    }
  };

  const handleImportVideos = async () => {
    await onImportVideos();
    setLibraryTab("imported");
  };

  const emptyTitle = libraryTab === "clips" ? "No clips yet" : "No imported videos yet";
  const emptyCopy = libraryTab === "clips"
    ? "Your saved clips will appear here."
    : "Choose a folder and Clipture will read videos from it without copying them.";
  const emptyDetail = libraryTab === "clips"
    ? "Use the in-game overlay to create clips while you play."
    : "Imported videos stay in their original folders.";

  return (
    <div className="library-layout">
      <section className="browse-surface">
        <div className="library-top">
          <div className="library-hero">
            <div className="library-title">
              <Clapperboard className="library-title-icon" size={28} />
              <h1>
                <span className="library-heading-default">Clip Library</span>
                <span className="library-heading-glitten">Clips</span>
              </h1>
            </div>
            <button className="primary library-save-button" onClick={onSaveClip} disabled={isSavingClip}>
              <Save size={18} /> {isSavingClip ? "Saving..." : `Save last ${clipLengthSeconds}s`}
            </button>
          </div>

          <div className="library-tabs" role="tablist" aria-label="Library sections">
            <button
              className={libraryTab === "clips" ? "library-tab active" : "library-tab"}
              type="button"
              onClick={() => setLibraryTab("clips")}
            >
              Clips
            </button>
            <button
              className={libraryTab === "imported" ? "library-tab active" : "library-tab"}
              type="button"
              onClick={() => setLibraryTab("imported")}
            >
              Imported Videos
            </button>
          </div>

          <label className="library-search">
            <Search size={22} />
            <input value={query} onChange={(event) => setQuery(event.target.value)} placeholder="Filter by game, app, track, or title" />
          </label>

          <div className="library-actions-row">
            <div className="chip-row">
              {folderFilters.map((folder) => (
                <button
                  className={folderFilter === folder ? "chip active" : "chip"}
                  key={folder}
                  onClick={() => setFolderFilter(folderFilter === folder ? "" : folder)}
                  type="button"
                >
                  {folder}
                </button>
              ))}
            </div>
            {selectionMode ? (
              <div className="selection-bar">
                <strong>{selectedCount} selected</strong>
                <span className="selection-divider" />
                <button className="secondary-button select-all-button" type="button" onClick={toggleSelectAllFilteredClips} disabled={filteredClips.length === 0}>
                  <span className={allFilteredClipsSelected ? "select-clips-empty-box checked" : "select-clips-empty-box"} aria-hidden="true">
                    {allFilteredClipsSelected && <Check size={13} />}
                  </span>
                  {allFilteredClipsSelected ? "Deselect all" : "Select all"}
                </button>
                <button className="secondary-button" type="button" onClick={cancelSelection}>Cancel</button>
                <button className="danger-button" type="button" onClick={() => void deleteSelectedClips()} disabled={selectedCount === 0}>
                  <Trash2 size={18} /> Delete
                </button>
              </div>
            ) : (
              <div className="library-inline-actions">
                <button className="secondary-button import-videos-button" type="button" onClick={() => void handleImportVideos()}>
                  <Upload size={18} /> Import videos
                </button>
                <button
                  className="secondary-button select-clips-button"
                  type="button"
                  onClick={() => {
                    setSelectionMode(true);
                  }}
                >
                  <span className="select-clips-empty-box" aria-hidden="true" />
                  Select clips
                </button>
              </div>
            )}
          </div>
        </div>

        {filteredClips.length === 0 ? (
          <LibraryEmptyState
            title={emptyTitle}
            copy={emptyCopy}
            detail={emptyDetail}
            actionLabel={libraryTab === "clips" ? "Save your first clip" : "Import videos"}
            onAction={libraryTab === "clips" ? onSaveClip : handleImportVideos}
          />
        ) : editorialPreviewClip ? (
          selectedClip ? (
            <ClipPlayer
              clip={selectedClip}
              onClose={() => {
                setEditorialPreviewId(selectedClip.id);
                setSelectedClip(undefined);
              }}
              sidebar={<LibraryPlayerSidebar
                clip={selectedClip}
                settings={settings}
                onSelectClip={playEditorialClip}
                onToggleSelected={toggleClipSelection}
                railClips={filteredClips}
                selectedClipIds={selectedClipIds}
                selectionMode={selectionMode}
              />}
              settings={settings}
            />
          ) : (
            <LibraryClipPreview
              clip={editorialPreviewClip}
              railClips={filteredClips}
              settings={settings}
              onPlay={() => playEditorialClip(editorialPreviewClip)}
              onSelectClip={previewEditorialClip}
              onToggleSelected={toggleClipSelection}
              selectedClipIds={selectedClipIds}
              selectionMode={selectionMode}
            />
          )
        ) : null}
      </section>
    </div>
  );
}
