// Hypersion — core types shared by all modules.
// Small, header-only, no allocations.

#ifndef HYPERSION_TYPES_H
#define HYPERSION_TYPES_H

#include <cassert>
#include <cstdint>
#include <cstddef>

namespace hypersion {

using Bitboard = std::uint64_t;
using Key      = std::uint64_t;
using Value    = int;
using Depth    = int;

constexpr int MAX_PLY   = 246;
constexpr int MAX_MOVES = 256;

constexpr Value VALUE_ZERO      = 0;
constexpr Value VALUE_DRAW      = 0;
constexpr Value VALUE_INFINITE  = 32001;
constexpr Value VALUE_NONE      = 32002;
constexpr Value VALUE_MATE      = 32000;
constexpr Value VALUE_MATE_IN_MAX_PLY  =  VALUE_MATE - MAX_PLY;
constexpr Value VALUE_MATED_IN_MAX_PLY = -VALUE_MATE + MAX_PLY;
constexpr Value VALUE_TB_WIN           = VALUE_MATE_IN_MAX_PLY - 1;
constexpr Value VALUE_TB_LOSS          = -VALUE_TB_WIN;
// Lower bound of the TB-win/loss range. Mirrors SF18's
// VALUE_TB_WIN_IN_MAX_PLY / VALUE_TB_LOSS_IN_MAX_PLY: any value with
// |v| >= VALUE_TB_WIN_IN_MAX_PLY is decisive (TB win/loss or true mate).
// Used to ply-adjust TB scores through the TT exactly the same way
// true mate scores are adjusted. Source: SF18 src/types.h::value_to_tt.
constexpr Value VALUE_TB_WIN_IN_MAX_PLY  =  VALUE_TB_WIN - MAX_PLY;
constexpr Value VALUE_TB_LOSS_IN_MAX_PLY = -VALUE_TB_WIN_IN_MAX_PLY;

enum Color : std::int8_t { WHITE, BLACK, COLOR_NB = 2 };

enum PieceType : std::int8_t {
    NO_PIECE_TYPE = 0,
    PAWN = 1, KNIGHT, BISHOP, ROOK, QUEEN, KING,
    ALL_PIECES = 0,
    PIECE_TYPE_NB = 8
};

// Piece encoding: color in bit 3, type in bits 0-2.
// Matches common layout so `type_of(pc) = pc & 7`, `color_of(pc) = pc >> 3`.
enum Piece : std::int8_t {
    NO_PIECE = 0,
    W_PAWN = 1, W_KNIGHT, W_BISHOP, W_ROOK, W_QUEEN, W_KING,
    B_PAWN = 9, B_KNIGHT, B_BISHOP, B_ROOK, B_QUEEN, B_KING,
    PIECE_NB = 16
};

enum Square : std::int8_t {
    SQ_A1, SQ_B1, SQ_C1, SQ_D1, SQ_E1, SQ_F1, SQ_G1, SQ_H1,
    SQ_A2, SQ_B2, SQ_C2, SQ_D2, SQ_E2, SQ_F2, SQ_G2, SQ_H2,
    SQ_A3, SQ_B3, SQ_C3, SQ_D3, SQ_E3, SQ_F3, SQ_G3, SQ_H3,
    SQ_A4, SQ_B4, SQ_C4, SQ_D4, SQ_E4, SQ_F4, SQ_G4, SQ_H4,
    SQ_A5, SQ_B5, SQ_C5, SQ_D5, SQ_E5, SQ_F5, SQ_G5, SQ_H5,
    SQ_A6, SQ_B6, SQ_C6, SQ_D6, SQ_E6, SQ_F6, SQ_G6, SQ_H6,
    SQ_A7, SQ_B7, SQ_C7, SQ_D7, SQ_E7, SQ_F7, SQ_G7, SQ_H7,
    SQ_A8, SQ_B8, SQ_C8, SQ_D8, SQ_E8, SQ_F8, SQ_G8, SQ_H8,
    SQ_NONE = 64,
    SQUARE_NB = 64
};

enum File : std::int8_t { FILE_A, FILE_B, FILE_C, FILE_D, FILE_E, FILE_F, FILE_G, FILE_H, FILE_NB = 8 };
enum Rank : std::int8_t { RANK_1, RANK_2, RANK_3, RANK_4, RANK_5, RANK_6, RANK_7, RANK_8, RANK_NB = 8 };

enum CastlingRights : std::uint8_t {
    NO_CASTLING   = 0,
    WHITE_OO      = 1,
    WHITE_OOO     = 2,
    BLACK_OO      = 4,
    BLACK_OOO     = 8,
    WHITE_CASTLING = WHITE_OO | WHITE_OOO,
    BLACK_CASTLING = BLACK_OO | BLACK_OOO,
    ANY_CASTLING   = WHITE_CASTLING | BLACK_CASTLING,
    CASTLING_RIGHT_NB = 16
};

enum Bound : std::uint8_t {
    BOUND_NONE  = 0,
    BOUND_UPPER = 1,
    BOUND_LOWER = 2,
    BOUND_EXACT = BOUND_UPPER | BOUND_LOWER
};

// Move is packed into 16 bits:
//   bits 0-5   : from-square (0..63)
//   bits 6-11  : to-square   (0..63)
//   bits 12-13 : promotion piece type - 2  (0=N, 1=B, 2=R, 3=Q)
//   bits 14-15 : move type   (0=normal, 1=promotion, 2=en passant, 3=castling)
enum MoveType : std::uint16_t {
    MT_NORMAL    = 0 << 14,
    MT_PROMOTION = 1 << 14,
    MT_EN_PASSANT= 2 << 14,
    MT_CASTLING  = 3 << 14
};

class Move {
    std::uint16_t data = 0;
public:
    constexpr Move() = default;
    constexpr explicit Move(std::uint16_t d) : data(d) {}
    constexpr Move(Square from, Square to)
        : data(std::uint16_t((to << 6) | from)) {}

    static constexpr Move make(Square from, Square to,
                               MoveType mt = MT_NORMAL,
                               PieceType promo = KNIGHT) {
        return Move(std::uint16_t((int(promo) - int(KNIGHT)) << 12 | int(mt) | (to << 6) | from));
    }
    static constexpr Move none() { return Move(0); }
    static constexpr Move null() { return Move(65); }   // nonzero, invalid (from==to==1)

    constexpr Square   from_sq() const { return Square(data & 0x3F); }
    constexpr Square   to_sq()   const { return Square((data >> 6) & 0x3F); }
    constexpr MoveType type_of() const { return MoveType(data & 0xC000); }
    constexpr PieceType promotion_type() const {
        return PieceType(((data >> 12) & 0x3) + int(KNIGHT));
    }
    constexpr std::uint16_t raw() const { return data; }
    constexpr bool is_ok() const { return data != Move::none().data && data != Move::null().data; }
    constexpr bool operator==(const Move& o) const { return data == o.data; }
    constexpr bool operator!=(const Move& o) const { return data != o.data; }
    constexpr explicit operator bool() const { return data != 0; }
};

// Helpers -------------------------------------------------------------------

constexpr Color    operator~(Color c)       { return Color(c ^ BLACK); }
constexpr Square   operator~(Square s)      { return Square(s ^ SQ_A8); }  // vertical flip
constexpr File     file_of(Square s)        { return File(s & 7); }
constexpr Rank     rank_of(Square s)        { return Rank(s >> 3); }
constexpr Square   make_square(File f, Rank r) { return Square((r << 3) | f); }
constexpr PieceType type_of(Piece pc)       { return PieceType(pc & 7); }
constexpr Color    color_of(Piece pc)       { assert(pc != NO_PIECE); return Color(pc >> 3); }
constexpr Piece    make_piece(Color c, PieceType pt) { return Piece((c << 3) | pt); }

constexpr Value    mate_in(int ply)  { return VALUE_MATE - ply; }
constexpr Value    mated_in(int ply) { return -VALUE_MATE + ply; }

// Arithmetic operators on enums so `sq + 8` etc. works without casts.
#define ENABLE_ENUM_OPERATORS(T) \
    constexpr T  operator+(T a, int b) { return T(int(a) + b); } \
    constexpr T  operator-(T a, int b) { return T(int(a) - b); } \
    constexpr T& operator+=(T& a, int b) { return a = a + b; } \
    constexpr T& operator-=(T& a, int b) { return a = a - b; } \
    constexpr T& operator++(T& a)      { return a = T(int(a) + 1); } \
    constexpr T& operator--(T& a)      { return a = T(int(a) - 1); }
ENABLE_ENUM_OPERATORS(Square)
ENABLE_ENUM_OPERATORS(File)
ENABLE_ENUM_OPERATORS(Rank)
ENABLE_ENUM_OPERATORS(PieceType)
#undef ENABLE_ENUM_OPERATORS

}  // namespace hypersion

#endif  // HYPERSION_TYPES_H
