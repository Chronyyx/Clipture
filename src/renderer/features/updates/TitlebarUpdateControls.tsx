import { Download, RefreshCw } from "lucide-react";
import { useState } from "react";
import type { UpdateState } from "../../../shared/types";

export const defaultUpdateState: UpdateState = { status: "idle" };

function updateButtonTitle(updateState: UpdateState): string {
  switch (updateState.status) {
    case "checking":
      return "Checking for updates";
    case "downloading":
      return updateState.message || "Downloading update";
    case "ready":
      return "Restart to install update";
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
          onClick={ready ? onInstall : (updateState.status === "available" ? onDownload : undefined)}
        >
          <Download size={16} strokeWidth={2.1} />
        </button>
      )}
    </div>
  );
}
