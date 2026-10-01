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
