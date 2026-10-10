"use strict";

// Renders the Aero theme's wallpaper, assets/aero/skyline.webp: a glass
// city of the promised future under a deep blue sky: glass towers, one
// carrying sky gardens, white curved architecture, street trees, and a
// glossy plaza that mirrors it.
// The scene is drawn here as SVG (seeded, so it renders the same every
// time), rasterized in headless Microsoft Edge, and captured as a WebP,
// so the app shows one ready bitmap and draws nothing at runtime.
//
//   node scripts/assets/render-aero-scene.cjs [width height]

const { spawn } = require("node:child_process");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { pathToFileURL } = require("node:url");

const width = Number(process.argv[2] ?? 2560);
const height = Number(process.argv[3] ?? 1440);
const root = path.resolve(__dirname, "..", "..");
const output = path.join(root, "assets", "aero", "skyline.webp");
const edge = [
  "C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe",
  "C:/Program Files/Microsoft/Edge/Application/msedge.exe"
].find((candidate) => fs.existsSync(candidate));
if (!edge) throw new Error("Microsoft Edge was not found");

// mulberry32: a small seeded generator, so the skyline is stable.
function seeded(seed) {
  return () => {
    seed = (seed + 0x6d2b79f5) | 0;
    let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}
const random = seeded(20090722);
const between = (low, high) => low + (high - low) * random();
const n = (value) => Math.round(value * 10) / 10;

// The scene is drawn in a 1920x1080 frame; the horizon and the vanishing
// point of the plaza sit at HORIZON.
const HORIZON = 738;
const VANISH = 960;

// Distant towers, pale with distance.
function farSkyline() {
  let out = "";
  for (let x = -20; x < 1940;) {
    const w = between(24, 66);
    const nearCentre = 1 - Math.min(1, Math.abs(x - 980) / 900);
    const h = between(40, 120) + nearCentre * between(40, 170);
    out += `<rect x="${n(x)}" y="${n(HORIZON - h)}" width="${n(w)}" height="${n(h + 2)}" fill="url(#farGlass)"/>`;
    out += `<rect x="${n(x)}" y="${n(HORIZON - h)}" width="${n(w)}" height="${n(h + 2)}" fill="url(#mullions)" opacity="0.45"/>`;
    out += `<rect x="${n(x)}" y="${n(HORIZON - h)}" width="${n(w)}" height="2" fill="#f2fbff" opacity="0.7"/>`;
    x += w + between(-8, 10);
  }
  return out;
}

// A glass tower: a tapered face, the clouds it reflects, its mullions,
// and a sheen across it.
function tower(id, points, glass = "glass") {
  const p = points.map(([x, y]) => `${x},${y}`).join(" ");
  const xs = points.map(([x]) => x), ys = points.map(([, y]) => y);
  const left = Math.min(...xs), top = Math.min(...ys);
  return `<g id="${id}">
    <clipPath id="${id}-face"><polygon points="${p}"/></clipPath>
    <polygon points="${p}" fill="url(#${glass})"/>
    <g clip-path="url(#${id}-face)" opacity="0.38">
      <rect x="${left}" y="${top}" width="${Math.max(...xs) - left}" height="${Math.max(...ys) - top}" fill="#ffffff" filter="url(#reflected)"/>
    </g>
    <polygon points="${p}" fill="url(#mullions)"/>
    <polygon points="${p}" fill="url(#sheen)"/>
    <polygon points="${p}" fill="none" stroke="#e8f8ff" stroke-width="1.5" opacity="0.8"/>
  </g>`;
}

// Foliage: clumps in four tones laid shadow first and highlights last,
// the light ones gathered toward the sun at the upper right; the
// #foliage filter frays their edges into leaves and dapples them.
const LEAF = ["#256e27", "#47a52f", "#78d241", "#b6f466"];
function clumps(cx, cy, rx, ry, count) {
  let out = "";
  for (let tone = 0; tone < 4; tone += 1) {
    const many = Math.round(count * [1, 1, 0.7, 0.32][tone]);
    for (let k = 0; k < many; k += 1) {
      const a = between(0, Math.PI * 2), d = Math.sqrt(random()) * (1 - tone * 0.18);
      const x = cx + Math.cos(a) * d * rx + tone * 0.12 * rx;
      const y = cy + Math.sin(a) * d * ry - tone * 0.14 * ry + (tone === 0 ? ry * 0.18 : 0);
      const r = between(0.15, 0.27) * Math.min(rx, ry) * (1 - tone * 0.14);
      out += `<circle cx="${n(x)}" cy="${n(y)}" r="${n(Math.max(1.2, r))}" fill="${LEAF[tone]}"/>`;
    }
  }
  return out;
}

// A street tree: a tapering trunk that forks, and an uneven crown of a
// main mass and two side lobes.
function tree(x, size) {
  const base = HORIZON;
  const fork = base - size * 0.8;
  const lean = between(-0.08, 0.08) * size;
  return `<ellipse cx="${x}" cy="${base + 1}" rx="${n(size * 0.55)}" ry="${n(size * 0.06)}" fill="#4f7486" opacity="0.28"/>
    <path d="M ${n(x - size * 0.06)} ${base} L ${n(x - size * 0.03 + lean)} ${n(fork)} L ${n(x + size * 0.03 + lean)} ${n(fork)} L ${n(x + size * 0.06)} ${base} Z" fill="#5a4f3f"/>
    <path d="M ${n(x + lean)} ${n(fork)} L ${n(x - size * 0.3)} ${n(fork - size * 0.38)} M ${n(x + lean)} ${n(fork)} L ${n(x + size * 0.28)} ${n(fork - size * 0.42)} M ${n(x + lean)} ${n(fork)} L ${n(x + lean * 2)} ${n(fork - size * 0.6)}" stroke="#5a4f3f" stroke-width="${n(size * 0.04)}" stroke-linecap="round"/>
    <g filter="url(#foliage)">
      ${clumps(x - size * 0.36, fork - size * 0.42, size * 0.36, size * 0.3, 14)}
      ${clumps(x + size * 0.38, fork - size * 0.46, size * 0.34, size * 0.3, 14)}
      ${clumps(x + lean, fork - size * 0.72, size * 0.58, size * 0.46, 30)}
    </g>`;
}

// A hedge along a ledge, and the vines that hang over its edge.
function hedge(x0, x1, y, h) {
  let out = "";
  for (let x = x0; x < x1; x += h * 0.9) out += clumps(x + h * 0.5, y - h * 0.62, h * 0.85, h * 0.62, 11);
  return out;
}
function vines(x0, x1, y, longest) {
  let out = "";
  for (let x = x0; x < x1; x += between(3, 7)) {
    const length = between(0.25, 1) * longest;
    const sway = between(-4, 4);
    out += `<path d="M ${n(x)} ${y} q ${n(sway)} ${n(length * 0.5)} ${n(sway * 0.4)} ${n(length)}" stroke="${LEAF[random() < 0.5 ? 0 : 1]}" stroke-width="1.6" fill="none"/>`;
    for (let t = 0.15; t < 1; t += between(0.12, 0.22)) {
      out += `<circle cx="${n(x + sway * t * 0.9 + between(-1.5, 1.5))}" cy="${n(y + length * t)}" r="${n(between(1.4, 2.6))}" fill="${LEAF[Math.floor(between(0, 3.2))]}"/>`;
    }
  }
  return out;
}

// The white building at the left: crisp slabs with railings and glass
// between them, the floors swelling and narrowing along one gentle
// curve so the building reads as curved in plan, planters spilling over
// a few balconies.
function terraces() {
  let out = "", green = "";
  for (let i = 0; i < 8; i += 1) {
    const y = 418 + i * 40;
    const right = 318 + 62 * Math.sin(Math.PI * (i + 0.7) / 8.6);
    out += `<rect x="-60" y="${y + 10}" width="${n(right + 50)}" height="30" fill="url(#glass)"/>`;
    for (let c = -40; c < right - 20; c += 46) out += `<rect x="${c}" y="${y + 10}" width="3" height="30" fill="#0b3c7a" opacity="0.28"/>`;
    out += `<rect x="-60" y="${y + 10}" width="${n(right + 50)}" height="30" fill="url(#mullions)"/>`;
    out += `<rect x="${n(right - 70)}" y="${y + 10}" width="70" height="30" fill="url(#wrap)"/>`;
    out += `<rect x="-60" y="${y + 10}" width="${n(right + 50)}" height="6" fill="#0b3c7a" opacity="0.28"/>`;
    out += `<rect x="-60" y="${y}" width="${n(right + 60)}" height="10" rx="2" fill="url(#slab)"/>`;
    out += `<rect x="-60" y="${y}" width="${n(right + 58)}" height="1.2" fill="#ffffff"/>`;
    out += `<rect x="-60" y="${y - 7}" width="${n(right + 56)}" height="1.4" fill="#ffffff" opacity="0.85"/>`;
    for (let r = -50; r < right - 4; r += 15) out += `<rect x="${r}" y="${y - 7}" width="1" height="7" fill="#ffffff" opacity="0.5"/>`;
    if (i % 2 === 1 && i < 7) {
      green += hedge(right - 130, right - 14, y + 2, 10);
      green += vines(right - 124, right - 18, y + 9, 26);
    }
  }
  return out + `<g filter="url(#foliage)">${green}</g>`;
}

// The hero: a glass tower whose flank carries sky gardens on slabs that
// jut out from it, hedges on top and vines hanging over the edges. A
// thin green light runs up the corner instead of a painted fin.
function gardenTower() {
  const front = [[850, 150], [1002, 128], [1024, HORIZON], [832, HORIZON]];
  const frontLeft = (y) => 850 - 18 * (y - 150) / (HORIZON - 150);
  const sideLeft = (y) => 812 - 14 * (y - 182) / (HORIZON - 182);
  const frontRight = (y) => 1002 + 22 * (y - 128) / (HORIZON - 128);
  let out = tower("towerSide", [[812, 182], [850, 150], [832, HORIZON], [798, HORIZON]], "sideGlass");
  out += tower("towerHero", front);
  out += `<polygon points="1002,128 1014,138 1036,${HORIZON} 1024,${HORIZON}" fill="url(#column)"/>`;
  out += `<line x1="850" y1="150" x2="832" y2="${HORIZON}" stroke="#9cff45" stroke-width="2.4" filter="url(#glow)"/>`;
  // The crown: a white cap, a glass lantern set back on it, a mast.
  out += `<polygon points="844,152 1008,126 1008,120 844,146" fill="url(#slab)"/>
    <polygon points="880,141 976,126 974,98 882,108" fill="url(#glass)"/>
    <polygon points="880,141 976,126 974,98 882,108" fill="url(#mullions)"/>
    <polygon points="878,110 978,99 978,94 878,105" fill="#ffffff"/>
    <line x1="928" y1="100" x2="928" y2="34" stroke="#eef6fb" stroke-width="2.5"/>
    <circle cx="928" cy="33" r="3.4" fill="#e4fbff" filter="url(#glow)"/>`;
  let slabs = "", green = "";
  for (const [y, reach] of [[236, 66], [352, 50], [468, 74], [584, 56]]) {
    const x0 = sideLeft(y) - reach, x1 = frontLeft(y) + 10;
    slabs += `<rect x="${n(x0)}" y="${y + 9}" width="${n(x1 - x0)}" height="7" fill="#0b3c7a" opacity="0.22"/>`;
    slabs += `<rect x="${n(x0)}" y="${y}" width="${n(x1 - x0)}" height="9" rx="1.5" fill="url(#slab)"/>`;
    slabs += `<rect x="${n(x0)}" y="${y - 9}" width="${n(x1 - x0 - 8)}" height="9" fill="#ffffff" opacity="0.22"/>`;
    slabs += `<rect x="${n(x0)}" y="${y - 9}" width="${n(x1 - x0 - 8)}" height="1.2" fill="#ffffff" opacity="0.9"/>`;
    green += hedge(x0 + 2, x1 - 6, y + 2, 13);
    green += vines(x0 + 1, x0 + reach * 0.9, y + 9, 54);
  }
  for (const [y, reach] of [[300, 26], [520, 30]]) {
    const x0 = frontRight(y) + 10, x1 = x0 + reach;
    slabs += `<rect x="${n(x0 - 12)}" y="${y}" width="${n(x1 - x0 + 12)}" height="8" rx="1.5" fill="url(#slab)"/>`;
    green += hedge(x0 - 8, x1, y + 2, 10);
    green += vines(x0, x1, y + 8, 34);
  }
  return out + slabs + `<g filter="url(#foliage)">${green}</g>`;
}

// The tower right of centre: stepped setbacks with white rims, shrubs
// along the ledges, and a slim mast.
function steppedTower() {
  let out = tower("towerEast", [[1120, 330], [1232, 330], [1238, HORIZON], [1114, HORIZON]]);
  out += tower("towerEastUpper", [[1136, 290], [1216, 290], [1216, 330], [1136, 330]]);
  out += tower("towerEastTop", [[1154, 262], [1198, 262], [1198, 290], [1154, 290]]);
  out += `<rect x="1114" y="325" width="124" height="6" rx="1" fill="url(#slab)"/>
    <rect x="1131" y="286" width="90" height="5" rx="1" fill="url(#slab)"/>
    <rect x="1150" y="258" width="52" height="5" rx="1" fill="url(#slab)"/>
    <line x1="1176" y1="258" x2="1176" y2="196" stroke="#eef6fb" stroke-width="2"/>
    <circle cx="1176" cy="195" r="2.8" fill="#e4fbff" filter="url(#glow)"/>`;
  out += `<g filter="url(#foliage)">${hedge(1116, 1136, 325, 6)}${hedge(1216, 1236, 325, 6)}${hedge(1133, 1152, 286, 5)}${hedge(1200, 1219, 286, 5)}</g>`;
  return out;
}

// Lamp posts with blue banners along the right edge of the pool, nearer
// ones taller.
function lamps() {
  let out = "";
  for (const t of [0.08, 0.18, 0.32, 0.52]) {
    const y = HORIZON + 2 + (1080 - HORIZON) * t;
    const x = 982 + (760 - 982) * t + 14 + 60 * t, h = 22 + 230 * t, w = 0.6 + 3.2 * t;
    out += `<ellipse cx="${n(x + 2)}" cy="${n(y)}" rx="${n(2 + 8 * t)}" ry="${n(0.6 + 1.6 * t)}" fill="#4f7486" opacity="0.3"/>
      <rect x="${n(x - w / 2)}" y="${n(y - h)}" width="${n(w)}" height="${n(h)}" fill="url(#column)"/>
      <rect x="${n(x - h * 0.1)}" y="${n(y - h - w * 0.4)}" width="${n(h * 0.1 + w / 2)}" height="${n(w * 0.8)}" fill="#e8f1f6"/>
      <ellipse cx="${n(x - h * 0.1)}" cy="${n(y - h + w * 0.5)}" rx="${n(w * 1.4)}" ry="${n(w * 0.6)}" fill="#f4feff"/>
      <rect x="${n(x + w / 2)}" y="${n(y - h * 0.88)}" width="${n(h * 0.09)}" height="${n(h * 0.3)}" fill="url(#banner)"/>
      <path d="M ${n(x + w / 2)} ${n(y - h * 0.66)} q ${n(h * 0.03)} ${n(-h * 0.06)} ${n(h * 0.09)} ${n(-h * 0.035)}" stroke="#ffffff" stroke-width="${n(0.4 + 1.6 * t)}" fill="none" opacity="0.9"/>`;
  }
  return out;
}

// A few people about the plaza, sized by distance: legs, a torso with
// its arms, a head, and a soft shadow.
function people() {
  let out = "";
  for (const [x, y, shirt, legs] of [[1060, 760, "#ffffff", "#2b3e55"], [1095, 770, "#3a7bd5", "#e8eef2"], [1180, 788, "#ffffff", "#36506b"],
    [700, 768, "#7ed843", "#2b3e55"], [1320, 812, "#eef6fb", "#2f4a66"], [1352, 816, "#2f6cc2", "#dfe8ee"], [610, 800, "#ffffff", "#3c5570"],
    [1150, 850, "#5fbf3a", "#e9eff3"], [1470, 880, "#ffffff", "#2b3e55"]]) {
    const scale = 0.25 + (y - HORIZON) * 0.0105;
    out += `<g transform="translate(${x} ${y}) scale(${n(scale * 100) / 100})">
      <ellipse cx="3" cy="0" rx="6" ry="1.2" fill="#4f7486" opacity="0.28"/>
      <path d="M -2 0 L -1.6 -10 L 0 -10 L 0.2 -5 L 0.6 -10 L 2.2 -10 L 2 0 L 0.9 0 L 0.4 -6 L -0.8 0 Z" fill="${legs}"/>
      <path d="M -2.6 -10 Q -3 -16 -2 -19.5 Q 0 -20.6 2 -19.5 Q 3 -16 2.6 -10 Z" fill="${shirt}"/>
      <path d="M -2.4 -18.8 L -3.6 -12 M 2.4 -18.8 L 3.4 -12" stroke="${shirt}" stroke-width="1.2" stroke-linecap="round"/>
      <rect x="-0.6" y="-21" width="1.2" height="1.4" fill="#c79e7c"/>
      <ellipse cx="0" cy="-22.6" rx="1.8" ry="2.1" fill="#d9b493"/>
      <path d="M -1.9 -23 Q -1.6 -25 0 -24.9 Q 1.8 -25 1.9 -22.8 Q 1 -24 -1.9 -23 Z" fill="#4a3a2c"/>
    </g>`;
  }
  return out;
}

// Perspective lines for the plaza's tiles.
function tiles() {
  let out = "";
  for (let x = -2600; x <= 4600; x += 150) {
    out += `<line x1="${VANISH}" y1="${HORIZON}" x2="${x}" y2="1080"/>`;
  }
  for (let k = 1; k <= 16; k += 1) {
    const y = HORIZON + (1080 - HORIZON) * Math.pow(k / 16, 2.3);
    out += `<line x1="0" y1="${n(y)}" x2="1920" y2="${n(y)}"/>`;
  }
  return out;
}

const scene = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1920 1080" preserveAspectRatio="xMidYMid slice">
  <defs>
    <linearGradient id="sky" x1="0" y1="0" x2="0" y2="${HORIZON}" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="#0636b8"/>
      <stop offset="0.38" stop-color="#1563e0"/>
      <stop offset="0.75" stop-color="#3c9cf4"/>
      <stop offset="1" stop-color="#b4e2ff"/>
    </linearGradient>
    <radialGradient id="sunGlow" cx="1640" cy="80" r="760" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="#ffffff"/>
      <stop offset="0.07" stop-color="#ffffff" stop-opacity="0.9"/>
      <stop offset="0.2" stop-color="#d6f3ff" stop-opacity="0.45"/>
      <stop offset="0.5" stop-color="#9fdcff" stop-opacity="0.12"/>
      <stop offset="1" stop-color="#9fdcff" stop-opacity="0"/>
    </radialGradient>
    <linearGradient id="farGlass" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#7dbdf0"/>
      <stop offset="1" stop-color="#c6e9ff"/>
    </linearGradient>
    <linearGradient id="haze" x1="0" y1="540" x2="0" y2="${HORIZON}" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="#e2f4ff" stop-opacity="0"/>
      <stop offset="1" stop-color="#e2f4ff" stop-opacity="0.75"/>
    </linearGradient>
    <!-- Glass reflects the sky: deep at the top, pale toward the street. -->
    <linearGradient id="glass" x1="0" y1="0" x2="0.3" y2="1">
      <stop offset="0" stop-color="#1550bf"/>
      <stop offset="0.35" stop-color="#2f82e2"/>
      <stop offset="0.7" stop-color="#7cc6f7"/>
      <stop offset="1" stop-color="#cdeeff"/>
    </linearGradient>
    <linearGradient id="sheen" x1="0" y1="0" x2="1" y2="0.35">
      <stop offset="0" stop-color="#ffffff" stop-opacity="0"/>
      <stop offset="0.36" stop-color="#ffffff" stop-opacity="0"/>
      <stop offset="0.47" stop-color="#ffffff" stop-opacity="0.5"/>
      <stop offset="0.52" stop-color="#ffffff" stop-opacity="0.12"/>
      <stop offset="0.62" stop-color="#ffffff" stop-opacity="0"/>
      <stop offset="0.8" stop-color="#ffffff" stop-opacity="0.18"/>
      <stop offset="1" stop-color="#ffffff" stop-opacity="0"/>
    </linearGradient>
    <linearGradient id="sideGlass" x1="0" y1="0" x2="0.2" y2="1">
      <stop offset="0" stop-color="#0b3584"/>
      <stop offset="0.6" stop-color="#1f5fb8"/>
      <stop offset="1" stop-color="#6aaee6"/>
    </linearGradient>
    <linearGradient id="wrap" x1="0" y1="0" x2="1" y2="0">
      <stop offset="0" stop-color="#0b3c7a" stop-opacity="0"/>
      <stop offset="1" stop-color="#0b3c7a" stop-opacity="0.4"/>
    </linearGradient>
    <linearGradient id="banner" x1="0" y1="0" x2="1" y2="0">
      <stop offset="0" stop-color="#1a4fa8"/>
      <stop offset="1" stop-color="#2f7ae0"/>
    </linearGradient>
    <!-- Mullions every 10, a floor line every 17, a brighter line every
         fourth floor. -->
    <pattern id="mullions" width="10" height="68" patternUnits="userSpaceOnUse">
      <rect width="1.3" height="68" fill="#e6f7ff" opacity="0.55"/>
      <rect width="10" height="1.6" fill="#f2fbff" opacity="0.6"/>
      <rect y="17" width="10" height="1" fill="#e6f7ff" opacity="0.26"/>
      <rect y="34" width="10" height="1" fill="#e6f7ff" opacity="0.26"/>
      <rect y="51" width="10" height="1" fill="#e6f7ff" opacity="0.26"/>
    </pattern>
    <filter id="reflected" x="0" y="0" width="100%" height="100%" color-interpolation-filters="sRGB">
      <feTurbulence type="fractalNoise" baseFrequency="0.006 0.012" numOctaves="4" seed="9" result="noise"/>
      <feColorMatrix in="noise" type="matrix" values="0 0 0 0 1  0 0 0 0 1  0 0 0 0 1  4.6 0 0 0 -2.5"/>
      <feGaussianBlur stdDeviation="2"/>
    </filter>
    <filter id="foliage" x="-30%" y="-30%" width="160%" height="160%" color-interpolation-filters="sRGB">
      <feTurbulence type="fractalNoise" baseFrequency="0.24" numOctaves="2" seed="5" result="noise"/>
      <feDisplacementMap in="SourceGraphic" in2="noise" scale="6" xChannelSelector="R" yChannelSelector="G" result="leafy"/>
      <feTurbulence type="fractalNoise" baseFrequency="0.55" numOctaves="1" seed="12" result="grain"/>
      <feColorMatrix in="grain" type="matrix" values="0 0 0 0 0.86  0 0 0 0 1  0 0 0 0 0.6  2.6 0 0 0 -1.45" result="sunlit"/>
      <feComposite in="sunlit" in2="leafy" operator="in" result="dapples"/>
      <feColorMatrix in="grain" type="matrix" values="0 0 0 0 0.04  0 0 0 0 0.22  0 0 0 0 0.07  -2.6 0 0 0 0.9" result="gaps"/>
      <feComposite in="gaps" in2="leafy" operator="in" result="shade"/>
      <feMerge><feMergeNode in="leafy"/><feMergeNode in="shade"/><feMergeNode in="dapples"/></feMerge>
    </filter>
    <linearGradient id="slab" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#ffffff"/>
      <stop offset="0.6" stop-color="#eef5f9"/>
      <stop offset="1" stop-color="#c3d5e2"/>
    </linearGradient>
    <linearGradient id="whiteCurve" x1="0" y1="0" x2="1" y2="0.2">
      <stop offset="0" stop-color="#c9d9e5"/>
      <stop offset="0.25" stop-color="#ffffff"/>
      <stop offset="0.7" stop-color="#f1f6f9"/>
      <stop offset="1" stop-color="#d5e2eb"/>
    </linearGradient>
    <linearGradient id="column" x1="0" y1="0" x2="1" y2="0">
      <stop offset="0" stop-color="#cadbe6"/>
      <stop offset="0.4" stop-color="#ffffff"/>
      <stop offset="1" stop-color="#b7cbd8"/>
    </linearGradient>
    <linearGradient id="plaza" x1="0" y1="${HORIZON}" x2="0" y2="1080" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="#d4e9f5"/>
      <stop offset="0.35" stop-color="#f6fbfe"/>
      <stop offset="1" stop-color="#e3edf3"/>
    </linearGradient>
    <linearGradient id="pool" x1="0" y1="${HORIZON}" x2="0" y2="1080" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="#a6dcff"/>
      <stop offset="0.5" stop-color="#4aa6f0"/>
      <stop offset="1" stop-color="#1c78db"/>
    </linearGradient>
    <linearGradient id="contrail" x1="0" y1="0" x2="1" y2="0">
      <stop offset="0" stop-color="#ffffff" stop-opacity="0"/>
      <stop offset="1" stop-color="#ffffff" stop-opacity="0.85"/>
    </linearGradient>
    <linearGradient id="streak" x1="0" y1="0" x2="1" y2="0">
      <stop offset="0" stop-color="#ffffff" stop-opacity="0"/>
      <stop offset="0.5" stop-color="#ffffff" stop-opacity="0.55"/>
      <stop offset="1" stop-color="#ffffff" stop-opacity="0"/>
    </linearGradient>
    <linearGradient id="fade" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#ffffff" stop-opacity="0.55"/>
      <stop offset="0.45" stop-color="#ffffff" stop-opacity="0.2"/>
      <stop offset="0.75" stop-color="#ffffff" stop-opacity="1"/>
      <stop offset="1" stop-color="#ffffff" stop-opacity="0"/>
    </linearGradient>
    <mask id="cloudBand" maskContentUnits="userSpaceOnUse">
      <rect width="1920" height="${HORIZON}" fill="url(#fade)"/>
    </mask>
    <!-- Cumulus: noise thresholded into puffs, then lit as a height field
         from the sun's side, for bright tops and blue-grey bellies. -->
    <filter id="clouds" x="0" y="0" width="100%" height="100%" color-interpolation-filters="sRGB">
      <feTurbulence type="fractalNoise" baseFrequency="0.0036 0.0085" numOctaves="6" seed="4" result="noise"/>
      <feColorMatrix in="noise" type="matrix" values="0 0 0 0 1  0 0 0 0 1  0 0 0 0 1  5.6 0 0 0 -3.15" result="puffs"/>
      <feGaussianBlur in="puffs" stdDeviation="2.2" result="soft"/>
      <feDiffuseLighting in="soft" surfaceScale="8" diffuseConstant="1.2" lighting-color="#ffffff" result="lit">
        <feDistantLight azimuth="300" elevation="50"/>
      </feDiffuseLighting>
      <feColorMatrix in="lit" type="matrix" values="0.4 0 0 0 0.62  0.34 0 0 0 0.68  0.22 0 0 0 0.8  0 0 0 0 1" result="tone"/>
      <feComposite in="tone" in2="soft" operator="in"/>
    </filter>
    <filter id="blur1" x="-10%" y="-10%" width="120%" height="120%"><feGaussianBlur stdDeviation="1"/></filter>
    <filter id="blur3" x="-10%" y="-10%" width="120%" height="120%"><feGaussianBlur stdDeviation="3"/></filter>
    <filter id="blur8" x="-50%" y="-50%" width="200%" height="200%"><feGaussianBlur stdDeviation="8"/></filter>
    <filter id="glow" x="-20%" y="-20%" width="140%" height="140%">
      <feGaussianBlur stdDeviation="6" result="wide"/>
      <feMerge><feMergeNode in="wide"/><feMergeNode in="wide"/><feMergeNode in="SourceGraphic"/></feMerge>
    </filter>
    <clipPath id="floor"><rect y="${HORIZON}" width="1920" height="${1080 - HORIZON}"/></clipPath>
    <clipPath id="poolEdge"><polygon points="944,${HORIZON + 2} 982,${HORIZON + 2} 760,1080 300,1080"/></clipPath>

    <g id="city">
      ${tower("towerWest", [[560, 262], [682, 240], [692, HORIZON], [552, HORIZON]])}
      <rect x="555" y="236" width="130" height="6" rx="1" fill="url(#slab)" transform="rotate(-10 620 239)"/>
      <rect x="690" y="626" width="112" height="26" fill="url(#glass)"/>
      <rect x="690" y="626" width="112" height="26" fill="url(#mullions)"/>
      <rect x="690" y="622" width="112" height="4" fill="#ffffff"/>
      <rect x="690" y="652" width="112" height="3" fill="#ffffff" opacity="0.8"/>
      ${gardenTower()}
      <rect x="794" y="690" width="246" height="${HORIZON - 690}" fill="#0f3f86" opacity="0.3"/>
      <rect x="794" y="686" width="246" height="5" fill="#ffffff"/>
      ${steppedTower()}
      ${tower("towerFar", [[1360, 430], [1460, 420], [1466, HORIZON], [1356, HORIZON]])}
      <rect x="1354" y="${HORIZON - 4}" width="114" height="4" fill="#9cff45" filter="url(#glow)"/>
      ${terraces()}
      ${tree(468, 46)}${tree(745, 36)}${tree(1078, 32)}${tree(1300, 42)}${tree(1532, 36)}${tree(1608, 26)}
    </g>
  </defs>

  <rect width="1920" height="1080" fill="url(#sky)"/>
  <rect width="1920" height="${HORIZON}" fill="#ffffff" filter="url(#clouds)" mask="url(#cloudBand)"/>
  <rect width="1920" height="1080" fill="url(#sunGlow)"/>

  <!-- A jet and its contrail, high at the left. -->
  <path d="M -20 380 Q 200 290 470 190" fill="none" stroke="url(#contrail)" stroke-width="4" filter="url(#blur1)"/>
  <g transform="translate(474 188) rotate(-22)" fill="#f6fbff">
    <path d="M -14 0 L 12 -1.5 L 16 0 L 12 1.5 Z"/>
    <path d="M 0 -1 L -5 -10 L -2 -10 L 5 -1 Z M 0 1 L -5 10 L -2 10 L 5 1 Z"/>
  </g>

  ${farSkyline()}
  <rect y="520" width="1920" height="${HORIZON - 518}" fill="url(#haze)"/>

  <use href="#city"/>

  <!-- The plaza: glossy white tiles mirroring the city. -->
  <rect y="${HORIZON}" width="1920" height="${1080 - HORIZON}" fill="url(#plaza)"/>
  <g clip-path="url(#floor)" opacity="0.2" filter="url(#blur3)">
    <use href="#city" transform="translate(0 ${HORIZON * 2}) scale(1 -1)"/>
  </g>
  <g clip-path="url(#floor)" stroke="#a9c4d6" stroke-width="1" opacity="0.5">${tiles()}</g>

  <!-- A reflecting pool running out toward the viewer. -->
  <polygon points="944,${HORIZON + 2} 982,${HORIZON + 2} 760,1080 300,1080" fill="url(#pool)"/>
  <g clip-path="url(#poolEdge)" opacity="0.5" filter="url(#blur3)">
    <use href="#city" transform="translate(0 ${HORIZON * 2}) scale(1 -1)"/>
  </g>
  <g clip-path="url(#poolEdge)" fill="#ffffff">
    <ellipse cx="760" cy="840" rx="90" ry="2" opacity="0.5"/>
    <ellipse cx="640" cy="930" rx="140" ry="3" opacity="0.4"/>
    <ellipse cx="520" cy="1030" rx="190" ry="4" opacity="0.35"/>
    <ellipse cx="860" cy="780" rx="40" ry="1.2" opacity="0.6"/>
  </g>
  <polyline points="944,${HORIZON + 2} 300,1080" fill="none" stroke="#ffffff" stroke-width="3"/>
  <polyline points="982,${HORIZON + 2} 760,1080" fill="none" stroke="#ffffff" stroke-width="3"/>

  ${lamps()}
  ${people()}

  <!-- The white curved canopy in the foreground at the right, its glass
       band, the green light strip along its edge, and its column. -->
  <rect x="1752" y="600" width="46" height="400" fill="url(#column)"/>
  <ellipse cx="1775" cy="1000" rx="40" ry="6" fill="#7f9fb4" opacity="0.35" filter="url(#blur3)"/>
  <path d="M 1920 0 L 1690 0 C 1590 130 1572 310 1636 470 C 1688 598 1800 664 1920 684 Z" fill="url(#whiteCurve)"/>
  <path d="M 1920 0 L 1762 0 C 1680 140 1664 300 1712 420 C 1756 520 1840 566 1920 580 Z" fill="url(#glass)"/>
  <path d="M 1920 0 L 1762 0 C 1680 140 1664 300 1712 420 C 1756 520 1840 566 1920 580 Z" fill="url(#mullions)"/>
  <path d="M 1920 0 L 1762 0 C 1680 140 1664 300 1712 420 C 1756 520 1840 566 1920 580 Z" fill="url(#sheen)"/>
  <g stroke="#ffffff" stroke-width="5" fill="none" opacity="0.85">
    <path d="M 1700 120 C 1780 128 1860 132 1920 132"/>
    <path d="M 1680 250 C 1770 262 1850 268 1920 268"/>
    <path d="M 1690 380 C 1770 400 1850 410 1920 412"/>
  </g>
  <path d="M 1636 470 C 1688 598 1800 664 1920 684" fill="none" stroke="#9cff45" stroke-width="7" filter="url(#glow)"/>
  <path d="M 1640 478 C 1692 604 1802 670 1920 690" fill="none" stroke="#ffffff" stroke-width="2" opacity="0.8"/>

  <!-- The sun's flare: its core, a streak through it, and ghosts. -->
  <circle cx="1640" cy="80" r="54" fill="#ffffff" filter="url(#blur8)"/>
  <rect x="1040" y="77" width="1200" height="6" fill="url(#streak)" transform="rotate(-28 1640 80)" filter="url(#blur1)"/>
  <circle cx="1420" cy="250" r="22" fill="#bff8ff" opacity="0.2"/>
  <circle cx="1300" cy="345" r="9" fill="#ddffb0" opacity="0.28"/>
  <circle cx="1150" cy="460" r="38" fill="#a9e6ff" opacity="0.1"/>
</svg>`;

// The light over the scene: a bloom from the sun and a soft vignette.
const page = `<!doctype html>
<style>
  html, body { margin: 0; height: 100%; overflow: hidden; background: #1563e0; }
  .layer { position: absolute; inset: 0; width: 100%; height: 100%; }
  svg.layer { display: block; filter: saturate(1.08) contrast(1.04); }
  .bloom { background: radial-gradient(ellipse 60% 55% at 86% 6%, rgb(220 245 255 / 38%), transparent 70%); mix-blend-mode: screen; }
  .vignette { background: radial-gradient(ellipse 85% 75% at 50% 45%, transparent 60%, rgb(0 30 80 / 22%)); }
</style>
${scene.replace("<svg ", '<svg class="layer" ')}
<div class="layer bloom"></div>
<div class="layer vignette"></div>`;

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function main() {
  const port = 9300 + Math.floor(Math.random() * 400);
  const profile = fs.mkdtempSync(path.join(os.tmpdir(), "aero-render-"));
  const browser = spawn(edge, [
    "--headless=new", `--remote-debugging-port=${port}`, `--user-data-dir=${profile}`,
    `--window-size=${width},${height}`, "--force-device-scale-factor=1", "about:blank"
  ], { stdio: "ignore" });
  try {
    let target;
    for (let attempt = 0; attempt < 50 && !target; attempt += 1) {
      try {
        target = (await (await fetch(`http://127.0.0.1:${port}/json`)).json()).find((entry) => entry.type === "page");
      } catch { /* not listening yet */ }
      if (!target) await sleep(200);
    }
    if (!target) throw new Error("Edge did not start");
    const socket = new WebSocket(target.webSocketDebuggerUrl);
    await new Promise((resolve) => { socket.onopen = resolve; });
    let id = 0;
    const pending = new Map();
    socket.onmessage = (message) => {
      const data = JSON.parse(message.data);
      pending.get(data.id)?.(data);
      pending.delete(data.id);
    };
    const send = (method, params = {}) => new Promise((resolve) => {
      id += 1;
      pending.set(id, resolve);
      socket.send(JSON.stringify({ id, method, params }));
    });
    await send("Emulation.setDeviceMetricsOverride", { width, height, deviceScaleFactor: 1, mobile: false });
    const file = path.join(profile, "scene.html");
    fs.writeFileSync(file, page);
    await send("Page.navigate", { url: pathToFileURL(file).href });
    await sleep(500);
    await send("Runtime.evaluate", {
      expression: "new Promise((done) => requestAnimationFrame(() => requestAnimationFrame(done)))",
      awaitPromise: true
    });
    await sleep(1500);
    const shot = await send("Page.captureScreenshot", { format: "webp", quality: 90 });
    if (!shot.result?.data) throw new Error("Capture failed");
    fs.writeFileSync(output, Buffer.from(shot.result.data, "base64"));
    socket.close();
    console.log(`Wrote ${path.relative(root, output)} (${width}x${height}, ${Math.round(fs.statSync(output).size / 1024)} KiB)`);
  } finally {
    const exited = new Promise((resolve) => browser.once("exit", resolve));
    browser.kill();
    await exited;
    fs.rmSync(profile, { recursive: true, force: true, maxRetries: 5, retryDelay: 200 });
  }
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
