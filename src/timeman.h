// Hypersion — time management.
// Translates a UCI `go` time command into a soft deadline (target ms to spend
// on this move) and a hard deadline (absolute cap before we must move now).

#ifndef HYPERSION_TIMEMAN_H
#define HYPERSION_TIMEMAN_H

#include <cstdint>
#include <vector>

#include "misc.h"
#include "types.h"

namespace hypersion {

struct SearchLimits {
    int     depth     = 0;          // 0 = unlimited
    int64_t movetime  = 0;          // exact ms (0 = unset)
    int64_t time[COLOR_NB] = {};    // remaining clock in ms per side
    int64_t inc [COLOR_NB] = {};    // increment in ms per side
    int     movestogo = 0;          // 0 = sudden death
    int64_t nodes     = 0;          // hard node cap (0 = unset)
    bool    infinite  = false;      // ignore time / depth, search until "stop"
    bool    ponder    = false;
    int     mate      = 0;
    // SF18-style: when the GUI's UCI `Ponder` option is enabled, the engine
    // knows pondering will cover part of think time. TimeManager uses this
    // to bump the optimum-time budget by 25 %, since wall-clock optimum
    // shrinks when part of the work happens on the opponent's clock.
    bool    ponderEnabled = false;

    // `go searchmoves m1 m2 ...` — restrict the root search to these moves.
    // Empty vector means "search all legal root moves" (the normal case).
    // Used by tournament managers / analysis features and OpenBench.
    std::vector<Move> searchMoves;

    // Skill / strength tuning (Stockfish-style). 20 = full strength;
    // lower values cap depth and add move-selection noise.
    int     skillLevel    = 20;
    bool    limitStrength = false;
    int     uciElo        = 1500;
    int     multiPv       = 1;
    int     moveOverhead  = 30;       // ms reserved for GUI / network jitter
    int     contempt      = 0;        // cp added to draw eval (positive = avoid draw)
    bool    showWDL       = false;    // emit `wdl W D L` per-iteration
    bool    analyseMode   = false;    // UCI_AnalyseMode: reduce pruning for thoroughness

    // 1-indexed counter of own-search moves this game. Set by uci.cpp
    // (g_ownSearchesThisGame) before Search::Threads.start(). Book hits
    // don't increment. Reset to 0 in cmd_ucinewgame.
    //
    // Originally added for the v8 H1 "empty-TT boost" experiment which
    // boosted timeman.cpp::optimumTime by 30 % when this is in [1..3].
    // That experiment was REJECTED (SPRT -27 ELO @ 200g, see tombstone
    // in timeman.cpp). The field and counter are kept in the codebase
    // so a future variant (e.g. aspiration-window adjustments, TT pre-
    // warming, or a different boost magnitude) can reuse the wiring.
    int     ownSearchIndex = 0;

    // 2026-05-17 audit uci #45: SF18 captures the `go` arrival time in
    // UCIEngine::go() (uci.cpp:204) and threads it through to TimeManager.
    // Previously Hypersion captured startTime inside TimeManager::init()
    // — i.e. AFTER cmd_go's book probe + ThreadPool::start() spinup, so
    // ~5-30 ms of pre-search latency was uncounted at bullet TCs. Now
    // cmd_go writes goStartTime = now() at the very top, and the
    // TimeManager uses it as the anchor if nonzero.
    TimePoint goStartTime = 0;
};

class TimeManager {
public:
    void init(const SearchLimits& limits, Color us, int ply);
    void reset_start() { startTime = now(); }    // re-anchor (e.g. on ponderhit)
    TimePoint optimum() const { return optimumTime; }
    TimePoint maximum() const { return maximumTime; }
    TimePoint elapsed() const { return now() - startTime; }
    TimePoint start()   const { return startTime; }
private:
    TimePoint startTime   = 0;
    TimePoint optimumTime = 0;
    TimePoint maximumTime = 0;
};

}  // namespace hypersion

#endif  // HYPERSION_TIMEMAN_H
