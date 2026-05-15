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

    // Cap overhead at half of remaining time. Without this, when
    // wtime drops below moveOverhead (~2000 ms default), `remaining`
    // clamps to 1 and the entire move budget collapses to MIN_MOVE_TIME_MS
    // (10 ms). That's the bullet-flag-out bug: at wtime <2 s the engine
    // panics with a 10 ms budget per move, often producing no completed
    // search iteration.  Capping overhead at wtime/2 guarantees at least
    // wtime/2 / MOVE_HORIZON time per move (e.g. wtime=500 -> 6 ms per
    // move at MOVE_HORIZON=40, still tight but the search runs).
    int overhead = std::max(0, limits.moveOverhead);
    if (limits.time[us] > 0)
        overhead = std::min<int>(overhead, int(limits.time[us] / 2));

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

    // SF18-style: gradually reduce moves-to-go when time is low (< 1 sec).
    // SF formula: centiMTG = scaledTime * 5.051 (so mtg = scaledTime / 19.8).
    // Without this, the engine assumes ~40 more moves to play even when
    // it has 200ms left, producing optimum=5ms per move - too tight.
    // With this, at remaining=500ms -> mtg=25, optimum=20ms. At
    // remaining=200ms -> mtg=10, optimum=20ms. Much safer for bullet
    // flag-out conversion.
    if (limits.movestogo == 0 && remaining < 1000)
        mtg = std::max(2, int(remaining / 20));

    // Optimum: divide remaining roughly evenly + a chunk of the increment.
    int64_t optimum = remaining / mtg + (inc * 3) / 4;
    optimum = std::min(optimum, remaining / 2);   // never spend more than half on a single move

    // Maximum: ~5x optimum, but capped at ~20 % of remaining so a runaway
    // iteration can't blow our whole clock on one move.
    int64_t maximum = std::min<int64_t>(optimum * 5, remaining / 5);
    maximum = std::max(maximum, optimum);

    optimumTime = std::max<int64_t>(optimum, MIN_MOVE_TIME_MS);
    maximumTime = std::max<int64_t>(maximum, MIN_MOVE_TIME_MS);

    // Ponder bonus (Stockfish 18 src/timeman.cpp). When the GUI's UCI
    // `Ponder` option is on, the engine spends part of think time on the
    // opponent's clock — so the wall-clock optimum here can safely be 25 %
    // larger. Bumps optimum only; maximum stays the safe hard ceiling.
    if (limits.ponderEnabled)
        optimumTime += optimumTime / 4;

    // ----- Tombstone: empty-TT boost (v8 H1) -----------------------------
    // 2026-05-15: boosted optimumTime by 30 % for the first 3 own searches
    // per game (ownSearchIndex ∈ [1..3]). Hypothesis: TT-empty searches
    // after book-exit lacked continuity, so extra time would let the
    // engine deepen by ~1 ply and pick up TT entries. Motivated by
    // bullet game analysis showing 3 of 6 Black-vs-1.e4 losses had
    // -150 to -550 cp blunders in moves 8-15.
    //
    //   SPRT (TC 5+0.05, conc=6):
    //     Stage 1  30g triage : -58.5 +/- 102.5 ELO (7W-12L-11D)
    //     Stage 2 200g        : -24.4 +/- 39.3 ELO (59W-73L-68D)
    //   REJECTED.
    //
    // Failure mode observation: candidate-as-Black scored 39 % vs 50 %
    // at parity (very asymmetric). The +1-ply deepening on first moves
    // seems to surface worse Black evals at higher depth and the engine
    // commits to defensive lines that lose initiative.
    //
    // Also: actual blunders in our lichess sample happened at moves
    // 8-15-20-30, NOT in the first 3 own moves. The boost targeted the
    // wrong window.
    //
    // Don't re-test the same 1.3x boost. Future ideas if pursuing:
    //   1. Skip aspiration window (start [-INF, +INF]) for move 1.
    //   2. Boost later moves (5-15) where blunders actually cluster.
    //   3. Pre-warm TT with a shallow probe at ucinewgame.
    // The SearchLimits::ownSearchIndex field and the uci.cpp counter
    // stay in tree so future variants can re-use the wiring.
    // ---------------------------------------------------------------------

    // ----- Tombstone: under-spend first 3 own moves (v16) ----------------
    // 2026-05-15: opposite-direction test of v8 H1. Reduced optimumTime
    // by 23 % (×10/13, inverse of v8 H1's ×13/10) on first 3 own searches.
    // Hypothesis: v8 H1 + v13 both regressed via Black-side asymmetry
    // when adding effort to first-move search; reducing effort might
    // help Black avoid revealing structural disadvantage too clearly,
    // and the saved clock budget would flow into moves 8-30 where the
    // actual blunders cluster.
    //
    //   SPRT (TC 5+0.05, conc=6):
    //     Stage 1  30g triage : +70.4 +/- 99.8  ELO (12W- 6L-12D)
    //     Stage 2 200g        : -15.6 +/- 38.2  ELO (58W-67L-75D)
    //   REJECTED.
    //
    // Classic 30g-fakeout pattern (CLAUDE.md / PROTOCOL.md). Stage 1's
    // strong positive was opening-set variance; Stage 2 reverted to
    // mildly negative. Final score: NEW playing White 30-31-38 (49.5 %),
    // NEW playing Black 28-35-35 (46.4 %) — both sides slightly under
    // parity, no asymmetry signal.
    //
    // Combined with v8 H1 (-24 ELO at +30 %) and v13 (-10 ELO at wider
    // aspiration), this confirms the first-move time/window settings
    // are at a TIGHT local optimum at TC 5+0.05. Neither direction
    // helps. Future contributors: don't touch optimum-time scaling at
    // first-move granularity without paired re-tuning of the underlying
    // time-budget formula (mtg, overhead) — single-feature shifts here
    // are net-negative.
    // ----------------------------------------------------------------------
    (void)limits.ownSearchIndex;   // tombstoned: no time scaling

    // ----- Tombstone: TC-adaptive LMR divisor (v30/v30b) -------------------
    // 2026-05-15: attempted runtime-switching LMR divisor (1.87 at bullet,
    // 1.85 at LTC) via precomputed dual tables ReductionsBullet[][] and
    // ReductionsLTC[][] in search.cpp, with a Search::UseBulletLMR global
    // set here based on (a) optimumTime < 300 ms (v30) then (b) inc < 100
    // (v30b after v30 had instability mid-game).
    //
    // **Both implementations were behaviorally NO-OPS.** Verified by
    // computing the int() of log(d)*log(mc)/divisor across the full
    // [d, mc] in {[2..64], [2..64]} grid: every cell rounds to the
    // SAME integer for 1.85 and 1.87 — the 1.07 % fractional difference
    // is absorbed by integer truncation. So the SPRT signal observed
    // (v30b vs v18 at LTC: -25 ELO at 27g, CI ±110) was pure noise from
    // the +2 KB struct-layout shift across translation units, NOT a real
    // search-behavior difference.
    //
    // For a real TC-adaptive divisor, the LMR table would need to keep
    // fractional precision — e.g. store reductions in 1/1024ths and round
    // per-call. Out of scope for autonomous-loop work. Future contributors
    // pursuing this: refactor Reductions[][] to fixed-point 1024× before
    // adding the runtime selector.
    // -----------------------------------------------------------------------
}

}  // namespace hypersion
