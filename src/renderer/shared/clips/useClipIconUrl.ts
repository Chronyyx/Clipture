import { useEffect, useState } from "react";
// @ts-ignore
import logoUrl from "../../../../assets/clipture-logo-ui.png";
import type { ClipRecord } from "../../../shared/types";
import { clipture } from "../../platform/cliptureClient";

export function useClipIconUrl(clip: ClipRecord, preferredLabels: string[], enabled = true): string {
  const [iconUrl, setIconUrl] = useState("");
  const focusedAppsKey = (clip.focusedApps ?? []).join("|");
  const audioTracksKey = clip.audioTracks.join("|");
  const preferredLabelsKey = preferredLabels.join("|");

  useEffect(() => {
    let active = true;
    if (!enabled) {
      setIconUrl("");
      return () => {
        active = false;
      };
    }
    if (preferredLabels.some((label) => label.trim().toLowerCase() === "clipture")) {
      setIconUrl(logoUrl);
      return () => {
        active = false;
      };
    }
    setIconUrl("");
    clipture.clipIconUrl(clip, preferredLabels).then((url) => {
      if (active) setIconUrl(url || "");
    }).catch(() => {
      if (active) setIconUrl("");
    });
    return () => {
      active = false;
    };
  }, [clip.id, clip.gameOrApp, focusedAppsKey, audioTracksKey, preferredLabelsKey, enabled]);

  return iconUrl;
}
