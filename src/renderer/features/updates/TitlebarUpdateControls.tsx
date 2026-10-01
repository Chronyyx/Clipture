import { Download, RefreshCw } from "lucide-react";
import { useEffect, useRef, useState } from "react";
import { createPortal } from "react-dom";
import type { UpdateState } from "../../../shared/types";

export const defaultUpdateState: UpdateState = { status: "idle" };

function updateButtonTitle(updateState: UpdateState): string {
  switch (updateState.status) {
    case "checking":
      return "Checking for updates";
    case "downloading":
      return updateState.message || "Downloading update";
    case "ready":
      return updateState.message || "Apply update now";
    case "error":
      return updateState.message ? `Update check failed: ${updateState.message}` : "Update check failed";
    default:
      return "Check for updates";
  }
}

export function TitlebarUpdateControls({
  updateState,
  onCheck,
  onDownload,
  onInstall
}: {
  updateState: UpdateState;
  onCheck: () => void;
  onDownload: () => void;
  onInstall: () => void;
}) {
  const [forceSpin, setForceSpin] = useState(false);
  const [confirmApply, setConfirmApply] = useState(false);
  const dialog = useRef<HTMLDialogElement>(null);
  useEffect(() => {
    if (!confirmApply || updateState.status !== "ready") return;
    const previous = document.activeElement;
    dialog.current?.showModal();
    return () => {
      dialog.current?.close();
      if (previous instanceof HTMLElement && previous.isConnected) previous.focus();
    };
  }, [confirmApply, updateState.status]);

  const handleCheck = () => {
    setForceSpin(true);
    setTimeout(() => setForceSpin(false), 1000);
    onCheck();
  };

  const checking = updateState.status === "checking" || forceSpin;
  const downloading = updateState.status === "downloading";
  const ready = updateState.status === "ready";
  const error = updateState.status === "error";
  const detected = updateState.status === "available" || downloading || ready;
  
  const refreshTitle = error 
    ? (updateState.message ? `Update failed: ${updateState.message}` : "Update check failed")
    : (checking ? "Checking for updates" : "Check for updates");

  return (
    <div className="titlebar-update-controls">
      <button
        className={`titlebar-update-button refresh ${checking ? "checking" : ""} ${error && !checking ? "error" : ""}`}
        title={refreshTitle}
        aria-label={refreshTitle}
        disabled={checking || downloading}
        onClick={handleCheck}
      >
        <RefreshCw size={14} strokeWidth={2.1} />
      </button>
      {detected && (
        <button
          className={`titlebar-update-button download ${updateState.status}`}
          title={updateButtonTitle(updateState)}
          aria-label={updateButtonTitle(updateState)}
          disabled={downloading}
          onClick={ready ? () => setConfirmApply(true) : (updateState.status === "available" ? onDownload : undefined)}
        >
          <Download size={16} strokeWidth={2.1} />
        </button>
      )}
      {confirmApply && ready && createPortal(
        <dialog ref={dialog} className="modal update-confirm-dialog" aria-labelledby="update-confirm-title" aria-describedby="update-confirm-description"
          onCancel={() => setConfirmApply(false)}>
          <h2 id="update-confirm-title">Apply update now?</h2>
          <p id="update-confirm-description">{updateState.message || "Applying this update may restart Clipture and clear unsaved replay."}</p>
          <p>Save any replay you want to keep before continuing.</p>
          <button type="button" className="secondary-button" autoFocus onClick={() => setConfirmApply(false)}>Later</button>
          <button type="button" className="primary" onClick={() => { setConfirmApply(false); onInstall(); }}>Apply now</button>
        </dialog>, document.body
      )}
    </div>
  );
}
