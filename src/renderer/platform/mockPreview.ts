import type { ClipRecord, ClipSettings } from '../../shared/types';
import { defaultSettings } from './defaultSettings';
import type { MockCliptureState } from './mock-adapter';

// Browser-only design preview (`?preview`, plus `&theme=<id>`, `&empty` and
// `&slow` for review): realistic library content for the
// mock host, never used when a desktop host is present.
// Each row: game, title, thumbnail hue, resolution, duration in seconds.
const games = [
  ['VALORANT', 'Clutch 1v4 on Ascent', 352, '1920x1080', 60],
  ['Counter-Strike 2', 'Deagle ace, Mirage B', 28, '2560x1440', 30],
  ['Rocket League', 'Ceiling shot overtime winner', 205, '1920x1080', 45],
  ['Apex Legends', 'Final ring third party', 12, '1920x1080', 60],
  ['Minecraft', 'Speedrun blaze rod RNG', 118, '1920x1080', 120],
  ['Deadlock', 'Triple kill mid lane', 262, '2560x1440', 60],
  ['Fortnite', 'Box fight win', 190, '1920x1080', 30],
  ['Elden Ring', 'Malenia no-hit phase 2', 38, '3840x2160', 90],
  ['Overwatch 2', 'Nano blade team wipe', 330, '1920x1080', 60]
] as const;

function thumbnail(hue: number, index: number) {
  const light = 'hsl(' + hue + ' 55% 58%)';
  const dark = 'hsl(' + ((hue + 40) % 360) + ' 45% 16%)';
  const svg = '<svg xmlns="http://www.w3.org/2000/svg" width="480" height="270" viewBox="0 0 480 270">' +
    '<defs><linearGradient id="g" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="' + light + '"/>' +
    '<stop offset="1" stop-color="' + dark + '"/></linearGradient></defs>' +
    '<rect width="480" height="270" fill="url(#g)"/>' +
    '<circle cx="' + (120 + index * 31) % 420 + '" cy="' + (80 + index * 47) % 200 + '" r="' + (60 + index * 9) + '" fill="#fff" fill-opacity=".10"/>' +
    '<path d="M0 ' + (190 + index * 7) + ' L480 ' + (150 + index * 11) + ' L480 270 L0 270Z" fill="#000" fill-opacity=".28"/></svg>';
  return 'data:image/svg+xml;charset=utf-8,' + encodeURIComponent(svg);
}

/** Served by clipture.app next to the demo build (web/demo-media/). */
export const DEMO_CLIP_URL = '/demo-media/sample-clip.mp4';

export function mockPreviewSeed(search: string): MockCliptureState | undefined {
  const params = new URLSearchParams(search);
  if (!params.has('preview')) return undefined;
  const now = Date.now();
  const clips: ClipRecord[] = games.map(([game, title, , resolution, durationSeconds], index) => ({
    id: 'preview-' + index,
    title,
    gameOrApp: game,
    librarySource: 'clip',
    isGame: true,
    createdAt: new Date(now - index * 5.5 * 3_600_000).toISOString(),
    durationSeconds,
    filePath: 'C:\\Users\\you\\Videos\\Clipture\\' + game + '\\clip-' + index + '.mp4',
    resolution,
    fps: 60,
    encoder: 'NVENC H.264',
    audioTracks: ['system-loopback-pcm', 'microphone-pcm', 'app:Discord.exe']
  }));
  const thumbnails = Object.fromEntries(clips.map((clip, index) => [clip.filePath, thumbnail(games[index]![2], index)]));
  const theme = params.get('theme') as ClipSettings['uiTheme'] | null;
  return {
    latencyMs: params.has('slow') ? 15000 : params.has('demo') ? 250 : 450,
    ...(params.has('demo') ? { demo: { playbackUrl: DEMO_CLIP_URL } } : {}),
    clips: params.has('empty') ? [] : clips,
    thumbnails,
    settings: {
      ...defaultSettings,
      ...(theme ? { uiTheme: theme } : {}),
      saveFolder: 'C:\\Users\\you\\Videos\\Clipture',
      audioSources: [
        ...defaultSettings.audioSources,
        { id: 'app-default-browser', label: 'Chromium', kind: 'app', processName: 'chrome.exe', enabled: true, omitIfSilent: true },
        { id: 'app-discord', label: 'Discord', kind: 'app', processName: 'Discord.exe', enabled: true, omitIfSilent: true }
      ]
    },
    diagnostics: {
      activeEncoder: 'NVENC',
      encoderMode: 'P3 · buffered',
      gpu: 'NVIDIA GeForce RTX 4070',
      status: 'Replay buffer recording',
      degraded: false
    },
    audioInputs: [
      { id: 'mic-1', name: 'Shure MV7 (USB Audio)', isDefault: true },
      { id: 'mic-2', name: 'Webcam microphone', isDefault: false }
    ],
    displays: [
      { id: 'display-1', name: 'DELL S2721DGF', width: 2560, height: 1440, x: 0, y: 0, isPrimary: true, hdr: false },
      { id: 'display-2', name: 'LG 24GN600', width: 1920, height: 1080, x: 2560, y: 0, isPrimary: false, hdr: false }
    ],
    processes: [
      { name: 'chrome.exe', pid: 4120 },
      { name: 'Discord.exe', pid: 8812 },
      { name: 'Spotify.exe', pid: 6124 },
      { name: 'VALORANT-Win64-Shipping.exe', pid: 10244 }
    ],
    sounds: [{ id: 'default.mp3', label: 'Default', builtIn: true }, { id: 'option2.wav', label: 'Soft chime', builtIn: true }]
  };
}
