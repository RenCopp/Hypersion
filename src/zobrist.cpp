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
    for (int f = 0; f < FILE_NB; ++f)
        enpassant[f] = prng.rand();
    for (int c = 0; c < CASTLING_RIGHT_NB; ++c)
        castling[c] = prng.rand();
    side    = prng.rand();
    noPawns = prng.rand();
}

}  // namespace hypersion::Zobrist
