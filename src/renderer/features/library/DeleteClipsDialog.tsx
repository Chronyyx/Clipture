import { Trash2 } from "lucide-react";
import { useEffect, useRef, useState } from "react";
import { createPortal } from "react-dom";
import type { ClipRecord } from "../../../shared/types";
import { userMessage } from "../../platform";

/** Asks before deleting: the video files are removed from the PC. Cancel has
 * focus, so Enter or Escape never deletes by accident. */
export function DeleteClipsDialog({
  clips,
  onCancel,
  onConfirm
}: {
  clips: ClipRecord[];
  onCancel: () => void;
  onConfirm: () => Promise<void>;
}) {
  const dialog = useRef<HTMLDialogElement>(null);
  const [deleting, setDeleting] = useState(false);
  const [error, setError] = useState("");

  useEffect(() => {
    const previous = document.activeElement;
    dialog.current?.showModal();
    return () => {
      dialog.current?.close();
      if (previous instanceof HTMLElement && previous.isConnected) previous.focus();
    };
  }, []);

  const single = clips.length === 1;
  const title = single ? `Delete “${clips[0].title}”?` : `Delete ${clips.length} clips?`;
  const imported = clips.some((clip) => clip.id.startsWith("imported:"));

  return createPortal(
    <dialog
      ref={dialog}
      className="modal delete-clips-dialog"
      aria-labelledby="delete-clips-title"
      aria-describedby="delete-clips-description"
      onCancel={(event) => {
        event.preventDefault();
        if (!deleting) onCancel();
      }}
    >
      <h2 id="delete-clips-title">{title}</h2>
      <p id="delete-clips-description">
        {single ? "The video file is" : "Their video files are"} deleted from your PC
        {imported ? ", including imported videos in their own folders" : ""}. This can't be undone.
      </p>
      {error && <p className="delete-clips-error" role="alert">{error}</p>}
      <div className="delete-clips-actions">
        <button type="button" className="secondary-button" autoFocus disabled={deleting} onClick={onCancel}>
          Cancel
        </button>
        <button
          type="button"
          className="danger-button"
          disabled={deleting}
          onClick={() => {
            setDeleting(true);
            setError("");
            void onConfirm()
              .catch((reason: unknown) => setError(userMessage(reason, "Clipture could not delete the clips.")))
              .finally(() => setDeleting(false));
          }}
        >
          <Trash2 size={16} /> {deleting ? "Deleting…" : "Delete"}
        </button>
      </div>
    </dialog>,
    document.body
  );
}
