# clipture.app design

The site is plain HTML, CSS and a little vanilla JS in `web/`, assembled by
`scripts/release/render-site.cjs`. The CSP allows same-origin scripts, styles,
fonts and images only, so there is no framework, CDN or animation library.

## Brief

Clipture is a free, open-source Windows game clipper. Visitors are PC players
with an NVIDIA GPU. The page has one job: get the download, after letting
people try the real app (the `/demo/` build) in place.

## Concept

```
Idea:        The page records you the way Clipture records a game. A 60-second
             buffer of your visit rolls along the hero; Ctrl+Shift+S keeps it.
Metaphor:    the replay buffer and the recorder's tally light
Signature:   the buffer strip: one bar per second of pointer, scroll and key
             activity, sliding left as time passes. Ctrl+Shift+S (or Save) flashes
             it, holds the kept seconds in tally orange, and the tally line reads
             "Saved 0:42". Nothing else appears.
Emotion:     the calm of something quietly recording, then the small thrill of
             keeping a moment you didn't plan
Motion verb: roll and lock. Time rolls in from the right; a save locks it.
Screenshot:  the poster headline over the strip, just saved, bars in orange
```

Alternatives considered: a camera viewfinder frame (REC brackets, timecode),
rejected as generic recording chrome; the page as an editor timeline with
features as track lanes, whose waveform bars survive in the strip.

## System

- **Palette** (the app's Graphite theme): ground `#0f0f0e`, raised `#181817`,
  ink `#f7f6f2`, text `#e4e4dd`, muted `#a9a89e`, line `#2a2a27`, tally `#f07836`.
  Near-black with one orange accent is a common default; here the orange is the
  recorder's tally light in the app, so it marks only recording, saving and
  "now", never decoration.
- **Type:** Archivo (variable width and weight, self-hosted) for display at
  125% width, 800; Segoe UI Variable for text and UI (the audience is on
  Windows, it matches the app and costs nothing); the system mono (Cascadia
  Mono) for timecode and keys, with tabular figures.
- **Grid:** 12 columns, gutter `clamp(16px, 2.5vw, 32px)`, content hung from
  column 1; the hero is left-aligned, not centered.
- **Shape:** 6 px radius on controls, 12 px on the app window.
- **Motion:** one ease, `cubic-bezier(0.165, 0.84, 0.44, 1)`; hovers 0.3 to
  0.45 s. Buttons roll their label, text links draw an underline, the Save
  keys press down. Nothing else animates on hover. The buffer strip is the only thing
  moving at rest, on one requestAnimationFrame clock that stops off screen, in
  hidden tabs and under reduced motion (which shows a still strip).

## Sections

Reduced to what carries the idea. Anything that repeated the hero or the demo
was cut: section nav, a three-step timeline, a themes row, and a floating
shelf of saved clips (a save now resolves in the strip itself).

```
Hero (poster)     REC tally and buffer time
                  KEEP THE LAST / 60 SECONDS / OF YOUR GAME.
                  [|||| ||  |||||||| buffer strip ............ now]
                  lede                          [Ctrl][Shift][S]  [Download]
                                                requirements
Demo (full-bleed) the real app in a window frame; phones get it at their width
Features (index)  four rows of facts: spec | name | one line
Finale            Ctrl + Shift + S at poster size, one line, Download
```

The 404 carries the concept: an empty, flat buffer, "That moment wasn't in
the buffer."
