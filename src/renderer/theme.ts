import type { ClipSettings } from "../shared/types";

export type UiAppearance = Pick<ClipSettings, "uiTheme" | "customMainColor" | "customAccentColor">;

const storageKey = "clipture.ui-appearance.v1";
const customProperties = [
  "--app-bg",
  "--sidebar-bg",
  "--surface",
  "--surface-deep",
  "--surface-row",
  "--surface-control",
  "--surface-hover",
  "--surface-active",
  "--border",
  "--border-strong",
  "--text",
  "--text-strong",
  "--text-muted",
  "--text-subtle",
  "--icon",
  "--accent",
  "--accent-hover",
  "--accent-soft",
  "--accent-border",
  "--accent-text",
  "--on-accent",
  "--primary-bg",
  "--primary-text",
  "--selection",
  "--selection-ring",
  "--toggle-on",
  "--scrollbar",
  "--scrollbar-hover",
  "--scrollbar-active",
  "--shadow",
  "--inset-highlight"
] as const;

type Rgb = { r: number; g: number; b: number };

function parseHex(value: string): Rgb {
  const hex = value.slice(1);
  return {
    r: Number.parseInt(hex.slice(0, 2), 16),
    g: Number.parseInt(hex.slice(2, 4), 16),
    b: Number.parseInt(hex.slice(4, 6), 16)
  };
}

function toHex({ r, g, b }: Rgb): string {
  const channel = (value: number) => Math.round(Math.min(255, Math.max(0, value))).toString(16).padStart(2, "0");
  return `#${channel(r)}${channel(g)}${channel(b)}`;
}

function mix(first: string, second: string, secondWeight: number): string {
  const a = parseHex(first);
  const b = parseHex(second);
  const weight = Math.min(1, Math.max(0, secondWeight));
  return toHex({
    r: a.r + (b.r - a.r) * weight,
    g: a.g + (b.g - a.g) * weight,
    b: a.b + (b.b - a.b) * weight
  });
}

function luminance(value: string): number {
  const { r, g, b } = parseHex(value);
  const linear = (channel: number) => {
    const normalized = channel / 255;
    return normalized <= 0.04045 ? normalized / 12.92 : ((normalized + 0.055) / 1.055) ** 2.4;
  };
  return linear(r) * 0.2126 + linear(g) * 0.7152 + linear(b) * 0.0722;
}

export function normalizeThemeColor(value: unknown, fallback: string): string {
  return typeof value === "string" && /^#[0-9a-f]{6}$/i.test(value.trim())
    ? value.trim().toLowerCase()
    : fallback;
}

function clearCustomProperties(root: HTMLElement): void {
  for (const property of customProperties) root.style.removeProperty(property);
  root.style.removeProperty("color-scheme");
}

function customThemeProperties(mainColor: string, accentColor: string): Record<(typeof customProperties)[number], string> {
  const main = normalizeThemeColor(mainColor, "#101114");
  const accent = normalizeThemeColor(accentColor, "#c8a6ff");
  const isLight = luminance(main) > 0.34;
  const surface = isLight ? mix(main, "#ffffff", 0.72) : mix(main, "#ffffff", 0.025);
  const onAccent = luminance(accent) > 0.43 ? "#101114" : "#ffffff";
  const accentHover = luminance(accent) > 0.45 ? mix(accent, "#000000", 0.12) : mix(accent, "#ffffff", 0.14);

  return {
    "--app-bg": main,
    "--sidebar-bg": isLight ? mix(main, "#ffffff", 0.55) : mix(main, "#ffffff", 0.04),
    "--surface": surface,
    "--surface-deep": isLight ? mix(main, "#000000", 0.035) : mix(main, "#000000", 0.16),
    "--surface-row": isLight ? mix(main, "#ffffff", 0.42) : mix(main, "#ffffff", 0.04),
    "--surface-control": isLight ? mix(main, "#ffffff", 0.58) : mix(main, "#ffffff", 0.065),
    "--surface-hover": isLight ? mix(main, "#000000", 0.06) : mix(main, "#ffffff", 0.12),
    "--surface-active": mix(main, accent, 0.14),
    "--border": isLight ? mix(main, "#000000", 0.15) : mix(main, "#ffffff", 0.16),
    "--border-strong": isLight ? mix(main, "#000000", 0.24) : mix(main, "#ffffff", 0.25),
    "--text": isLight ? "#17191d" : "#f4f4f5",
    "--text-strong": isLight ? "#0c0d10" : "#ffffff",
    "--text-muted": isLight ? mix(main, "#17191d", 0.6) : mix(main, "#ffffff", 0.64),
    "--text-subtle": isLight ? mix(main, "#17191d", 0.46) : mix(main, "#ffffff", 0.48),
    "--icon": isLight ? mix(main, "#17191d", 0.72) : mix(main, "#ffffff", 0.78),
    "--accent": accent,
    "--accent-hover": accentHover,
    "--accent-soft": mix(surface, accent, 0.16),
    "--accent-border": mix(surface, accent, 0.44),
    "--accent-text": isLight ? mix(accent, "#000000", 0.3) : mix(accent, "#ffffff", 0.2),
    "--on-accent": onAccent,
    "--primary-bg": accent,
    "--primary-text": onAccent,
    "--selection": accent,
    "--selection-ring": mix(accent, isLight ? "#000000" : "#ffffff", 0.18),
    "--toggle-on": accent,
    "--scrollbar": isLight ? mix(main, "#000000", 0.3) : mix(main, "#ffffff", 0.3),
    "--scrollbar-hover": isLight ? mix(main, "#000000", 0.42) : mix(main, "#ffffff", 0.42),
    "--scrollbar-active": isLight ? mix(main, "#000000", 0.56) : mix(main, "#ffffff", 0.58),
    "--shadow": isLight ? "rgba(26, 32, 44, 0.16)" : "rgba(0, 0, 0, 0.42)",
    "--inset-highlight": isLight ? "rgba(255, 255, 255, 0.56)" : "rgba(255, 255, 255, 0.04)"
  };
}

export function applyUiTheme(appearance: UiAppearance): void {
  const root = document.documentElement;
  const theme = appearance.uiTheme === "light" || appearance.uiTheme === "custom" ? appearance.uiTheme : "graphite";
  clearCustomProperties(root);
  root.dataset.theme = theme;

  if (theme === "custom") {
    const properties = customThemeProperties(appearance.customMainColor, appearance.customAccentColor);
    for (const [property, value] of Object.entries(properties)) root.style.setProperty(property, value);
    root.style.setProperty("color-scheme", luminance(normalizeThemeColor(appearance.customMainColor, "#101114")) > 0.34 ? "light" : "dark");
  }
}

export function cacheAndApplyUiTheme(appearance: UiAppearance): void {
  applyUiTheme(appearance);
  try {
    localStorage.setItem(storageKey, JSON.stringify(appearance));
  } catch {
    // The settings file remains authoritative if renderer storage is unavailable.
  }
}

export function applyCachedUiTheme(): void {
  try {
    const cached = JSON.parse(localStorage.getItem(storageKey) ?? "null") as Partial<UiAppearance> | null;
    if (!cached) return;
    cacheAndApplyUiTheme({
      uiTheme: cached.uiTheme === "light" || cached.uiTheme === "custom" ? cached.uiTheme : "graphite",
      customMainColor: normalizeThemeColor(cached.customMainColor, "#101114"),
      customAccentColor: normalizeThemeColor(cached.customAccentColor, "#c8a6ff")
    });
  } catch {
    document.documentElement.dataset.theme = "graphite";
  }
}
