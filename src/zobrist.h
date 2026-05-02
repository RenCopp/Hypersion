// Hypersion — Zobrist hashing tables.
// Each (piece, square), side-to-move, castling-right combo, and en-passant
// file gets a random 64-bit key. The full position key is the XOR of all
// applicable keys, so do_move/undo_move only XOR a handful of values.
//
// Pawn key (only pawn-related keys) and material/non-pawn keys are also kept
// separately for use by NNUE (incremental refresh) and correction history.

#ifndef HYPERSION_ZOBRIST_H
#define HYPERSION_ZOBRIST_H

#include "types.h"

namespace hypersion::Zobrist {

extern Key psq        [PIECE_NB][SQUARE_NB];
extern Key enpassant  [FILE_NB];
extern Key castling   [CASTLING_RIGHT_NB];
extern Key side;
extern Key noPawns;

void init();

}  // namespace hypersion::Zobrist

#endif  // HYPERSION_ZOBRIST_H
