// The hero's buffer strip (docs/site-design.md): one bar per second of the
// visitor's pointer, scroll and key activity, rolling left the way Clipture's
// replay buffer does. Ctrl+Shift+S, or Save, keeps it: the strip holds still
// and the tally says what was saved. One animation-frame clock, stopped off
// screen, in hidden tabs and under reduced motion.
(function () {
  "use strict";
  var figure = document.querySelector("[data-buffer]");
  if (!figure) return;
  var canvas = figure.querySelector("canvas");
  var fill = document.querySelector("[data-buffer-fill]");
  var label = document.querySelector("[data-tally-text]");
  var status = document.querySelector("[data-save-status]");
  var saveButton = document.querySelector("[data-save]");
  var reduce = window.matchMedia("(prefers-reduced-motion: reduce)");

  var SECONDS = 60;
  var HOLD_MS = 2600;
  var history = [];          // completed seconds, oldest first: activity 0..1
  var raw = 0;               // activity gathered in the second under way
  var startedAt = performance.now();
  var secondAt = startedAt;
  var heldUntil = 0;         // after a save the strip shows the kept clip
  var kept = null;
  var visible = true;
  var frame = 0;

  // Activity: pointer travel, scrolling and keys, squashed into 0..1.
  var pointer = null;
  window.addEventListener("pointermove", function (event) {
    if (pointer) raw += Math.hypot(event.clientX - pointer.x, event.clientY - pointer.y) / Math.max(innerWidth, 1);
    pointer = { x: event.clientX, y: event.clientY };
  }, { passive: true });
  var lastScroll = window.scrollY;
  window.addEventListener("scroll", function () {
    raw += Math.abs(window.scrollY - lastScroll) / Math.max(innerHeight, 1);
    lastScroll = window.scrollY;
  }, { passive: true });
  window.addEventListener("keydown", function () { raw += 0.12; });
  window.addEventListener("pointerdown", function () { raw += 0.2; });
  var level = function (value) { return 1 - Math.exp(-value * 2.4); };
  var clock = function (seconds) {
    return Math.floor(seconds / 60) + ":" + String(Math.floor(seconds % 60)).padStart(2, "0");
  };

  function advance(now) {
    while (now - secondAt >= 1000) {
      history.push(level(raw));
      if (history.length > SECONDS) history.shift();
      raw = 0;
      secondAt += 1000;
    }
    if (now >= heldUntil && kept) {
      kept = null;
      label.textContent = "Recording this visit";
      figure.classList.remove("is-kept");
    }
    var text = clock(Math.min(SECONDS, (now - startedAt) / 1000));
    if (!kept && fill.textContent !== text) fill.textContent = text;
  }

  function render(now) {
    var width = canvas.clientWidth;
    var height = canvas.clientHeight;
    if (!width || !height) return;
    var scale = window.devicePixelRatio || 1;
    var w = Math.round(width * scale);
    var h = Math.round(height * scale);
    if (canvas.width !== w) canvas.width = w;
    if (canvas.height !== h) canvas.height = h;
    var context = canvas.getContext("2d");
    context.setTransform(scale, 0, 0, scale, 0, 0);
    context.clearRect(0, 0, width, height);

    var slot = width / SECONDS;
    var middle = height / 2;
    var reach = height * 0.42;
    var still = kept || reduce.matches;
    var seconds = kept || history;
    var offset = still ? 0 : Math.min(1, (now - secondAt) / 1000);
    context.fillStyle = "rgba(141, 140, 131, 0.5)";
    context.fillRect(0, Math.round(middle), width, 1);
    var bar = function (value, x, ink) {
      var half = Math.max(1, value * reach);
      context.fillStyle = ink;
      context.fillRect(x + slot * 0.18, middle - half, slot * 0.64, half * 2);
    };
    var ink = kept ? "#f07836" : "rgba(228, 228, 221, 0.7)";
    for (var i = 0; i < seconds.length; i += 1) {
      bar(seconds[i], width - (seconds.length - i + offset) * slot, ink);
    }
    if (!still) bar(level(raw), width - offset * slot, ink);
    context.fillStyle = "#f07836";
    context.fillRect(width - 2, 0, 2, height);
  }

  function tick(now) {
    frame = 0;
    advance(now);
    render(now);
    schedule();
  }
  function schedule() {
    if (!frame && visible && !document.hidden && !reduce.matches) frame = requestAnimationFrame(tick);
  }
  new IntersectionObserver(function (entries) {
    visible = entries[0].isIntersecting;
    schedule();
  }).observe(figure);
  document.addEventListener("visibilitychange", schedule);
  // The buffer keeps filling while the strip isn't drawn; a still strip
  // (reduced motion) redraws once a second.
  window.setInterval(function () {
    var now = performance.now();
    advance(now);
    if (reduce.matches) render(now);
  }, 1000);
  render(performance.now());
  schedule();

  function save() {
    var now = performance.now();
    advance(now);
    var length = Math.min(SECONDS, (now - startedAt) / 1000);
    kept = history.concat([level(raw)]).slice(-SECONDS);
    heldUntil = now + HOLD_MS;
    label.textContent = "Saved";
    fill.textContent = clock(length);
    status.textContent = "Saved the last " + Math.round(length) + " seconds of your visit.";
    figure.classList.remove("is-kept");
    void figure.offsetWidth;
    figure.classList.add("is-kept");
    render(now);
  }

  saveButton.addEventListener("click", save);
  window.addEventListener("keydown", function (event) {
    if (!(event.ctrlKey || event.metaKey) || !event.shiftKey || event.code !== "KeyS" || event.repeat) return;
    event.preventDefault();
    saveButton.classList.add("is-pressed");
    window.setTimeout(function () { saveButton.classList.remove("is-pressed"); }, 220);
    save();
  });
})();
