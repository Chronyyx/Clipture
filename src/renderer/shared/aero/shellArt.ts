// The Aero theme's shell: the one moulded object the whole interface is
// formed in, after the old media player. Drawn as an SVG at the window's
// own size and pixel ratio, so every edge is sharp at any scale.
//
// ARCHITECTURE
//
// One piece of pale silver-blue plastic inside a rolled rim, lit from
// above. Its silhouette: a rounded corner at the top left, then a shallow
// scoop over the name before the top rises to the body's level; an S up
// to a raised platform at the top right where the window keys sit; a
// wide rounded corner at the bottom left, and an S down from the wing's
// foot to the body's. Formed in it:
//
//   the cap      a glossy blue lobe at the top left that carries the name
//   the pocket   a tray of periwinkle gel on the wing; the taskbar
//   the well     a dark recess under the pocket; the recorder's status
//   the floor    the deep blue working face; the workspace lies on it
//
// The rim is metal, and metal is read by its creases: each side has a
// sharp crest where the surface turns from facing the light to facing
// away, and the shading jumps there rather than fading. Every side's rim
// is painted from a colour profile measured across that side of the
// reference design, one entry per reference pixel (1.3 window pixels),
// as concentric bands of the edge it is measured from:
//
//   top     the outline: a blue lip, a silver roll, a dark seam
//   left    the outline: a silver bead lit at its outer edge
//   right   the outline: a silver bead with a white crest, a blue lip
//   foot    the groove, a line inside the outline: below it a band of
//           blue gel, deep under the wing and a sliver along the body;
//           above it a shaded lower face, a crest, a lit top face, and a
//           second crest at the floor's lip
//
// Sides blend into each other across their corners.
//
// GEOMETRY is in pixels from the shell's upper left as it sits in a
// 1220 x 763 window (1138.5 x 719.3). Points to the right of SHELL.w less
// ANCHOR.right keep their distance from the right edge; points below
// SHELL.h less ANCHOR.bottom keep theirs from the bottom. The workspace's
// margins in aero.css match FLOOR: change them together.

const SHELL = { w: 1138.5, h: 719.3 };
const ANCHOR = { right: 260, bottom: 185 };
/** Where the art's shape changes, in pixels from each edge of the shell:
 * between these lines every edge runs straight, so a drawing can be
 * stretched there (cut in nine) without bending a curve. */
export const SHELL_SLICES = { left: 400, top: 140, right: ANCHOR.right, bottom: ANCHOR.bottom };
const REF = 1.3; // reference pixels per window pixel

type Point = [number, number];

const n = (value: number) => Math.round(value * 100) / 100;
const smooth = (t: number) => { const c = Math.min(1, Math.max(0, t)); return c * c * (3 - 2 * c); };

// A superellipse corner at vertex (vx, vy), from reachA along unit vector a
// to reachB along unit vector b (both pointing away from the vertex).
function corner(vx: number, vy: number, reachA: number, reachB: number, a: Point, b: Point, exponent = 2.6, steps = 28): Point[] {
  const k = 2 / exponent;
  const points: Point[] = [];
  for (let i = 0; i <= steps; i += 1) {
    const angle = (Math.PI / 2) * (i / steps);
    const alongA = reachA * (1 - Math.sin(angle) ** k);
    const alongB = reachB * (1 - Math.cos(angle) ** k);
    points.push([vx + alongA * a[0] + alongB * b[0], vy + alongA * a[1] + alongB * b[1]]);
  }
  return points;
}

// A smooth run through traced points (Catmull-Rom), endpoints included.
function spline(points: Point[], perSegment = 8): Point[] {
  const out: Point[] = [];
  for (let i = 0; i < points.length - 1; i += 1) {
    const p0 = points[Math.max(0, i - 1)], p1 = points[i], p2 = points[i + 1], p3 = points[Math.min(points.length - 1, i + 2)];
    for (let s = 0; s < perSegment; s += 1) {
      const t = s / perSegment, t2 = t * t, t3 = t2 * t;
      const at = (d: 0 | 1) => 0.5 * ((2 * p1[d]) + (-p0[d] + p2[d]) * t + (2 * p0[d] - 5 * p1[d] + 4 * p2[d] - p3[d]) * t2 + (-p0[d] + 3 * p1[d] - 3 * p2[d] + p3[d]) * t3);
      out.push([at(0), at(1)]);
    }
  }
  out.push(points[points.length - 1]);
  return out;
}

// Splits long segments so a closed outline can be offset smoothly.
function resample(points: Point[], step: number): Point[] {
  const out: Point[] = [];
  for (let i = 0; i < points.length; i += 1) {
    const [x0, y0] = points[i], [x1, y1] = points[(i + 1) % points.length];
    const parts = Math.max(1, Math.ceil(Math.hypot(x1 - x0, y1 - y0) / step));
    for (let s = 0; s < parts; s += 1) out.push([x0 + ((x1 - x0) * s) / parts, y0 + ((y1 - y0) * s) / parts]);
  }
  return out;
}

// Moves each point of a clockwise closed outline inward by inset(point).
function offsetInward(points: Point[], inset: (p: Point) => number): Point[] {
  const count = points.length;
  return points.map((p, i) => {
    const [ax, ay] = points[(i - 2 + count) % count], [bx, by] = points[(i + 2) % count];
    const length = Math.hypot(bx - ax, by - ay) || 1;
    const d = inset(p);
    return [p[0] - ((by - ay) / length) * d, p[1] + ((bx - ax) / length) * d];
  });
}

// The silhouette, traced. Top edge left to right: the scoop over the name,
// then the body's top; the S up to the platform; the platform.
const TOP_RUN: Point[] = [[47, 23.1], [84, 26.2], [130, 30], [148.5, 30], [167, 28.5], [190, 25.4], [213, 21.6], [236, 18.5], [268.5, 17.7], [300, 17.7]];
const PLATFORM_RUN: Point[] = [[880, 17.7], [890, 17.4], [913, 14.6], [941, 6.9], [960, 3], [976, 1.2], [1000, 0.8]];
// Bottom edge right to left from the body's foot: the S up to the wing.
const FOOT_RUN: Point[] = [[300, 719.3], [240.8, 719.3], [222, 717.5], [204, 714], [186, 709], [168, 704.6], [148, 702], [127, 700.8], [100, 700.3], [73, 700.2]];
// The top's one crest beyond the body: it leaves the roll's crest and
// climbs the S to become the platform's edge. Around it is only shading.
const STREAK: Point[] = [[800, 36.4], [840, 36], [870, 34], [898, 28.5], [922, 19.5], [944, 11.5], [965, 6.5], [990, 4.6]];
// The cap's foot, from the left rim, level under the name, then up to run
// out under the rim.
const CAP_FOOT: Point[] = [[-2, 132], [3.9, 127.7], [8.5, 110.8], [13, 102.3], [22.3, 93.9], [31.5, 89.3], [50, 87.5], [80, 87], [120, 86.8], [137.7, 84.6], [156, 80.5], [174.6, 73.9], [193, 62.3], [211.5, 47], [230, 35.4], [248.5, 28.5], [267, 26.2], [290, 25.2], [320, 24.6]];
// The glass sheen across the pocket: its lower edge, rising to the right.
const SHEEN: Point[] = [[0, 445], [34.6, 429.3], [103.9, 390.8], [180.8, 344.6], [219.2, 313.9], [240, 296]];
const POCKET = { left: 12.3, right: 219.2, top: 104.6, footLeft: 563.9, footRight: 580 };
const WELL = { left: 19.2, right: 219.2, topLeft: 570, topRight: 586, bottom: 645.4 };
const FLOOR = { left: 224.6, top: 43.9, right: 1122.7, bottom: 690.3 };

// How far the groove sits inside the outline: a sliver along the body's
// foot, deep under the wing, the left bead's width up the left side.
const GROOVE = { body: 3.1, wing: 18.4, left: 7.7 };

// Profiles, one colour per reference pixel from the edge they are measured
// from. "face" marks the foot's lit top face, painted with a gradient whose
// highlight sits toward the middle of the body.
const LEFT_BEAD = ["#60709c", "#a7b4d3", "#b9c8e6", "#bccae8", "#c2ceea", "#d8e2f5", "#f4f6fe", "#d3d7e8", "#a9b6d5", "#97a8cf", "#7a8bb7", "#6e80ac", "#7284af", "#687ba9", "#4e66a4"];
const RIGHT_BEAD = ["#4f659c", "#7a90c4", "#7d94c9", "#7890c8", "#7893cc", "#8ca5db", "#a4b7e4", "#d4e0f8", "#e7f3fe", "#bbcbee", "#91a5d4", "#93a7d5", "#a2b7e3", "#748cc4", "#274389", "#1c3c8a", "#314f9a", "#85a6e6", "#536db0", "#0d2268", "#051555"];
const BODY_BELOW = ["#486197", "#566c9e", "#7b93ce", "#4d699f"];
const BODY_ABOVE = ["#5f7ab8", "#8fa9e6", "#879fd9", "#7d97cd", "#7d96cc", "#7b93ca", "#7891c6", "#768fc3", "#768ec1", "#758cbf", "#758cbe", "#788dbf", "#8b9fcc", "#b5c5e7",
  ...Array<string>(30).fill("face")];
// Outside the floor along the foot: its seam, a lip, and the crest the
// floor's edge rolls over.
const FLOOR_LIP = ["#2a478e", "#bac6e4", "#d5e0f7", "#b0c4e9", "#c1d0f1", "#eff6fd", "#f8fbfe", "#d8e2f8"];
const WING_BELOW = ["#3e5591", "#698ddc", "#87abf6", "#94b4f6", "#c8e0fd", "#a6c0f7", "#4d6eca", "#395cbd", "#375ab9", "#3355b3", "#3153b0", "#2e50ac", "#2c4ea8", "#2b4ba6", "#2a4aa4", "#2847a0", "#27459f", "#244094", "#1c3881", "#193473", "#193170", "#20387e", "#234096", "#1a3475", "#1a3475", "#1a3475"];
const WING_ABOVE = ["#4b5d89", "#6f81a7", "#6d7ea7", "#6b7ba5", "#6e7fa8", "#6f80a8", "#7b8cb3", "#a3b4d7", "#b9c8e8", "#b7c6e5", "#bac8e6", "#becbe8",
  "#c2cfeb", "#c7d3ee", "#cad6ef", "#cedaf2", "#d1dcf4", "#d5dff5", "#d7e0f6", "#d9e2f7", "#dbe4f8", "#dce5f8", "#dce5f7", "#dde7f8", "#dee6f9", "#dfe7f9",
  "#dfe6f8", "#dde6f8", "#dce5f9", "#d2ddf5", "#bbc8e6"];
// The top: a blue lip at the outline (no pale bead: over a dark desktop it
// read as a stray line), then a silver roll measured from the
// floor's top edge: its seam, the shaded underside, a white crest, and the
// lit face rising to the lip.
const TOP_LIP = ["#2a56bd", "#3463cc", "#4473d4", "#5582dc", "#6586da", "#4a6cc4", "#294799", "#4f669d", "#9baacd"];
// On the platform the silver is one long shade, measured from the floor's
// top edge beside the window keys: a dim crest over the seam, darkening up
// to the middle, lightening again to the outline. Its one bright crest is
// the streak up the S.
const PLATFORM_LIP = ["#4f68a3", "#6a7eb0"];
const PLATFORM_ROLL = ["#4a597a", "#94a1c4", "#b1bfe3", "#8699c3", "#8599c7", "#899ecc", "#9eb2da", "#cddcf7", "#cad7f4", "#a8bade", "#a2b5dd", "#9fb2da",
  "#9cafd7", "#9aadd4", "#97aad2", "#96a8d0", "#92a5cc", "#90a2ca", "#8d9fc7", "#8b9cc4", "#899ac2", "#8798c0", "#8495be", "#8395bc", "#8192b9", "#8192b9",
  "#8192b9", "#8192b9", "#8192b9", "#8294b9", "#8596bb", "#8a9abe", "#8e9dc1", "#92a1c5", "#96a5c7", "#9aaacb", "#9faece", "#a4b2d1", "#a8b6d4", "#adbad8",
  "#b1bedc", "#b5c3e0", "#b8c5e1", "#b9c6e2", "#b9c6e2", "#b8c5e1", "#b5c2df", "#b0bedc", "#aab8d8", "#a3b2d3", "#9babce", "#93a4c9", "#8a9cc3", "#8193bd",
  "#7789b6", "#6c7fae"];
const TOP_ROLL = ["#4a5673", "#9fabcd", "#8c9cc1", "#7e90b5", "#8494bb", "#8c9dc2", "#a1b1d2", "#b3c1df", "#bcc9e5", "#e1e8f9", "#f9fbfe", "#e8eefc",
  "#e0e8fa", "#dde6f9", "#dbe4f7", "#d9e3f7", "#d7e1f6", "#d4dff5", "#d1dcf3", "#cedaf2", "#cad6f0", "#c4d0ec", "#bbc9e6", "#afbddd", "#a5b4d6", "#a1b1d3",
  "#9babcf", "#96a7cb", "#91a2c7", "#8c9ec3", "#889ac2", "#8598c1", "#8195bf", "#8195bf", "#8195bf", "#8195bf"];
// Round the top right corner: one shaded roll, no crest.
const CORNER_ROLL = ["#4f68a3", "#6a7eb0", "#7689b6", "#7f91bb", "#8496bf", "#8899c2", "#8b9cc4", "#8d9ec5", "#8e9fc6", "#8e9fc6", "#8d9ec5", "#8b9cc4",
  "#8899c2", "#8697c0", "#8697c0", "#8b9cc4", "#94a6cc", "#9fb2da", "#a4b5dc", "#a9b8dc"];

/** The shell as an SVG document for a width x height box, its pixel size
 * multiplied by scale (the display's pixel ratio) so it rasterises sharp. */
export function drawAeroShell(width: number, height: number, scale = 1): string {
  const w = Math.max(width, 700), h = Math.max(height, 400);
  const X = (x: number) => (x > SHELL.w - ANCHOR.right ? w - (SHELL.w - x) : x);
  const Y = (y: number) => (y > SHELL.h - ANCHOR.bottom ? h - (SHELL.h - y) : y);
  const at = ([x, y]: Point): Point => [X(x), Y(y)];
  const line = (points: Point[]) => points.map(([x, y], i) => `${i ? "L" : "M"}${n(x)} ${n(y)}`).join(" ");
  const shape = (points: Point[]) => `${line(points)} Z`;

  const outline = [
    ...corner(0, 23.1, 47.7, 47, [0, 1], [1, 0]).slice(0, -1),
    ...spline(TOP_RUN),
    ...spline(PLATFORM_RUN),
    ...corner(SHELL.w, 0.8, 39, 36, [-1, 0], [0, 1]),
    ...corner(SHELL.w, SHELL.h, 50, 46, [0, -1], [-1, 0], 2.2),
    ...spline(FOOT_RUN),
    ...corner(0, 700.2, 73, 60, [1, 0], [0, -1]).slice(1)
  ].map(at);

  // The groove: deep under the wing, easing to a sliver through the foot's
  // S, and the left bead's width once the rim turns up the left side.
  const wingX = (x: number) => GROOVE.body + (GROOVE.wing - GROOVE.body) * smooth((281 - x) / 131);
  const groove = offsetInward(resample(outline, 4), ([x, y]) => {
    const footward = smooth((y - (h - 90)) / 70);
    return GROOVE.left + (wingX(x) - GROOVE.left) * footward;
  });

  const capFootLine = spline(CAP_FOOT, 10).map(at);
  const cap = [...capFootLine, [X(330), -30], [-30, -30], [-30, Y(132)]] as Point[];

  const pocket = [
    ...corner(POCKET.left, POCKET.top, 22, 22, [0, 1], [1, 0]),
    ...corner(POCKET.right, POCKET.top, 18, 18, [-1, 0], [0, 1]),
    ...corner(POCKET.right, POCKET.footRight, 14, 14, [0, -1], [-1, 0]),
    ...spline([[POCKET.right - 14, POCKET.footRight], [150, 575.5], [90, 568], [POCKET.left + 14, POCKET.footLeft]]).slice(1, -1),
    ...corner(POCKET.left, POCKET.footLeft, 14, 14, [1, 0], [0, -1])
  ].map(at);
  const well = [
    ...corner(WELL.left, WELL.topLeft, 13, 13, [0, 1], [1, 0]),
    ...spline([[WELL.left + 13, WELL.topLeft], [42.3, 570.2], [85, 572], [130, 576], [175, 582], [WELL.right - 13, WELL.topRight]]).slice(1, -1),
    ...corner(WELL.right, WELL.topRight, 13, 13, [-1, 0], [0, 1]),
    ...corner(WELL.right, WELL.bottom, 16, 16, [0, -1], [-1, 0]),
    ...corner(WELL.left, WELL.bottom, 16, 16, [1, 0], [0, -1])
  ].map(at);
  const floor = [
    ...corner(FLOOR.left, FLOOR.top, 19, 19, [0, 1], [1, 0], 2),
    ...corner(FLOOR.right, FLOOR.top, 19, 19, [-1, 0], [0, 1], 2),
    ...corner(FLOOR.right, FLOOR.bottom, 50, 50, [0, -1], [-1, 0], 2.2),
    ...corner(FLOOR.left, FLOOR.bottom, 19, 19, [1, 0], [0, -1])
  ].map(at);

  // Paint.
  const stops = (list: Array<[number, string, number?]>) => list.map(([o, colour, opacity = 1]) => `<stop offset="${n(o)}" stop-color="${colour}" stop-opacity="${opacity}"/>`).join("");
  const down = (id: string, y0: number, y1: number, list: Array<[number, string, number?]>) =>
    `<linearGradient id="${id}" gradientUnits="userSpaceOnUse" x1="0" y1="${n(Y(y0))}" x2="0" y2="${n(Y(y1))}">${stops(list)}</linearGradient>`;
  const across = (id: string, x0: number, x1: number, list: Array<[number, string, number?]>) =>
    `<linearGradient id="${id}" gradientUnits="userSpaceOnUse" x1="${n(X(x0))}" y1="0" x2="${n(X(x1))}" y2="0">${stops(list)}</linearGradient>`;
  // Concentric strokes of a shape, widest first; each band is [half-width, colour].
  const moulding = (id: string, bands: Array<[number, string, number?]>) =>
    bands.map(([half, colour, opacity = 1]) => `<use href="#${id}" fill="none" stroke="${colour}" stroke-opacity="${opacity}" stroke-width="${n(half * 2)}"/>`).join("");
  // A measured profile as concentric strokes: entry k covers reference
  // pixel k from the edge.
  const profile = (id: string, colours: string[]) =>
    moulding(id, colours.map((colour, k) => [(k + 1) / REF, colour === "face" ? "url(#aero-face)" : colour] as [number, string]).reverse());
  const region = (id: string, areas: Point[][], blur: number) =>
    `<filter id="${id}-soft" x="-20%" y="-20%" width="140%" height="140%"><feGaussianBlur stdDeviation="${blur}"/></filter>` +
    `<mask id="${id}" maskUnits="userSpaceOnUse" x="-60" y="-60" width="${n(w + 120)}" height="${n(h + 120)}"><path d="${areas.map(shape).join(" ")}" fill="#fff" filter="url(#${id}-soft)"/></mask>`;

  const split = X(230);
  const cut = 70; // how far into each corner a side's region reaches along the diagonal

  return `<svg xmlns="http://www.w3.org/2000/svg" width="${n(w * scale)}" height="${n(h * scale)}" viewBox="0 0 ${n(w)} ${n(h)}" shape-rendering="geometricPrecision" aria-hidden="true" focusable="false">
<defs>
  <path id="aero-shell" d="${shape(outline)}"/>
  <path id="aero-groove" d="${shape(groove)}"/>
  <path id="aero-cap" d="${shape(cap)}"/>
  <path id="aero-streak" d="${line(spline(STREAK).map(at))}"/>
  <filter id="aero-streak-soft" x="-10%" y="-50%" width="120%" height="200%"><feGaussianBlur stdDeviation="2.2"/></filter>
  ${across("aero-streak-fade", 800, 990, [[0, "#ffffff", 0], [0.15, "#ffffff", 1], [0.85, "#ffffff", 1], [1, "#ffffff", 0]])}
  ${across("aero-streak-shade", 800, 990, [[0, "#7f90b6", 0], [0.15, "#7f90b6", 0.55], [0.85, "#7f90b6", 0.55], [1, "#7f90b6", 0]])}
  <path id="aero-cap-foot" d="${line(capFootLine)}"/>
  <path id="aero-pocket" d="${shape(pocket)}"/>
  <path id="aero-well" d="${shape(well)}"/>
  <path id="aero-floor" d="${shape(floor)}"/>
  <!-- The silhouette, cut once over the finished art so its edge is smoothed once. -->
  <mask id="aero-shell-mask" maskUnits="userSpaceOnUse" x="-60" y="-60" width="${n(w + 120)}" height="${n(h + 120)}"><use href="#aero-shell" fill="#fff"/></mask>
  <clipPath id="aero-in-groove"><use href="#aero-groove"/></clipPath>
  <clipPath id="aero-in-cap"><use href="#aero-cap"/></clipPath>
  <clipPath id="aero-in-pocket"><use href="#aero-pocket"/></clipPath>
  <clipPath id="aero-in-well"><use href="#aero-well"/></clipPath>
  <clipPath id="aero-in-floor"><use href="#aero-floor"/></clipPath>
  <mask id="aero-out-groove" maskUnits="userSpaceOnUse" x="-60" y="-60" width="${n(w + 120)}" height="${n(h + 120)}">
    <rect x="-60" y="-60" width="${n(w + 120)}" height="${n(h + 120)}" fill="#fff"/><use href="#aero-groove" fill="#000"/>
  </mask>
  ${region("aero-top", [[[X(218), -20], [w + 20, -20], [w + 20, 105], [X(218), 105]], [[X(219), 40], [X(248), 40], [X(248), h - 80], [X(219), h - 80]]], 4)}
  ${region("aero-left", [[[-20, Y(140)], [40, Y(140)], [40, h - 110], [-20, h - 110]]], 6)}
  ${region("aero-right", [[[w - cut, 45], [w + 20, 45], [w + 20, h - 95], [w - cut, h - 95]]], 12)}
  ${region("aero-corner", [[[w - 95, -20], [w + 20, -20], [w + 20, 150], [w - 95, 150]]], 14)}
  ${region("aero-body", [[[split, h - cut], [w + 20, h - cut], [w + 20, h + 20], [split, h + 20]]], 8)}
  ${region("aero-wing", [[[-20, h - 115], [split, h - 115], [split, h + 20], [-20, h + 20]]], 8)}
  ${across("aero-body-lip-fade", 860, 940, [[0, "#ffffff"], [1, "#ffffff", 0]])}
  ${across("aero-platform-lip-fade", 860, 940, [[0, "#ffffff", 0], [1, "#ffffff"]])}
  <mask id="aero-body-lip" maskUnits="userSpaceOnUse" x="-60" y="-60" width="${n(w + 120)}" height="${n(h + 120)}"><rect x="-60" y="-60" width="${n(w + 120)}" height="${n(h + 120)}" fill="url(#aero-body-lip-fade)"/></mask>
  <mask id="aero-platform-lip" maskUnits="userSpaceOnUse" x="-60" y="-60" width="${n(w + 120)}" height="${n(h + 120)}"><rect x="-60" y="-60" width="${n(w + 120)}" height="${n(h + 120)}" fill="url(#aero-platform-lip-fade)"/></mask>
  ${down("aero-silver", 0, SHELL.h, [[0, "#dfe7f8"], [0.08, "#c9d5f0"], [0.5, "#c5d2ee"], [0.92, "#c3cfec"], [1, "#b5c4e6"]])}
  ${down("aero-cap-fill", 21, 92, [[0, "#4a70d0"], [0.06, "#5b82dc"], [0.26, "#eef8ff"], [0.34, "#b6d1fd"], [0.48, "#7aa0ee"], [0.62, "#4a72d1"], [0.8, "#2e55bb"], [1, "#2143a1"]])}
  ${down("aero-gel-across", POCKET.top, POCKET.footLeft, [[0, "#5a86ef"], [0.72, "#5481ec"], [0.9, "#729cf5"], [1, "#90b4fb"]])}
  ${across("aero-gel-edge", POCKET.left, POCKET.right, [[0, "#ffffff", 0.4], [0.12, "#ffffff", 0.08], [0.55, "#1a3fb0", 0], [0.9, "#1a3fb0", 0.16], [0.97, "#ffffff", 0], [1, "#ffffff", 0.35]])}
  <clipPath id="aero-above-sheen"><path d="${shape([...spline(SHEEN).map(at), [X(POCKET.right + 10), Y(POCKET.top - 10)], [X(POCKET.left - 10), Y(POCKET.top - 10)]])}"/></clipPath>
  <path id="aero-sheen" d="${line(spline(SHEEN).map(at))}"/>
  <filter id="aero-sheen-soft" x="-20%" y="-60%" width="140%" height="220%"><feGaussianBlur stdDeviation="22"/></filter>
  ${down("aero-gel-down", POCKET.top, SHELL.h - 120, [[0, "#ffffff", 0.15], [0.05, "#ffffff", 0], [0.8, "#ffffff", 0], [1, "#ffffff", 0.12]])}
  ${down("aero-well-fill", WELL.topLeft, WELL.bottom, [[0, "#162a6c"], [0.7, "#1a2f74"], [1, "#203a85"]])}
  ${down("aero-floor-fill", FLOOR.top, FLOOR.bottom, [[0, "#1d3480"], [0.1, "#1a2f74"], [0.9, "#1f3888"], [1, "#2e4ca3"]])}
  ${across("aero-face", 281, 1050, [[0, "#b5c4e8"], [0.13, "#a4b7e0"], [0.38, "#dee8fc"], [0.58, "#d3dff9"], [0.83, "#9bb0db"], [1, "#869dcf"]])}
  ${across("aero-lip-fade", 0, 300, [[0, "#ffffff", 0], [0.035, "#ffffff", 0], [0.07, "#ffffff", 1], [0.8, "#ffffff", 1], [1, "#ffffff", 0]])}
  <mask id="aero-lip" maskUnits="userSpaceOnUse" x="-60" y="-60" width="${n(w + 120)}" height="${n(h + 120)}"><rect x="-60" y="-60" width="${n(w + 120)}" height="${n(h + 120)}" fill="url(#aero-lip-fade)"/></mask>
</defs>

<g mask="url(#aero-shell-mask)">
<rect x="-60" y="-60" width="${n(w + 120)}" height="${n(h + 120)}" fill="url(#aero-silver)"/>
<g>
  ${moulding("aero-shell", [[29, "#f6f8fc"], [28, "#4a5673"], [27, "#8494bb"], [25, "#a1b1d2"], [23, "#bcc9e5"], [20, "#dde6f9"], [17, "#eef3fd"], [14, "#d4dff5"], [11, "#bccae7"], [8.5, "#93a4c9"], [6.5, "#294799"], [5.5, "#6586da"], [4.5, "#b4c9f2"], [2, "#2f5fc2"]])}
</g>

${moulding("aero-pocket", [[9, "#a4b6df"], [7, "#92a5d4"], [5, "#7e92c2"], [3.5, "#6e84b6"], [2, "#4d64a4"]])}
${moulding("aero-well", [[10, "#c8d4f0"], [8.5, "#d0dbf3"], [7.5, "#9eaed3"], [6, "#708ecc"], [3.6, "#6c8ccb"], [2.6, "#bacef2"], [1.4, "#253d7a"]])}
${moulding("aero-floor", [[1.5, "#061e6e"]])}

<g>
  <g mask="url(#aero-top)">
    <g mask="url(#aero-body-lip)">${profile("aero-floor", TOP_ROLL)}${profile("aero-shell", TOP_LIP)}</g>
    <g mask="url(#aero-platform-lip)">${profile("aero-floor", PLATFORM_ROLL)}${profile("aero-shell", PLATFORM_LIP)}</g>
    <use href="#aero-streak" fill="none" stroke="url(#aero-streak-shade)" stroke-width="7" transform="translate(1.5 5)" filter="url(#aero-streak-soft)"/>
    <use href="#aero-streak" fill="none" stroke="url(#aero-streak-fade)" stroke-width="6" stroke-opacity="0.55" transform="translate(-1 -3)" filter="url(#aero-streak-soft)"/>
    <use href="#aero-streak" fill="none" stroke="url(#aero-streak-fade)" stroke-width="2"/>
  </g>
  <g mask="url(#aero-left)">${profile("aero-shell", LEFT_BEAD)}</g>
  <g mask="url(#aero-right)">${profile("aero-shell", RIGHT_BEAD)}</g>
  <g mask="url(#aero-corner)">${profile("aero-shell", CORNER_ROLL)}</g>
  <g mask="url(#aero-wing)">
    <g mask="url(#aero-out-groove)">${profile("aero-groove", WING_BELOW)}</g>
    <g clip-path="url(#aero-in-groove)">${profile("aero-groove", WING_ABOVE)}</g>
  </g>
  <g mask="url(#aero-body)">
    <g mask="url(#aero-out-groove)">${profile("aero-groove", BODY_BELOW)}</g>
    <g clip-path="url(#aero-in-groove)">${profile("aero-groove", BODY_ABOVE)}</g>
    ${profile("aero-floor", FLOOR_LIP)}
  </g>
</g>

<g>
  <g clip-path="url(#aero-in-cap)" mask="url(#aero-lip)">
    <use href="#aero-shell" fill="url(#aero-cap-fill)"/>
    ${moulding("aero-shell", [[4.5, "#9cb9eb"], [2.5, "#c9dcfb"], [1.2, "#256adb"]])}
  </g>
  <g mask="url(#aero-lip)">
    <use href="#aero-cap-foot" fill="none" stroke="#f9fbfe" stroke-width="7" transform="translate(0 4)"/>
    <use href="#aero-cap-foot" fill="none" stroke="#c6cee3" stroke-width="3" transform="translate(0 6.5)"/>
    <use href="#aero-cap-foot" fill="none" stroke="#1b3686" stroke-width="3"/>
    <use href="#aero-cap-foot" fill="none" stroke="#546a98" stroke-width="1" transform="translate(0 2)"/>
  </g>
</g>
<use href="#aero-pocket" fill="url(#aero-gel-across)"/>
<use href="#aero-pocket" fill="url(#aero-gel-edge)"/>
<g clip-path="url(#aero-in-pocket)"><g clip-path="url(#aero-above-sheen)">
  <use href="#aero-sheen" fill="none" stroke="#bcd6ff" stroke-opacity="0.55" stroke-width="70" filter="url(#aero-sheen-soft)"/>
  <use href="#aero-sheen" fill="none" stroke="#cfe2ff" stroke-opacity="0.5" stroke-width="5" filter="url(#aero-streak-soft)"/>
</g></g>
<use href="#aero-pocket" fill="url(#aero-gel-down)"/>
<g clip-path="url(#aero-in-pocket)">${moulding("aero-pocket", [[2.4, "#e6f3fe"], [1, "#7b9be7"]])}</g>

<use href="#aero-well" fill="url(#aero-well-fill)"/>
<g clip-path="url(#aero-in-well)">
  <use href="#aero-well" fill="none" stroke="#000000" stroke-opacity="0.45" stroke-width="7" transform="translate(0 2)"/>
  ${moulding("aero-well", [[1.4, "#8fa8e8", 0.6]])}
</g>

<use href="#aero-floor" fill="url(#aero-floor-fill)"/>
<g clip-path="url(#aero-in-floor)">
  <use href="#aero-floor" fill="none" stroke="#000000" stroke-opacity="0.25" stroke-width="7" transform="translate(0 2.5)"/>
  ${moulding("aero-floor", [[3.4 / REF, "#5477c6"], [2.4 / REF, "#86a4ec"], [1.4 / REF, "#5b7ccf"], [0.9, "#2a4d9e"]])}
</g>
</g>
</svg>`;
}
