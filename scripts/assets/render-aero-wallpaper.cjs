"use strict";

// Renders the Aero theme's wallpaper, assets/aero/meadow.webp: a deep
// blue sky paling to the horizon, soft cloud along both sides, and a
// meadow on one broad hill, high on the left and falling away to the
// right.
//
// The meadow is a 3D render: aero-meadow.blender.py builds the hill, covers
// it in instanced grass blades and renders it with Cycles against a
// transparent sky. This script runs that render (or takes an existing
// one), then lays it over a sky and cloud drawn as SVG (seeded, so they
// render the same every time), rasterizes the whole picture in headless
// Microsoft Edge and captures it as a WebP. The app shows one ready
// bitmap and draws nothing at runtime.
//
//   node scripts/assets/render-aero-wallpaper.cjs [--ground meadow.png] [--samples 160]
//
// Needs Blender 4.x (BLENDER, or the default install folder) unless
// --ground is given.

const { spawnSync, spawn } = require("node:child_process");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { pathToFileURL } = require("node:url");

const WIDTH = 2560;
const HEIGHT = 1440;
const root = path.resolve(__dirname, "..", "..");
const output = path.join(root, "assets", "aero", "meadow.webp");
const option = (name) => { const at = process.argv.indexOf(name); return at > 0 ? process.argv[at + 1] : undefined; };
const edge = [
  "C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe",
  "C:/Program Files/Microsoft/Edge/Application/msedge.exe"
].find((candidate) => fs.existsSync(candidate));
if (!edge) throw new Error("Microsoft Edge was not found");

// The meadow: rendered in Blender unless one is supplied.
function renderGround(work) {
  const given = option("--ground");
  if (given) return path.resolve(given);
  const blender = [process.env.BLENDER, "C:/Program Files/Blender Foundation/Blender 4.3/blender.exe"]
    .find((candidate) => candidate && fs.existsSync(candidate));
  if (!blender) throw new Error("Blender was not found; set BLENDER or pass --ground");
  const file = path.join(work, "meadow-ground.png");
  console.log("Rendering the meadow in Blender (several minutes)...");
  const run = spawnSync(blender, [
    "--background", "--factory-startup", "--python", path.join(__dirname, "aero-meadow.blender.py"),
    "--", file, String(WIDTH), String(HEIGHT), option("--samples") ?? "160"
  ], { stdio: "inherit" });
  if (run.status !== 0 || !fs.existsSync(file)) throw new Error("The Blender render failed");
  return file;
}

// mulberry32: a small seeded generator, so the cloud is stable.
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

// The sky is drawn in a 1920x1080 frame, the meadow laid over all of it.
const HORIZON = 600;

// A cloud: a flat base with puffs heaped on it, each lit from above and
// shaded beneath. The #cloud filter frays the heap into vapour.
function cloud(cx, cy, size, opacity) {
  let out = `<ellipse cx="${cx}" cy="${n(cy + size * 0.1)}" rx="${n(size * 1.5)}" ry="${n(size * 0.2)}" fill="#cfe6fb"/>`;
  const puffs = Math.round(between(6, 9));
  for (let k = 0; k < puffs; k += 1) {
    const along = (k / (puffs - 1)) * 2 - 1;
    const heap = 1 - along * along;
    const r = size * (0.2 + 0.34 * heap) * between(0.8, 1.15);
    out += `<circle cx="${n(cx + along * size * 1.15)}" cy="${n(cy - r * 0.55 - heap * size * between(0.05, 0.22))}" r="${n(r)}" fill="url(#puff)"/>`;
  }
  return `<g filter="url(#cloud)" opacity="${opacity}">${out}</g>`;
}

const scene = (ground) => `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1920 1080" preserveAspectRatio="xMidYMid slice">
  <defs>
    <linearGradient id="sky" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#0a4ce3"/>
      <stop offset="0.2" stop-color="#0c50e8"/>
      <stop offset="0.33" stop-color="#2a74f6"/>
      <stop offset="0.45" stop-color="#5096fa"/>
      <stop offset="0.52" stop-color="#8fbcfb"/>
      <stop offset="0.56" stop-color="#c4dbfb"/>
      <stop offset="1" stop-color="#c4dbfb"/>
    </linearGradient>
    <linearGradient id="puff" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#ffffff"/>
      <stop offset="0.5" stop-color="#ffffff"/>
      <stop offset="1" stop-color="#c3e0fa"/>
    </linearGradient>
    <linearGradient id="haze" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#ffffff" stop-opacity="0"/>
      <stop offset="0.5" stop-color="#f4fcff" stop-opacity="0.85"/>
      <stop offset="1" stop-color="#ffffff" stop-opacity="0"/>
    </linearGradient>
    <!-- Vapour: fray a heap of discs with slow noise, then soften. -->
    <filter id="cloud" x="-30%" y="-60%" width="160%" height="220%">
      <feTurbulence type="fractalNoise" baseFrequency="0.013 0.02" numOctaves="5" seed="7" result="noise"/>
      <feDisplacementMap in="SourceGraphic" in2="noise" scale="58" xChannelSelector="R" yChannelSelector="G"/>
      <feGaussianBlur stdDeviation="6"/>
    </filter>
  </defs>

  <rect width="1920" height="1080" fill="url(#sky)"/>

  <!-- Cloud along both sides, high and thin to low and heaped. -->
  ${cloud(60, 0, 70, 0.8)}
  ${cloud(150, 480, 105, 0.75)}
  ${cloud(1820, 225, 85, 0.8)}
  ${cloud(1830, 470, 115, 0.7)}
  ${cloud(860, 20, 60, 0.35)}

  <!-- Haze on the horizon. -->
  <rect x="0" y="${HORIZON - 90}" width="1920" height="170" fill="url(#haze)" opacity="0.3"/>

  <!-- The meadow, rendered with its sky transparent. -->
  <image href="${ground}" x="0" y="0" width="1920" height="1080" preserveAspectRatio="xMidYMid slice"/>
</svg>`;

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function main() {
  const work = fs.mkdtempSync(path.join(os.tmpdir(), "aero-render-"));
  const port = 9300 + Math.floor(Math.random() * 400);
  let browser;
  try {
    const ground = renderGround(work);
    const page = `<!doctype html>
<style>html, body { margin: 0; height: 100%; overflow: hidden; background: #0b62d6; } svg { display: block; width: 100%; height: 100%; }</style>
${scene(pathToFileURL(ground).href)}`;
    const file = path.join(work, "scene.html");
    fs.writeFileSync(file, page);
    browser = spawn(edge, [
      "--headless=new", `--remote-debugging-port=${port}`, `--user-data-dir=${path.join(work, "profile")}`,
      `--window-size=${WIDTH},${HEIGHT}`, "--force-device-scale-factor=1", "--allow-file-access-from-files", "about:blank"
    ], { stdio: "ignore" });
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
    await send("Emulation.setDeviceMetricsOverride", { width: WIDTH, height: HEIGHT, deviceScaleFactor: 1, mobile: false });
    await send("Page.navigate", { url: pathToFileURL(file).href });
    await sleep(800);
    await send("Runtime.evaluate", {
      expression: "Promise.all([...document.images].map((i) => i.decode?.())).then(() => new Promise((done) => requestAnimationFrame(() => requestAnimationFrame(done))))",
      awaitPromise: true
    });
    await sleep(2500);
    const shot = await send("Page.captureScreenshot", { format: "webp", quality: 90 });
    if (!shot.result?.data) throw new Error("Capture failed");
    fs.writeFileSync(output, Buffer.from(shot.result.data, "base64"));
    socket.close();
    console.log(`Wrote ${path.relative(root, output)} (${WIDTH}x${HEIGHT}, ${Math.round(fs.statSync(output).size / 1024)} KiB)`);
  } finally {
    if (browser) {
      const exited = new Promise((resolve) => browser.once("exit", resolve));
      browser.kill();
      await exited;
    }
    fs.rmSync(work, { recursive: true, force: true, maxRetries: 5, retryDelay: 200 });
  }
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
