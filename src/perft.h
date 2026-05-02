// Hypersion — perft (move-generation correctness test).
// Counts leaf nodes at a given depth for a position. Used to verify that
// movegen + do_move + undo_move are bug-free against known reference counts.

#ifndef HYPERSION_PERFT_H
#define HYPERSION_PERFT_H

#include <cstdint>

#include "position.h"

namespace hypersion {

// Bulk-counted perft: sums leaf nodes. At depth 1 we just return the legal
// move count, which is ~10x faster than recursing to d=0 trivially.
std::uint64_t perft(Position& pos, int depth);

// Like perft() but prints "<move>: <count>" per root move and the total at
// the end. Matches Stockfish's `go perft N` output for cross-checking.
void perft_divide(Position& pos, int depth);

// Run the canonical 7-position perft suite and report pass/fail.
void perft_run_suite();

}  // namespace hypersion

#endif  // HYPERSION_PERFT_H
