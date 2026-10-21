// The hero's buffer strip (docs/site-design.md): one bar per second of the
// visitor's pointer, scroll and key activity, rolling left the way Clipture's
// replay buffer does. Ctrl+Shift+S, or Save, keeps the last 60 seconds as a
// clip of the visit. One animation-frame clock, stopped off screen, in hidden
// tabs and under reduced motion.
(function () {
  "use strict";
  var figure = document.querySelector("[data-buffer]");
  if (!figure) return;
  var canvas = figure.querySelector("canvas");
  var context = canvas.getContext("2d");
  var fill = document.querySelector("[data-buffer-fill]");
  var shelf = document.querySelector("[data-shelf]");
  var status = document.querySelector("[data-shelf-status]");
  var saveButton = document.querySelector("[data-save]");
  var reduce = window.matchMedia("(prefers-reduced-motion: reduce)");

  var SECONDS = 60;
  var history = [];          // completed seconds, oldest first: { level, moment }
  var raw = 0;               // activity gathered in the second under way
  var startedAt = performance.now();
  var secondAt = startedAt;
  var moment = "the top of the page";
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

  // Where the visitor is: the section most in view.
  var ratios = new Map();
  var sections = new IntersectionObserver(function (entries) {
    entries.forEach(function (entry) { ratios.set(entry.target, entry.intersectionRatio); });
    var best = 0;
    ratios.forEach(function (ratio, section) {
      if (ratio > best) { best = ratio; moment = section.getAttribute("data-moment"); }
    });
  }, { threshold: [0, 0.25, 0.5, 0.75, 1] });
  document.querySelectorAll("[data-moment]").forEach(function (section) { sections.observe(section); });

  function advance(now) {
    while (now - secondAt >= 1000) {
      history.push({ level: level(raw), moment: moment });
      if (history.length > SECONDS) history.shift();
      raw = 0;
      secondAt += 1000;
    }
    if (fill) {
      var text = Math.min(SECONDS, Math.floor((now - startedAt) / 1000)) + " s";
      if (fill.textContent !== text) fill.textContent = text;
    }
  }

  function bars(target, width, height, seconds, offset, live) {
    var slot = width / SECONDS;
    var middle = height / 2;
    var reach = height * 0.42;
    target.clearRect(0, 0, width, height);
    target.fillStyle = "rgba(141, 140, 131, 0.5)";
    target.fillRect(0, Math.round(middle), width, 1);
    var draw = function (value, x, alpha) {
      var h = Math.max(1, value * reach);
      target.fillStyle = "rgba(228, 228, 221, " + alpha + ")";
      target.fillRect(x + slot * 0.18, middle - h, slot * 0.64, h * 2);
    };
    for (var i = 0; i < seconds.length; i += 1) {
      var age = seconds.length - i;
      draw(seconds[i].level, width - (age + offset) * slot, age <= 3 ? 0.95 : 0.62);
    }
    if (live !== undefined) draw(live, width - offset * slot, 0.95);
  }

  function size(element, width, height) {
    var scale = window.devicePixelRatio || 1;
    var w = Math.max(1, Math.round(width * scale));
    var h = Math.max(1, Math.round(height * scale));
    if (element.width !== w) element.width = w;
    if (element.height !== h) element.height = h;
    var target = element.getContext("2d");
    target.setTransform(scale, 0, 0, scale, 0, 0);
    return target;
  }

  function render(now) {
    var width = canvas.clientWidth;
    var height = canvas.clientHeight;
    if (!width || !height) return;
    size(canvas, width, height);
    var offset = reduce.matches ? 0 : Math.min(1, (now - secondAt) / 1000);
    bars(context, width, height, history, offset, reduce.matches ? undefined : level(raw));
    // Now: the tally playhead at the right edge.
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
  // The buffer keeps filling while the strip isn't drawn; still screens draw
  // once a second.
  window.setInterval(function () {
    var now = performance.now();
    advance(now);
    if (reduce.matches) render(now);
  }, 1000);
  render(performance.now());
  schedule();

  // Saving
  var clock = function (seconds) {
    return Math.floor(seconds / 60) + ":" + String(Math.floor(seconds % 60)).padStart(2, "0");
  };

  function save() {
    var now = performance.now();
    advance(now);
    var seconds = history.concat([{ level: level(raw), moment: moment }]).slice(-SECONDS);
    var length = Math.min(SECONDS, (now - startedAt) / 1000);
    var counts = new Map();
    seconds.forEach(function (second) { counts.set(second.moment, (counts.get(second.moment) || 0) + 1); });
    var mostly = moment;
    var most = 0;
    counts.forEach(function (count, name) { if (count > most) { most = count; mostly = name; } });

    figure.classList.remove("is-saved");
    void figure.offsetWidth;
    figure.classList.add("is-saved");
    addClip(seconds, length, mostly);
  }

  function addClip(seconds, length, mostly) {
    var card = document.createElement("article");
    card.className = "clip";
    var thumb = document.createElement("canvas");
    thumb.setAttribute("aria-hidden", "true");
    var text = document.createElement("div");
    text.className = "clip-text";
    var title = document.createElement("strong");
    title.textContent = "Saved " + clock(length) + " of your visit";
    var where = document.createElement("span");
    where.textContent = "Mostly " + mostly;
    var replay = document.createElement("button");
    replay.type = "button";
    replay.className = "clip-replay";
    replay.textContent = "Go back there";
    var close = document.createElement("button");
    close.type = "button";
    close.className = "clip-close";
    close.setAttribute("aria-label", "Dismiss this clip");
    close.textContent = "×";
    text.append(title, where, replay);
    card.append(thumb, text, close);

    var dismiss = function () {
      card.remove();
      if (!shelf.querySelector(".clip")) shelf.hidden = true;
    };
    replay.addEventListener("click", function () {
      var target = document.querySelector('[data-moment="' + mostly + '"]');
      if (target) target.scrollIntoView({ behavior: reduce.matches ? "auto" : "smooth" });
    });
    close.addEventListener("click", dismiss);

    shelf.hidden = false;
    shelf.append(card);
    var clips = shelf.querySelectorAll(".clip");
    if (clips.length > 3) clips[0].remove();
    bars(size(thumb, 112, 63), 112, 63, seconds, 0);
    status.textContent = "Saved the last " + Math.round(length) + " seconds of your visit.";

    // Gone after a while, unless the visitor is using it.
    var timer = window.setTimeout(dismiss, 14000);
    var hold = function () { window.clearTimeout(timer); };
    card.addEventListener("pointerenter", hold);
    card.addEventListener("focusin", hold);
  }

  if (saveButton) saveButton.addEventListener("click", save);
  window.addEventListener("keydown", function (event) {
    if (!(event.ctrlKey || event.metaKey) || !event.shiftKey || event.code !== "KeyS" || event.repeat) return;
    event.preventDefault();
    if (saveButton) {
      saveButton.classList.add("is-pressed");
      window.setTimeout(function () { saveButton.classList.remove("is-pressed"); }, 220);
    }
    save();
  });
})();
