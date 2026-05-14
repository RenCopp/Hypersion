// Hypersion — Syzygy tablebase wrapper around the Fathom probing library.
//
// At the root of search we ask Fathom for the WDL/DTZ verdict. If the position
// is in the tablebases we report the truth (mate distance / draw / loss) and
// restrict the root move list to moves that preserve the win or hold the draw.

#ifndef HYPERSION_SYZYGY_H
#define HYPERSION_SYZYGY_H

#include <string>
#include <vector>

#include "types.h"

namespace hypersion {
class Position;
}

namespace hypersion::Syzygy {

bool init(const std::string& path);   // empty path = unload
bool is_loaded();
int  largest();                        // max piece count covered

// Tunable knobs (Stockfish-compatible UCI options).
void set_probe_depth(int d);    // SyzygyProbeDepth: only probe when search depth >= d
int  probe_depth();
void set_probe_limit(int n);    // SyzygyProbeLimit: skip probes when pieces > n
int  probe_limit();
void set_50_move_rule(bool b);  // Syzygy50MoveRule: passes rule50 to TB lookup if true
bool fifty_move_rule();

// Probe at the root. On success, fills `bestMove` with the principal Syzygy
// recommendation and `score` with a TB-aware value (cp scale). Returns true
// if the position was found in the tablebases.
//
// LEGACY single-move probe. Kept for back-compat; new code should use
// probe_root_dtz() below to get per-move ranking that lets the search
// filter root moves to the TB-best bucket and prefer fastest conversion.
struct RootProbe {
    Move  bestMove = Move::none();
    Value score    = VALUE_NONE;
    int   wdl      = 0;     // -2/-1/0/+1/+2
};
bool probe_root(const Position& pos, RootProbe& out);

// Per-move TB ranking from Fathom's tb_probe_root_dtz. Each entry is a
// legal root move with a tbRank (higher = better, Stockfish-compatible
// 0-1000 scale where 1000 = winning DTZ-optimal, 0 = draw, negative = lose),
// a tbScore (cp-scale Value), and the raw WDL value. Use rank as primary
// sort key, fall back to NNUE within the same rank bucket.
//
// Crucially this works regardless of rule50_count (unlike the legacy
// probe_root which gated on rule50 == 0).
struct RootMoveEntry {
    Move  move;
    int   rank;     // higher = better; ties broken by usual eval
    Value score;    // TB-derived score for the move
    int   wdl;      // -2/-1/0/+1/+2
};
bool probe_root_dtz(const Position& pos, std::vector<RootMoveEntry>& out);

// In-search WDL probe — fast, no DTZ. Returns VALUE_NONE if not found.
Value probe_wdl(const Position& pos);

}  // namespace hypersion::Syzygy

#endif
