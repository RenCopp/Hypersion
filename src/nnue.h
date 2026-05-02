// Hypersion — Stockfish 18 NNUE evaluator.
//
// Reads SF18's binary .nnue format (SFNNv10 architecture, FullThreats +
// HalfKAv2_hm) and runs forward inference. Adapted from HybridChess_v17's
// self-contained NNUE module — no Stockfish source is linked, the module
// only needs Hypersion's existing Position / bitboard helpers.
//
// Two networks are supported:
//   * BIG   — 1024-d FT, with FullThreats features (default `EvalFile`)
//   * SMALL — 128-d FT, no threats (default `EvalFileSmall`)
//
// The eval picks BIG by default and switches to SMALL when material
// imbalance crosses a threshold (matches SF18's logic). Both networks
// can be loaded; only one needs to be for the engine to run.

#pragma once

#include <string>

#include "types.h"

namespace hypersion {
class Position;
}

namespace hypersion::NNUE {

// Build the threat-feature lookup tables. Cheap; called once at engine
// startup. Subsequent calls are no-ops.
void init();

// Load a network from disk. `is_big` selects which slot to fill.
// Returns true on success. Side-effects: prints info-string lines about
// the loaded architecture / hashes.
bool load_big  (const std::string& path);
bool load_small(const std::string& path);

// True if at least one network slot is populated.
bool is_loaded();

// Drop both networks. Frees ~110 MB if a big net was loaded.
void unload();

// Invalidate the Finny refresh cache. Call on `ucinewgame` so a fresh
// game doesn't carry stale piece-set snapshots from the previous game.
void new_game();

// Stockfish 18 NNUE evaluation, returned in centipawns from the side-to-
// move's perspective. Falls back to VALUE_ZERO if no network is loaded
// (caller should check is_loaded() first or use evaluate.cpp's dispatch).
Value evaluate(const Position& pos);

}  // namespace hypersion::NNUE
