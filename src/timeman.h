// Hypersion — time management.
// Translates a UCI `go` time command into a soft deadline (target ms to spend
// on this move) and a hard deadline (absolute cap before we must move now).

#ifndef HYPERSION_TIMEMAN_H
#define HYPERSION_TIMEMAN_H

#include <cstdint>

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

    // Skill / strength tuning (Stockfish-style). 20 = full strength;
    // lower values cap depth and add move-selection noise.
    int     skillLevel    = 20;
    bool    limitStrength = false;
    int     uciElo        = 1500;
    int     multiPv       = 1;
    int     moveOverhead  = 30;       // ms reserved for GUI / network jitter
    int     contempt      = 0;        // cp added to draw eval (positive = avoid draw)
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
