// The page's motion, built with Motion (https://motion.dev) and bundled to
// /site-motion.js by scripts/release/render-site.cjs. Every animation starts
// from the hidden state site.css gives `.motion` pages, and only transform,
// opacity and filter move.
import { animate, inView, scroll, stagger } from "motion";

const root = document.documentElement;
const ease = [0.22, 1, 0.36, 1];

if (root.classList.contains("motion")) {
  root.classList.add("motion-ready");

  // Hero: the headline rises word by word out of its mask, then the rest
  // of the hero settles in behind it.
  const words = document.querySelectorAll("#headline .word > span");
  animate(words, { transform: ["translateY(105%)", "translateY(0%)"] },
    { duration: 0.9, ease, delay: stagger(0.06, { startDelay: 0.12 }) });
  animate(".hero [data-enter]",
    { opacity: [0, 1], transform: ["translateY(14px)", "translateY(0px)"], filter: ["blur(6px)", "blur(0px)"] },
    { duration: 0.8, ease, delay: stagger(0.08, { startDelay: 0.35 }) });

  // The app: the window lies back like an opened lid and settles flat as
  // it scrolls into view.
  const window_ = document.querySelector("[data-tilt]");
  if (window_) {
    scroll(
      animate(window_, {
        transform: [
          "perspective(1800px) rotateX(16deg) scale(0.92) translateY(32px)",
          "perspective(1800px) rotateX(0deg) scale(1) translateY(0px)"
        ],
        opacity: [0.5, 1]
      }, { ease: "linear" }),
      { target: window_, offset: ["start end", "start 0.3"] }
    );
  }

  // Everything else reveals once, as it comes into view; cards in a row
  // follow each other.
  inView("[data-reveal]", (element) => {
    const row = element.parentElement ? [...element.parentElement.children].filter((child) => child.hasAttribute("data-reveal")) : [];
    const index = Math.max(0, row.indexOf(element)) % 3;
    animate(element,
      { opacity: [0, 1], transform: ["translateY(24px)", "translateY(0px)"] },
      { duration: 0.8, ease, delay: index * 0.08 });
  }, { amount: 0.3 });

  // The closing mark lands with a little spring.
  inView(".closing-mark", (element) => {
    animate(element, { transform: ["scale(0.8) rotate(-10deg)", "scale(1) rotate(0deg)"] },
      { type: "spring", bounce: 0.35, visualDuration: 0.7 });
  }, { amount: 0.6 });
}

// The navigation gains its hairline once the page has scrolled.
const nav = document.querySelector(".nav");
if (nav) scroll((_, info) => nav.classList.toggle("scrolled", info.y.current > 8));

// Feature cards: a soft light follows the pointer across each one.
for (const card of document.querySelectorAll(".feature")) {
  card.addEventListener("pointermove", (event) => {
    const box = card.getBoundingClientRect();
    card.style.setProperty("--mx", `${event.clientX - box.left}px`);
    card.style.setProperty("--my", `${event.clientY - box.top}px`);
  });
}
