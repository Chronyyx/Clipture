import type { ClipRecord, ClipSettings } from "../../../shared/types";

export function uniqueLabels(labels: Array<string | null | undefined>): string[] {
  const seen = new Set<string>();
  return labels
    .map((label) => (label ?? "").trim())
    .filter((label) => {
      const key = label.toLowerCase();
      if (!label || seen.has(key)) return false;
      seen.add(key);
      return true;
    });
}

export function clipSourceLabels(clip: ClipRecord, settings?: ClipSettings): string[] {
  if (clip.focusedApps && clip.focusedApps.length > 0) {
    return uniqueLabels(clip.focusedApps);
  }

  const bgApps = new Set(
    settings?.audioSources
      .filter((source) => source.kind === "app" && source.processName)
      .flatMap((source) => [`app:${source.processName}`, source.processName!.replace(/\.exe$/i, "")]) || []
  );
  const activeTracks = clip.audioTracks
    .filter((track) =>
      track !== "system-loopback-pcm" &&
      track !== "System audio" &&
      track !== "microphone-pcm" &&
      track !== "Microphone" &&
      track !== "mixed-preview-pcm" &&
      !bgApps.has(track)
    )
    .map((track) =>
      track.startsWith("app:")
        ? track.substring(4).replace(/\.exe$/i, "")
        : track.startsWith("game:")
          ? track.substring(5).replace(/\.exe$/i, "")
          : track
    );

  return uniqueLabels([
    clip.gameOrApp !== "Foreground app" ? clip.gameOrApp : null,
    ...activeTracks
  ]);
}

export function displayAudioTrackName(track: string) {
  if (track === "microphone-pcm") return "Microphone";
  if (track === "system-loopback-pcm") return "System audio";
  if (track === "mixed-preview-pcm") return "Mixed preview";
  if (track.startsWith("app:")) return track.slice(4).replace(/\.exe$/i, "");
  if (track.startsWith("game:")) return track.slice(5).replace(/\.exe$/i, "");
  return track;
}

export function displayAudioTracks(tracks: string[]) {
  return tracks.map(displayAudioTrackName).join(", ");
}

export function parseClipDate(value: string) {
  const numeric = /^\d+$/.test(value) ? Number(value) : Number.NaN;
  const date = Number.isFinite(numeric)
    ? new Date(value.length <= 10 ? numeric * 1000 : numeric)
    : new Date(value);
  return Number.isNaN(date.getTime()) ? undefined : date;
}

export function formatClipDate(date: Date | undefined) {
  if (!date) return "Unknown date";
  const year = date.getFullYear();
  const month = String(date.getMonth() + 1).padStart(2, "0");
  const day = String(date.getDate()).padStart(2, "0");
  return `${year}/${month}/${day}`;
}

export function formatClipTime(date: Date | undefined) {
  if (!date) return "Unknown time";
  return new Intl.DateTimeFormat("en-US", {
    hour: "numeric",
    minute: "2-digit",
    second: "2-digit",
    hour12: true
  }).format(date);
}

export function formatDuration(seconds: number) {
  const safeSeconds = Math.max(0, Math.round(seconds));
  const days = Math.floor(safeSeconds / 86400);
  const hours = Math.floor((safeSeconds % 86400) / 3600);
  const minutes = Math.floor((safeSeconds % 3600) / 60);
  const remainder = safeSeconds % 60;
  const clock = `${String(minutes).padStart(2, "0")}:${String(remainder).padStart(2, "0")}`;
  if (days > 0) return `${days}d ${String(hours).padStart(2, "0")}:${clock}`;
  if (hours > 0) return `${hours}:${clock}`;
  return `${minutes}:${String(remainder).padStart(2, "0")}`;
}
