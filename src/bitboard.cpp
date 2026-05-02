// Hypersion — bitboard table initialization.
// Sliding attacks: classic Pradyumna Kannan magics (or BMI2 PEXT). Magic
// numbers are searched at startup using a Xorshift PRNG; this is fast enough
// (<1 ms total) and avoids shipping a hard-coded table.

#include "bitboard.h"

#include <algorithm>
#include <cstring>

#include "misc.h"

namespace hypersion {

// ---------------------------------------------------------------------------
// Public table storage
// ---------------------------------------------------------------------------
std::uint8_t SquareDistance[SQUARE_NB][SQUARE_NB];
Bitboard     LineBB    [SQUARE_NB][SQUARE_NB];
Bitboard     BetweenBB [SQUARE_NB][SQUARE_NB];
Bitboard     PseudoAttacks[PIECE_TYPE_NB][SQUARE_NB];
Bitboard     PawnAttacks  [COLOR_NB][SQUARE_NB];

Magic RookMagics  [SQUARE_NB];
Magic BishopMagics[SQUARE_NB];

namespace {

// Backing storage for sliding attacks. Sizes are the well-known totals:
//   Rooks   : 102400 entries (sum of 2^bits over 64 squares)
//   Bishops :   5248 entries
Bitboard RookTable  [0x19000];
Bitboard BishopTable[0x01480];

// Slow attack — only used at init time to build the magic tables.
Bitboard sliding_attack(const Direction dirs[4], Square s, Bitboard occupied) {
    Bitboard attacks = 0;
    for (int i = 0; i < 4; ++i) {
        Square cur = s;
        while (true) {
            Square nxt = Square(cur + dirs[i]);
            if (nxt < SQ_A1 || nxt > SQ_H8) break;
            // Stop if we wrapped around a file boundary.
            if (SquareDistance[cur][nxt] != 1) break;
            attacks |= square_bb(nxt);
            if (occupied & square_bb(nxt)) break;
            cur = nxt;
        }
    }
    return attacks;
}

// Initialize the magic table for one piece type (BISHOP or ROOK).
//   table  : flat backing array, sized to fit all squares' attack tables.
//   magics : output array, one Magic per square.
//   dirs   : the 4 ray directions for this piece.
void init_magics(Bitboard table[], Magic magics[], const Direction dirs[4]) {
    Bitboard* attackPtr = table;
    PRNG prng(0x246C'CB2D'3B40'2853ULL);

    for (Square s = SQ_A1; s <= SQ_H8; ++s) {
        // Edge squares of the rays don't need to be in the mask: blockers
        // sitting on the outer edge can't change which squares are attacked
        // beyond it, so excluding them keeps the table tiny.
        Bitboard edges = ((Rank1BB | Rank8BB) & ~rank_bb(s))
                       | ((FileABB | FileHBB) & ~file_bb(s));

        Magic& m = magics[s];
        m.mask  = sliding_attack(dirs, s, 0) & ~edges;
        m.shift = 64 - popcount(m.mask);
        m.attacks = attackPtr;

        // Enumerate every blocker subset of m.mask and compute its attack BB.
        // We then search for a magic number that maps each subset to a
        // collision-free index in the table. PEXT mode skips the search.
        const unsigned size = 1u << popcount(m.mask);

        Bitboard occ[4096], ref[4096];
        unsigned epoch[4096] = {};
        unsigned cnt = 0;
        Bitboard b = 0;
        do {
            occ[cnt] = b;
            ref[cnt] = sliding_attack(dirs, s, b);
            ++cnt;
            // Carry-rippler trick — iterates all subsets of m.mask exactly once.
            b = (b - m.mask) & m.mask;
        } while (b);

#if defined(USE_PEXT)
        m.magic = 0;
        for (unsigned i = 0; i < cnt; ++i)
            m.attacks[_pext_u64(occ[i], m.mask)] = ref[i];
#else
        // Search for a magic. Sparse randoms (& & &) converge in a few tries.
        // Epoch counter avoids needing to memset m.attacks between attempts.
        for (unsigned tries = 1;; ++tries) {
            do { m.magic = prng.sparse_rand(); }
            while (popcount((m.magic * m.mask) >> 56) < 6);

            bool ok = true;
            for (unsigned i = 0; ok && i < cnt; ++i) {
                unsigned idx = unsigned(((occ[i] & m.mask) * m.magic) >> m.shift);
                if (epoch[idx] != tries) {
                    epoch[idx]      = tries;
                    m.attacks[idx]  = ref[i];
                } else if (m.attacks[idx] != ref[i]) {
                    ok = false;
                }
            }
            if (ok) break;
        }
#endif
        attackPtr += size;
    }
}

}  // namespace

namespace Bitboards {

void init() {
    // 1. Square distance table (Chebyshev) — needed by sliding_attack().
    for (Square a = SQ_A1; a <= SQ_H8; ++a)
        for (Square b = SQ_A1; b <= SQ_H8; ++b)
            SquareDistance[a][b] =
                std::uint8_t(std::max(std::abs(file_of(a) - file_of(b)),
                                      std::abs(rank_of(a) - rank_of(b))));

    // 2. Knight, king, pawn pseudo-attacks (no occupancy needed).
    for (Square s = SQ_A1; s <= SQ_H8; ++s) {
        Bitboard b = square_bb(s);

        PawnAttacks[WHITE][s] = pawn_attacks_bb<WHITE>(b);
        PawnAttacks[BLACK][s] = pawn_attacks_bb<BLACK>(b);

        // Knight: 8 L-shaped offsets.
        for (int step : { -17, -15, -10, -6, 6, 10, 15, 17 }) {
            Square t = Square(int(s) + step);
            if (t >= SQ_A1 && t <= SQ_H8 && SquareDistance[s][t] <= 2)
                PseudoAttacks[KNIGHT][s] |= square_bb(t);
        }
        // King: 8 surrounding squares.
        for (int step : { -9, -8, -7, -1, 1, 7, 8, 9 }) {
            Square t = Square(int(s) + step);
            if (t >= SQ_A1 && t <= SQ_H8 && SquareDistance[s][t] == 1)
                PseudoAttacks[KING][s] |= square_bb(t);
        }
    }

    // 3. Sliding-piece magic tables.
    static const Direction RookDirs  [4] = { NORTH, EAST, SOUTH, WEST };
    static const Direction BishopDirs[4] = { NORTH_EAST, SOUTH_EAST, SOUTH_WEST, NORTH_WEST };
    init_magics(RookTable,   RookMagics,   RookDirs);
    init_magics(BishopTable, BishopMagics, BishopDirs);

    // 4. PseudoAttacks for sliders (empty board) + Line / Between tables.
    for (Square s1 = SQ_A1; s1 <= SQ_H8; ++s1) {
        PseudoAttacks[BISHOP][s1] = attacks_bb<BISHOP>(s1, 0);
        PseudoAttacks[ROOK]  [s1] = attacks_bb<ROOK>  (s1, 0);
        PseudoAttacks[QUEEN] [s1] = PseudoAttacks[BISHOP][s1] | PseudoAttacks[ROOK][s1];

        for (PieceType pt : { BISHOP, ROOK }) {
            for (Square s2 = SQ_A1; s2 <= SQ_H8; ++s2) {
                if (PseudoAttacks[pt][s1] & square_bb(s2)) {
                    LineBB   [s1][s2] = (attacks_bb(pt, s1, 0) & attacks_bb(pt, s2, 0))
                                        | square_bb(s1) | square_bb(s2);
                    BetweenBB[s1][s2] =  attacks_bb(pt, s1, square_bb(s2))
                                       & attacks_bb(pt, s2, square_bb(s1));
                }
                BetweenBB[s1][s2] |= square_bb(s2);   // include the destination square
            }
        }
    }
}

std::string pretty(Bitboard b) {
    std::string s = "+---+---+---+---+---+---+---+---+\n";
    for (Rank r = RANK_8; r >= RANK_1; --r) {
        for (File f = FILE_A; f <= FILE_H; ++f)
            s += (b & square_bb(make_square(f, r))) ? "| X " : "|   ";
        s += "| " + std::to_string(1 + int(r)) + "\n+---+---+---+---+---+---+---+---+\n";
    }
    s += "  a   b   c   d   e   f   g   h\n";
    return s;
}

}  // namespace Bitboards
}  // namespace hypersion
