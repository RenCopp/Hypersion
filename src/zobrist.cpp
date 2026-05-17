#include "zobrist.h"

#include "misc.h"

namespace hypersion::Zobrist {

Key psq      [PIECE_NB][SQUARE_NB];
Key enpassant[FILE_NB];
Key castling [CASTLING_RIGHT_NB];
Key side;
Key noPawns;

void init() {
    PRNG prng(1070372ULL);   // Stockfish-compatible seed; deterministic across runs.
    for (int p = 0; p < PIECE_NB; ++p)
        for (int s = 0; s < SQUARE_NB; ++s)
            psq[p][s] = prng.rand();
    // 2026-05-17 audit zb #2: SF18 (position.cpp:125-127) zeros out pawn
    // psq slots on promotion ranks. Pawns can't legally exist there (they
    // promote on arrival), so the slots are never used in a real position;
    // zeroing them matches SF18 exactly and keeps a clean invariant.
    Piece WP = make_piece(WHITE, PAWN), BP = make_piece(BLACK, PAWN);
    for (int f = 0; f < FILE_NB; ++f) {
        psq[WP][make_square(File(f), RANK_8)] = 0;
        psq[BP][make_square(File(f), RANK_1)] = 0;
    }
    for (int f = 0; f < FILE_NB; ++f)
        enpassant[f] = prng.rand();
    for (int c = 0; c < CASTLING_RIGHT_NB; ++c)
        castling[c] = prng.rand();
    side    = prng.rand();
    noPawns = prng.rand();
}

}  // namespace hypersion::Zobrist
