// Hypersion — time-budget computation.
//
// Two budgets are produced:
//   * `optimumTime` — soft target for one search iteration. We stop the next
//     iteration if we've already spent this much.
//   * `maximumTime` — hard ceiling. We must return a move before this elapses
//     to avoid losing on time, including the GUI's network jitter buffer.
//
// Both are derived from `remaining`, `inc`, and an estimated `movestogo`.

#include "timeman.h"

#include <algorithm>

namespace hypersion {

namespace {
constexpr int    MOVE_HORIZON      = 40;     // assume ~40 moves left if no movestogo
constexpr int    MIN_MOVE_TIME_MS  = 10;
}  // namespace

void TimeManager::init(const SearchLimits& limits, Color us, int /*ply*/) {
    startTime = now();

    int overhead = std::max(0, limits.moveOverhead);

    if (limits.movetime > 0) {
        // Account for the GUI's own latency on a movetime command too.
        int64_t mt = std::max<int64_t>(MIN_MOVE_TIME_MS, limits.movetime - overhead);
        optimumTime = mt;
        maximumTime = mt;
        return;
    }

    // Depth / nodes / infinite searches bypass clock-based budgeting entirely.
    if (limits.depth > 0 || limits.nodes > 0 || limits.infinite || limits.mate > 0) {
        optimumTime = INT64_MAX / 4;
        maximumTime = INT64_MAX / 4;
        return;
    }

    int64_t remaining = std::max<int64_t>(limits.time[us] - overhead, 1);
    int64_t inc       = std::max<int64_t>(limits.inc[us], 0);
    int     mtg       = limits.movestogo > 0 ? limits.movestogo : MOVE_HORIZON;

    // Optimum: divide remaining roughly evenly + a chunk of the increment.
    int64_t optimum = remaining / mtg + (inc * 3) / 4;
    optimum = std::min(optimum, remaining / 2);   // never spend more than half on a single move

    // Maximum: ~5x optimum, but capped at ~20 % of remaining so a runaway
    // iteration can't blow our whole clock on one move.
    int64_t maximum = std::min<int64_t>(optimum * 5, remaining / 5);
    maximum = std::max(maximum, optimum);

    optimumTime = std::max<int64_t>(optimum, MIN_MOVE_TIME_MS);
    maximumTime = std::max<int64_t>(maximum, MIN_MOVE_TIME_MS);
}

}  // namespace hypersion
