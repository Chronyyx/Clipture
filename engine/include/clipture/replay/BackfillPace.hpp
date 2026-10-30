#pragma once

namespace clipture::replay {
// Copy speeds for footage leased from a saved clip, as multiples of the rate
// footage arrives. Each in-place save starts where the previous one ended
// (ADR 0026), so only decoder preroll and AAC lookbehind are copied back.
struct BackfillPace {
    // Preroll copy-back right after a save; small, so it finishes at once.
    static constexpr double maximumMultiplier = 8.0;
    // A save that arrives before that finishes copies the rest itself, faster
    // (someone is waiting) but never at full drive speed.
    static constexpr double saveCopyMultiplier = 16.0;
};
}
