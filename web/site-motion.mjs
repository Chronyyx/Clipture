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

  // Linked chips: every few seconds while on screen, a few sparkles pop
  // around the chip and its letters hop in turn. Hover or focus does it too.
  const star = "M5 0 6.2 3.8 10 5 6.2 6.2 5 10 3.8 6.2 0 5 3.8 3.8Z";
  for (const chip of document.querySelectorAll("[data-sparkle]")) {
    const label = chip.textContent.trim();
    chip.setAttribute("aria-label", label);
    chip.textContent = "";
    const letters = [...label].map((character) => {
      const letter = document.createElement("span");
      letter.className = "letter";
      letter.setAttribute("aria-hidden", "true");
      letter.textContent = character;
      chip.append(letter);
      return letter;
    });
    const sparkles = Array.from({ length: 4 }, () => {
      const sparkle = document.createElementNS("http://www.w3.org/2000/svg", "svg");
      sparkle.setAttribute("class", "sparkle");
      sparkle.setAttribute("viewBox", "0 0 10 10");
      sparkle.setAttribute("aria-hidden", "true");
      sparkle.innerHTML = `<path fill="currentColor" d="${star}"/>`;
      chip.append(sparkle);
      return sparkle;
    });

    let playing = false;
    const play = () => {
      if (playing) return;
      playing = true;
      // Sparkles sit just outside the chip's edges, a different spot each time.
      sparkles.forEach((sparkle, index) => {
        const side = (index + Math.floor(Math.random() * 2)) % 4;
        const along = 10 + Math.random() * 80;
        sparkle.style.left = side === 1 ? "calc(100% + 4px)" : side === 3 ? "-4px" : `${along}%`;
        sparkle.style.top = side === 0 ? "-4px" : side === 2 ? "calc(100% + 4px)" : `${along}%`;
      });
      animate(sparkles,
        { opacity: [0, 1, 0], transform: ["scale(0) rotate(0deg)", "scale(1.15) rotate(70deg)", "scale(0) rotate(140deg)"] },
        { duration: 0.9, ease: "easeOut", delay: stagger(0.09) });
      animate(letters,
        { transform: ["translateY(0px)", "translateY(-4px)", "translateY(0px)"] },
        { duration: 0.42, ease: ["easeOut", "easeIn"], delay: stagger(0.028, { startDelay: 0.12 }) })
        .then(() => { playing = false; });
    };
    chip.addEventListener("pointerenter", play);
    chip.addEventListener("focus", play);

    inView(chip, () => {
      let timer = setTimeout(function next() {
        if (!document.hidden) play();
        timer = setTimeout(next, 5200 + Math.random() * 2600);
      }, 900);
      return () => clearTimeout(timer);
    }, { amount: 1 });
  }

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
