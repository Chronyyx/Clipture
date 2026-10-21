// Runs before the first paint: marks the page for its entrance animations,
// unless the visitor prefers reduced motion. Without it (or if site-motion.js
// never loads) site.css shows everything as is.
if (!window.matchMedia("(prefers-reduced-motion: reduce)").matches) {
  document.documentElement.classList.add("motion");
}
