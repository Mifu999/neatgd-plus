#pragma once

#include <cstdint>
#include <random>
#include <vector>

namespace neatgd {

// "Climber" / hybrid lock tuning. A tape is a sorted list of step indices at
// which the jump input flips state, starting from released. The search locks
// the prefix that already works and only mutates / explores the failing suffix.
struct SequenceParams {
    // exploration past the frontier (gaps biased short so fine tapping/hovering
    // is reachable, but long drifts remain possible)
    int exploreWindow = 360;   // how far beyond the frontier to generate inputs
    int gapMin = 1;            // frames between presses (release duration)
    int gapMax = 22;
    int pressMin = 1;          // frames a press is held
    int pressMax = 8;

    // modest "re-tune the wall" look-back (grows with stuck, then capped)
    int lookbackBase = 24;
    int lookbackGrow = 9;
    int lookbackMax = 600;

    // deep backtracking when stuck for a long time (escape dead-ends)
    int deepStuckScale = 700;  // stuck count at which deep rewinds reach ~max rate
    double deepMaxProb = 0.6;  // ceiling on the per-attempt deep-rewind chance

    // hybrid tape repair
    int repairAfter = 150;     // attempts stuck before the tape is repaired at all
    int repairBase = 90;       // smallest repair window, in frames
    int repairTierStuck = 900; // stuck attempts per tier doubling
    int repairMaxTiers = 7;    // ceiling on the doubling (90 << 7 = 11520)

    // suffix mutation (scaled up the longer a spot stays stuck)
    double jitterProb = 0.85;  // chance to nudge existing toggles' timing
    int jitterMax = 6;         // base max frames a toggle is nudged
    double addProb = 0.30;     // base chance to insert a toggle
    double removeProb = 0.25;  // base chance to drop a toggle
};

// Modest look-back: re-tunes just the approach to the wall. Grows with stuck
// but is capped, so on its own it can't escape an early dead-end.
int climberModestLookback(SequenceParams const& p, int stuck);

// Per-attempt look-back with escalating deep rewinds. Most attempts use the
// modest look-back (fast iteration on the wall); the longer a spot stays stuck,
// the more often it rewinds deep - anywhere up to the whole frontier - so it can
// re-search earlier locked sections and break out of a dead-end.
int climberBacktrack(
    SequenceParams const& p, int stuck, int frontier, std::mt19937& rng);

// Repair window for the hybrid mode: how many frames of the shared locked tape
// to unlock and re-search on this attempt.
//
// Unlike climberBacktrack this is deliberately BOUNDED and tiered. In hybrid
// mode the network has to re-play everything from the unlock point onward, so
// unlocking a uniformly random slice of the whole level (which is what the
// first implementation did) throws away all the progress after it and the
// attempt is wasted. Tiers grow with `stuck`, and each draw is biased toward
// the small end, so most attempts re-tune the clicks just before the wall -
// which is where the fix usually is - while occasional deeper ones can undo an
// earlier mistake.
int repairWindow(
    SequenceParams const& p, int stuck, int frontier, std::mt19937& rng);

// Builds the candidate tape for one attempt: toggles before
// `frontier - lookback` are locked verbatim; toggles from there up to the
// frontier are mutated; the unexplored region up to `horizon` is freshly
// generated. `stuck` scales how aggressive the mutation is. The result is
// sorted, de-duplicated and non-negative.
std::vector<int> buildCandidate(
    std::vector<int> const& best, int frontier, int lookback, int horizon,
    SequenceParams const& p, std::mt19937& rng, int stuck = 0);

}
