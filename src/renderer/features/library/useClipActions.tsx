import { useState } from "react";
import type { ClipRecord } from "../../../shared/types";
import { clipture } from "../../platform/cliptureClient";
import { ClipContextMenu, type ClipMenuRequest } from "./ClipContextMenu";
import { DeleteClipsDialog } from "./DeleteClipsDialog";
import type { ClipActions } from "./LibraryPlayerSidebar";

/** The library's clip actions: the right-click menu, the Delete key, and the
 * confirmation every delete goes through. Render `overlays` once. */
export function useClipActions({
  onPlay,
  onShare,
  onDeleted
}: {
  onPlay: (clip: ClipRecord) => void;
  onShare?: (clip: ClipRecord) => void;
  /** After the files are gone, with the deleted clips' ids. */
  onDeleted: (ids: string[]) => void;
}) {
  const [menu, setMenu] = useState<ClipMenuRequest>();
  const [pendingDelete, setPendingDelete] = useState<ClipRecord[]>();

  const confirmDelete = (clips: ClipRecord[]) => {
    if (clips.length > 0) setPendingDelete(clips);
  };
  const actions: ClipActions = {
    onMenu: setMenu,
    onDelete: (clip) => confirmDelete([clip])
  };

  const overlays = (
    <>
      {menu && (
        <ClipContextMenu
          request={menu}
          onClose={() => setMenu(undefined)}
          onPlay={onPlay}
          onReveal={(clip) => void clipture.revealClip(clip.filePath)}
          onShare={onShare}
          onDelete={(clip) => confirmDelete([clip])}
        />
      )}
      {pendingDelete && (
        <DeleteClipsDialog
          clips={pendingDelete}
          onCancel={() => setPendingDelete(undefined)}
          onConfirm={async () => {
            const ids = pendingDelete.map((clip) => clip.id);
            if (await clipture.deleteClips(ids)) onDeleted(ids);
            setPendingDelete(undefined);
          }}
        />
      )}
    </>
  );

  return { actions, confirmDelete, overlays };
}
