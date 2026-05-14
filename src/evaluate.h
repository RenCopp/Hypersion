// Hypersion — evaluation function.
//
// M2 ships a classical material + PSQT evaluator: enough signal to play a
// reasonable game and to validate the search. M5 replaces this with the
// Stockfish-format NNUE for the main score, while keeping a slimmed-down
// classical fallback for cases where NNUE is unavailable.

#ifndef HYPERSION_EVALUATE_H
#define HYPERSION_EVALUATE_H

#include "position.h"
#include "types.h"

namespace hypersion::Eval {

constexpr Value PieceValueMG[PIECE_TYPE_NB] = { 0, 126, 781, 825, 1276, 2538, 0, 0 };
constexpr Value PieceValueEG[PIECE_TYPE_NB] = { 0, 208, 854, 915, 1380, 2682, 0, 0 };
// Tempo moved to eval_params::Params::TempoMG / TempoEG (Round 2 2026-05-13).
// Texel-tunable; previous constexpr value (28) reproduced by setting
// TempoMG = TempoEG = 28 in eval_params.h.

void  init();
Value evaluate(const Position& pos);   // returns value from side-to-move's POV

}  // namespace hypersion::Eval

#endif  // HYPERSION_EVALUATE_H
