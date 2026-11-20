// Shows the embedded app at its real 1280x800 layout, scaled to the width
// available, so it looks exactly like the desktop window at any size.
(function () {
  "use strict";
  var frame = document.querySelector("[data-app-frame] .viewport");
  if (!frame || !("ResizeObserver" in window)) return;
  var resize = function () {
    frame.style.setProperty("--app-scale", String(frame.clientWidth / 1280));
  };
  new ResizeObserver(resize).observe(frame);
  resize();
})();

// The caption bar takes the demo's theme, as the desktop window's does:
// the app's background behind it, its border and its muted text.
(function () {
  "use strict";
  var win = document.querySelector("[data-app-frame]");
  var iframe = win && win.querySelector("iframe");
  if (!iframe || !("MutationObserver" in window)) return;
  var watched;
  var copy = function (root) {
    var style = root.ownerDocument.defaultView.getComputedStyle(root);
    [["--app-bg", "--sidebar-bg"], ["--border", "--border"], ["--text-muted", "--text-muted"], ["--text-subtle", "--text-subtle"]]
      .forEach(function (pair) {
        var value = style.getPropertyValue(pair[0]).trim();
        if (value) win.style.setProperty(pair[1], value);
        else win.style.removeProperty(pair[1]);
      });
  };
  var attach = function () {
    var root;
    try { root = iframe.contentDocument && iframe.contentDocument.documentElement; } catch (error) { return; }
    if (!root) return;
    if (watched) watched.disconnect();
    // Read on the next frame, when the browser restyles anyway, rather than
    // forcing an extra style pass in the middle of the theme switch.
    var queued = false;
    watched = new MutationObserver(function () {
      if (queued) return;
      queued = true;
      requestAnimationFrame(function () { queued = false; copy(root); });
    });
    watched.observe(root, { attributes: true, attributeFilter: ["data-theme", "style", "class"] });
    copy(root);
  };
  iframe.addEventListener("load", attach);
  attach();
})();
