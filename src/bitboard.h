// Hypersion — bitboard primitives, sliding-piece attack generation, and the
// precomputed lookup tables every other module relies on.
//
//  - Bitboard layout: bit i corresponds to Square i (a1=0, h1=7, a8=56, h8=63).
//  - Sliding attacks use Pradyumna Kannan's "fancy" magic bitboards by default
//    and BMI2 PEXT when compiled with -DUSE_PEXT (single-instruction lookups).
//  - All tables are filled by Bitboards::init() at startup.

#ifndef HYPERSION_BITBOARD_H
#define HYPERSION_BITBOARD_H

#include <bit>
#include <cassert>
#include <cstdint>
#include <string>

#include "types.h"

#if defined(USE_PEXT)
#include <immintrin.h>
#endif

namespace hypersion {

// ---------------------------------------------------------------------------
// File / rank / single-square bitboards
// ---------------------------------------------------------------------------
constexpr Bitboard FileABB = 0x0101010101010101ULL;
constexpr Bitboard FileBBB = FileABB << 1;
constexpr Bitboard FileCBB = FileABB << 2;
constexpr Bitboard FileDBB = FileABB << 3;
constexpr Bitboard FileEBB = FileABB << 4;
constexpr Bitboard FileFBB = FileABB << 5;
constexpr Bitboard FileGBB = FileABB << 6;
constexpr Bitboard FileHBB = FileABB << 7;

constexpr Bitboard Rank1BB = 0xFFULL;
constexpr Bitboard Rank2BB = Rank1BB << (8 * 1);
constexpr Bitboard Rank3BB = Rank1BB << (8 * 2);
constexpr Bitboard Rank4BB = Rank1BB << (8 * 3);
constexpr Bitboard Rank5BB = Rank1BB << (8 * 4);
constexpr Bitboard Rank6BB = Rank1BB << (8 * 5);
constexpr Bitboard Rank7BB = Rank1BB << (8 * 6);
constexpr Bitboard Rank8BB = Rank1BB << (8 * 7);

constexpr Bitboard AllSquares  = ~Bitboard(0);
constexpr Bitboard DarkSquares = 0xAA55AA55AA55AA55ULL;

constexpr Bitboard square_bb(Square s) { return Bitboard(1) << s; }
constexpr Bitboard file_bb(File f)     { return FileABB << f; }
constexpr Bitboard rank_bb(Rank r)     { return Rank1BB << (8 * r); }
constexpr Bitboard file_bb(Square s)   { return file_bb(file_of(s)); }
constexpr Bitboard rank_bb(Square s)   { return rank_bb(rank_of(s)); }

// ---------------------------------------------------------------------------
// Directions and bit-shift helpers
// ---------------------------------------------------------------------------
enum Direction : int {
    NORTH =  8,
    EAST  =  1,
    SOUTH = -NORTH,
    WEST  = -EAST,

    NORTH_EAST = NORTH + EAST,
    SOUTH_EAST = SOUTH + EAST,
    SOUTH_WEST = SOUTH + WEST,
    NORTH_WEST = NORTH + WEST
};

template<Direction D>
constexpr Bitboard shift(Bitboard b) {
    if constexpr (D == NORTH)      return b << 8;
    else if constexpr (D == SOUTH) return b >> 8;
    else if constexpr (D == EAST)  return (b & ~FileHBB) << 1;
    else if constexpr (D == WEST)  return (b & ~FileABB) >> 1;
    else if constexpr (D == NORTH_EAST) return (b & ~FileHBB) << 9;
    else if constexpr (D == NORTH_WEST) return (b & ~FileABB) << 7;
    else if constexpr (D == SOUTH_EAST) return (b & ~FileHBB) >> 7;
    else if constexpr (D == SOUTH_WEST) return (b & ~FileABB) >> 9;
    else return 0;
}

// Pawn pushes / attacks templated on Color so the compiler folds the branch.
template<Color C>
constexpr Bitboard pawn_push(Bitboard b) {
    return C == WHITE ? shift<NORTH>(b) : shift<SOUTH>(b);
}
template<Color C>
constexpr Bitboard pawn_attacks_bb(Bitboard b) {
    return C == WHITE ? shift<NORTH_EAST>(b) | shift<NORTH_WEST>(b)
                      : shift<SOUTH_EAST>(b) | shift<SOUTH_WEST>(b);
}

// ---------------------------------------------------------------------------
// Bit manipulation — lean wrappers around <bit> so call sites read clean.
// ---------------------------------------------------------------------------
inline int popcount(Bitboard b)        { return std::popcount(b); }
inline bool more_than_one(Bitboard b)  { return b & (b - 1); }
inline Square lsb(Bitboard b)          { assert(b); return Square(std::countr_zero(b)); }
inline Square msb(Bitboard b)          { assert(b); return Square(63 ^ std::countl_zero(b)); }
inline Square pop_lsb(Bitboard& b)     { Square s = lsb(b); b &= b - 1; return s; }
inline Bitboard least_significant_bit_bb(Bitboard b) { assert(b); return b & -b; }

// ---------------------------------------------------------------------------
// Precomputed tables (filled by Bitboards::init)
// ---------------------------------------------------------------------------
extern std::uint8_t SquareDistance[SQUARE_NB][SQUARE_NB];
extern Bitboard     LineBB    [SQUARE_NB][SQUARE_NB];   // through-line a..b extended to whole board
extern Bitboard     BetweenBB [SQUARE_NB][SQUARE_NB];   // squares strictly between a and b (incl. b)
extern Bitboard     PseudoAttacks[PIECE_TYPE_NB][SQUARE_NB];
extern Bitboard     PawnAttacks  [COLOR_NB][SQUARE_NB];

// Magic-bitboard descriptor for one square. The pointer attacks[] is shared
// across squares within the global RookTable / BishopTable arrays.
struct Magic {
    Bitboard  mask;
    Bitboard  magic;
    Bitboard* attacks;
    unsigned  shift;

    // index() turns an occupancy into the table offset.
    unsigned index(Bitboard occupied) const {
#if defined(USE_PEXT)
        return unsigned(_pext_u64(occupied, mask));
#else
        return unsigned(((occupied & mask) * magic) >> shift);
#endif
    }
};

extern Magic     RookMagics  [SQUARE_NB];
extern Magic     BishopMagics[SQUARE_NB];

// ---------------------------------------------------------------------------
// Attack queries
// ---------------------------------------------------------------------------
inline Bitboard attacks_bb(PieceType pt, Square s, Bitboard occupied) {
    assert(pt != PAWN && pt != NO_PIECE_TYPE);
    switch (pt) {
        case BISHOP: { const Magic& m = BishopMagics[s]; return m.attacks[m.index(occupied)]; }
        case ROOK:   { const Magic& m = RookMagics  [s]; return m.attacks[m.index(occupied)]; }
        case QUEEN:  { return attacks_bb(BISHOP, s, occupied) | attacks_bb(ROOK, s, occupied); }
        default:     return PseudoAttacks[pt][s];        // KNIGHT, KING
    }
}

// Compile-time-templated overload used in hot search paths.
template<PieceType Pt>
inline Bitboard attacks_bb(Square s, Bitboard occupied) {
    static_assert(Pt != PAWN && Pt != NO_PIECE_TYPE);
    if constexpr (Pt == BISHOP) { const Magic& m = BishopMagics[s]; return m.attacks[m.index(occupied)]; }
    else if constexpr (Pt == ROOK)   { const Magic& m = RookMagics[s]; return m.attacks[m.index(occupied)]; }
    else if constexpr (Pt == QUEEN)  { return attacks_bb<BISHOP>(s, occupied) | attacks_bb<ROOK>(s, occupied); }
    else                              { return PseudoAttacks[Pt][s]; }
}

// Pawn-attack lookup: PawnAttacks[c][s] = squares attacked by a c-color pawn on s.
inline Bitboard pawn_attacks_bb(Color c, Square s) { return PawnAttacks[c][s]; }

// "aligned(a,b,c)" → all three squares lie on a common line (rank/file/diag).
// Used by the pinned-piece logic and SEE.
inline bool aligned(Square a, Square b, Square c) { return LineBB[a][b] & square_bb(c); }

// Distance helper — Chebyshev distance (max of file/rank delta).
inline int distance(Square a, Square b) { return SquareDistance[a][b]; }

// ---------------------------------------------------------------------------
// Public init / debug
// ---------------------------------------------------------------------------
namespace Bitboards {
    void init();
    std::string pretty(Bitboard b);   // 8x8 grid for debug printing
}

}  // namespace hypersion

#endif  // HYPERSION_BITBOARD_H
