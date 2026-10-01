const relative = new Intl.RelativeTimeFormat(undefined, { numeric: "auto" });

export function formatSharedAt(ms: number, now = Date.now()) {
  const minutes = Math.round((ms - now) / 60_000);
  if (Math.abs(minutes) < 1) return "just now";
  if (Math.abs(minutes) < 60) return relative.format(minutes, "minute");
  const hours = Math.round(minutes / 60);
  if (Math.abs(hours) < 24) return relative.format(hours, "hour");
  return relative.format(Math.round(hours / 24), "day");
}

export function formatBytes(bytes: number) {
  if (bytes >= 1_000_000_000) return `${(bytes / 1_000_000_000).toFixed(1)} GB`;
  return `${Math.max(1, Math.round(bytes / 1_000_000))} MB`;
}

/** Four-character groups are easier to read aloud or compare by eye. */
export function codeGroups(code: string) {
  return code.match(/.{1,4}/g) ?? [];
}

export function possessive(name: string) {
  return name.endsWith("s") ? `${name}'` : `${name}'s`;
}
