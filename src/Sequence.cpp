#include "Sequence.hpp"

#include <algorithm>

namespace neatgd {

namespace {

void sanitize(std::vector<int>& tape) {
    for (auto& t : tape) if (t < 0) t = 0;
    std::sort(tape.begin(), tape.end());
    tape.erase(std::unique(tape.begin(), tape.end()), tape.end());
}

// Append fresh press/release toggles covering [from, horizon). Each short
// "burst" picks a random rhythm style so the search covers the range of GD
// input patterns: rapid taps (ship/wave hovering), sustained holds (climbs),
// and sparse taps (cube). Without this variety, tight sections that need a
// specific cadence are almost never sampled.
void generateFresh(std::vector<int>& out, int from, int horizon,
                   SequenceParams const& p, std::mt19937& rng) {
    if (horizon <= from) return;
    std::uniform_real_distribution<double> u(0.0, 1.0);
    int const gLo = std::max(1, p.gapMin), gHi = std::max(p.gapMin + 1, p.gapMax);
    int const pLo = std::max(1, p.pressMin), pHi = std::max(p.pressMin + 1, p.pressMax);

    int cur = from;
    int guard = 0;
    while (cur < horizon && guard++ < 200000) {
        // pick a rhythm style for this burst and how long to hold it
        double const r = u(rng);
        if (r < 0.12) {            // coast: emit no input for a stretch
            cur += 6 + static_cast<int>(u(rng) * 60.0);
            continue;
        }
        int gapLo, gapHi, prLo, prHi;
        if (r < 0.48) {            // rapid taps / hover
            gapLo = 1; gapHi = 3; prLo = 1; prHi = 2;
        } else if (r < 0.70) {     // sustained hold
            gapLo = 1; gapHi = 4; prLo = std::max(3, pLo); prHi = pHi + 4;
        } else if (r < 0.88) {     // sparse taps
            gapLo = std::max(4, gLo); gapHi = gHi; prLo = 1; prHi = 3;
        } else {                   // mixed / general
            gapLo = gLo; gapHi = gHi; prLo = pLo; prHi = pHi;
        }
        std::uniform_int_distribution<int> gap(gapLo, std::max(gapLo + 1, gapHi));
        std::uniform_int_distribution<int> press(prLo, std::max(prLo + 1, prHi));
        int const burst = 2 + static_cast<int>(u(rng) * 10.0);  // 2-11 presses
        for (int b = 0; b < burst && cur < horizon; ++b) {
            cur += gap(rng);
            if (cur >= horizon) break;
            out.push_back(cur);            // press on
            cur += press(rng);
            out.push_back(cur);            // press off
        }
    }
}

}

int climberModestLookback(SequenceParams const& p, int stuck) {
    long long lb = static_cast<long long>(p.lookbackBase)
        + static_cast<long long>(p.lookbackGrow) * std::min(std::max(0, stuck), 40);
    return static_cast<int>(std::min<long long>(lb, p.lookbackMax));
}

int climberBacktrack(
    SequenceParams const& p, int stuck, int frontier, std::mt19937& rng) {
    int const modest = climberModestLookback(p, stuck);
    if (frontier <= modest) return frontier;  // tiny run: re-search everything

    double const pDeep = std::min(
        p.deepMaxProb,
        static_cast<double>(std::max(0, stuck))
            / static_cast<double>(std::max(1, p.deepStuckScale)));
    std::uniform_real_distribution<double> u(0.0, 1.0);
    if (u(rng) < pDeep) {
        // deep rewind: anywhere from the modest window up to the whole frontier;
        // min of two draws biases slightly toward smaller (cheaper) rewinds, but
        // large rewinds (even back to step 0) remain possible.
        std::uniform_int_distribution<int> d(modest, frontier);
        return std::min(d(rng), d(rng));
    }
    return modest;
}

int repairWindow(
    SequenceParams const& p, int stuck, int frontier, std::mt19937& rng) {
    if (frontier <= 0) return 0;
    int const base = std::max(1, p.repairBase);
    if (stuck < p.repairAfter) return std::min(base, frontier);

    // How many scale doublings the search has unlocked so far.
    int const unlocked = std::min(
        std::max(0, stuck - p.repairAfter) / std::max(1, p.repairTierStuck),
        std::max(0, p.repairMaxTiers));

    // Pick the SCALE uniformly, not the size. That makes the window
    // log-uniform, so every order of magnitude keeps a fair share of attempts:
    // small windows (where the fix usually is - the clicks right before the
    // wall) stay frequent forever, while large ones remain reachable. Drawing
    // the size directly would let the search drift to the biggest window and
    // waste itself re-discovering hundreds of clicks by chance.
    int const tier = std::uniform_int_distribution<int>(0, unlocked)(rng);
    long long hi = static_cast<long long>(base) << tier;
    long long lo = tier == 0 ? base : hi / 2;
    hi = std::min<long long>(hi, frontier);
    lo = std::min<long long>(lo, hi);
    return std::uniform_int_distribution<int>(
        static_cast<int>(lo), static_cast<int>(hi))(rng);
}

std::vector<int> buildCandidate(
    std::vector<int> const& best, int frontier, int lookback, int horizon,
    SequenceParams const& p, std::mt19937& rng, int stuck) {
    int const lockUntil = std::max(0, frontier - std::max(0, lookback));

    std::vector<int> out;       // locked prefix
    std::vector<int> tail;      // mutable region (>= lockUntil) from best
    out.reserve(best.size() + 16);
    for (int t : best) {
        if (t < lockUntil) out.push_back(t);
        else tail.push_back(t);
    }

    // mutation aggressiveness grows the longer this spot has been stuck
    int const jitterMax = p.jitterMax + std::min(std::max(0, stuck) / 120, 18);
    double const addP =
        std::min(0.6, p.addProb + std::max(0, stuck) / 3000.0);
    double const removeP =
        std::min(0.6, p.removeProb + std::max(0, stuck) / 3000.0);
    int const jitterCount = 1 + std::min(std::max(0, stuck) / 250, 4);

    std::uniform_real_distribution<double> u(0.0, 1.0);

    // mutate the tail timings
    if (!tail.empty()) {
        if (u(rng) < p.jitterProb) {
            std::uniform_int_distribution<int> jit(-jitterMax, jitterMax);
            for (int k = 0; k < jitterCount; ++k) {
                size_t const idx =
                    static_cast<size_t>(u(rng) * tail.size()) % tail.size();
                tail[idx] = std::max(lockUntil, tail[idx] + jit(rng));
            }
        }
        // possibly several inserts / removes when very stuck
        int const ops = 1 + std::min(std::max(0, stuck) / 400, 3);
        for (int k = 0; k < ops; ++k) {
            if (tail.size() > 1 && u(rng) < removeP) {
                size_t const idx =
                    static_cast<size_t>(u(rng) * tail.size()) % tail.size();
                tail.erase(tail.begin() + idx);
            }
            if (u(rng) < addP) {
                int const hi = std::max(lockUntil + 1, horizon);
                std::uniform_int_distribution<int> at(lockUntil, hi);
                tail.push_back(at(rng));
            }
        }
    }

    // figure out where explored material currently ends
    int lastStep = lockUntil;
    if (!tail.empty())
        lastStep = std::max(lastStep, *std::max_element(tail.begin(), tail.end()));
    else if (!out.empty())
        lastStep = std::max(lastStep, out.back());

    // explore the unknown region ahead
    generateFresh(tail, std::max(lockUntil, lastStep), horizon, p, rng);

    out.insert(out.end(), tail.begin(), tail.end());
    sanitize(out);
    return out;
}

}
