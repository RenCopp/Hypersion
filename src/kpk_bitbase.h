// Hypersion — KPK (King + Pawn vs King) endgame bitbase.
//
// Pre-computes the win/draw status of every K+P-vs-K position via retrograde
// analysis, stored as a packed bit-table. Probed during classical eval to
// scale the eg score correctly for KP-vs-K positions: a KP-vs-K marked
// "draw" gets eg scaled to 0; a "win" keeps full pawn-bonus magnitude.
//
// Storage layout:
//   index = encode(stm, blackK, whiteK, whiteP)
//   where whiteP is one of the 24 squares on files a-d, ranks 2-7
//   (uses left-right symmetry to halve the table size).
//   With 2 stm × 24 pawn × 64 wK × 64 bK = 196 608 positions = 24 KB packed.
//
// Algorithm: SF's bitbase.cpp::Bitbases::init() reduced to essentials.
//   - Each position starts UNKNOWN, except those that are immediate WIN/DRAW
//     by rule (pawn promotion that survives, stalemate, capture).
//   - Repeat: for each UNKNOWN position, try to classify it:
//       * stm == strong (white): WIN if any legal move leads to a WIN child.
//       * stm == weak   (black): WIN if ALL legal moves lead to WIN child.
//       * Otherwise stays UNKNOWN (will collapse to DRAW after fixpoint).
//   - Iterate until no changes; then UNKNOWN positions are draws.
//
// Reference: chessprogramming.org/KPK, SF sf_10 src/bitbase.cpp.

#pragma once

#include "bitboard.h"
#include "types.h"

namespace hypersion::KPK {

// 196 608 positions packed as bits = 24 576 bytes ≈ 24 KB.
constexpr unsigned MAX_INDEX = 2 * 24 * 64 * 64;
inline std::uint32_t s_bitbase[MAX_INDEX / 32];   // global, init once at startup

enum Result : std::uint8_t { INVALID = 0, UNKNOWN = 1, DRAW = 2, WIN = 4 };

// Compress white-pawn square (a2-h7 minus a1/h1/a8/h8) into 0..23 via
// (file, rank-1) layout. Files are restricted to a-d by left-right
// reflection at probe time.
inline unsigned pawn_index(Square pawnSq) {
    return unsigned(file_of(pawnSq)) + 4 * unsigned(rank_of(pawnSq) - 1);
}

inline unsigned index(Color stm, Square bk, Square wk, Square wp) {
    return unsigned(stm) | (unsigned(bk) << 1) | (unsigned(wk) << 7)
         | (pawn_index(wp) << 13);
}

inline bool probe(Square wk, Square wp, Square bk, Color stm) {
    // Reflect to canonical (pawn on files a-d).
    if (file_of(wp) >= FILE_E) {
        wk = Square(int(wk) ^ 7);   // mirror file
        bk = Square(int(bk) ^ 7);
        wp = Square(int(wp) ^ 7);
    }
    unsigned idx = index(stm, bk, wk, wp);
    return s_bitbase[idx / 32] & (1u << (idx & 31));
}

namespace detail {

// Per-position state during init. The packed result is written to the
// final s_bitbase after init() converges.
struct KPKPosition {
    Square whiteK = SQ_A1, blackK = SQ_A1, whiteP = SQ_A1;
    Color  stm = WHITE;
    Result res = INVALID;

    KPKPosition() = default;

    explicit KPKPosition(unsigned idx) {
        stm    = Color((idx >> 0) & 1);
        blackK = Square((idx >> 1) & 0x3F);
        whiteK = Square((idx >> 7) & 0x3F);
        unsigned pi = (idx >> 13) & 0x1F;
        whiteP = Square((pi & 3) + 8 * ((pi >> 2) + 1));   // file 0..3, rank 1..6
        res    = classify_immediate();
    }

    Result classify_immediate() {
        // Same-square or king-too-close → invalid.
        if (whiteK == blackK || whiteK == whiteP) return INVALID;
        if (distance(whiteK, blackK) <= 1)        return INVALID;
        // Pawn attacks black king but black to move → invalid (black is
        // already in check on stm == white_already moved).
        Bitboard pawnAtk = pawn_attacks_bb(WHITE, whiteP);
        if (stm == WHITE && (pawnAtk & square_bb(blackK))) return INVALID;
        // Black king on the white pawn? invalid (king on piece).
        if (blackK == whiteP) return INVALID;
        // White's pawn on its own first rank? Already filtered by pi.

        // Immediate wins:
        // White to move, pawn on rank 7 with safe promotion = win.
        if (stm == WHITE && rank_of(whiteP) == RANK_7
            && whiteK != Square(int(whiteP) + 8)
            && (distance(blackK, Square(int(whiteP) + 8)) > 1
                || distance(whiteK, Square(int(whiteP) + 8)) == 1))
            return WIN;

        // Black to move, no legal black-king moves AND not in check → stalemate (DRAW).
        if (stm == BLACK) {
            Bitboard bkAtk = PseudoAttacks[KING][blackK];
            // squares attacked by white pieces
            Bitboard whiteAtk = PseudoAttacks[KING][whiteK] | pawn_attacks_bb(WHITE, whiteP);
            Bitboard legal = bkAtk & ~whiteAtk & ~square_bb(whiteK);
            if (!legal) {
                // In check?  Black king attacked by white pawn or king?
                bool inCheck = (pawn_attacks_bb(WHITE, whiteP) & square_bb(blackK)) ||
                               (PseudoAttacks[KING][whiteK] & square_bb(blackK));
                if (!inCheck) return DRAW;
                // In check with no escape — checkmate? But white pieces here
                // can't checkmate alone with one pawn (the pawn just attacks
                // diagonally). A king can never deliver mate without help. So
                // if in check + no escape, it's a contradiction → INVALID.
                return INVALID;
            }
            // Black can capture white pawn? → DRAW (KPvK with pawn captured
            // becomes KvK, draw).
            if (legal & square_bb(whiteP) && !(PseudoAttacks[KING][whiteK] & square_bb(whiteP)))
                return DRAW;
        }
        return UNKNOWN;
    }

    // Try to classify based on neighbour positions. Returns updated result.
    Result classify(const KPKPosition* db);
};

inline Result KPKPosition::classify(const KPKPosition* db) {
    if (res != UNKNOWN) return res;

    bool good = (stm == WHITE) ? false : true;   // strong = white
    Bitboard atk;

    if (stm == WHITE) {
        // White to move. WIN if any move leads to WIN; else (after fixpoint) DRAW.
        atk = PseudoAttacks[KING][whiteK];
        while (atk) {
            Square to = pop_lsb(atk);
            if (to == blackK) continue;
            if (distance(to, blackK) <= 1) continue;   // would put own king adjacent (illegal next turn)
            if (to == whiteP) continue;   // can't capture own pawn
            unsigned idx = index(BLACK, blackK, to, whiteP);
            Result r = Result(db[idx].res);
            if (r == WIN) return WIN;
        }
        // Pawn moves.
        Square push = Square(int(whiteP) + 8);
        if (push <= SQ_H8 && push != whiteK && push != blackK) {
            if (rank_of(whiteP) == RANK_7) {
                // Promotion → goal reached (the immediate-classify already
                // caught the safe promotion case; for non-safe cases this
                // doesn't reach here).
            } else {
                unsigned idx = index(BLACK, blackK, whiteK, push);
                if (db[idx].res == WIN) return WIN;
                // Two-square push from rank 2.
                if (rank_of(whiteP) == RANK_2) {
                    Square push2 = Square(int(whiteP) + 16);
                    if (push2 != whiteK && push2 != blackK) {
                        unsigned idx2 = index(BLACK, blackK, whiteK, push2);
                        if (db[idx2].res == WIN) return WIN;
                    }
                }
            }
        }
        return UNKNOWN;
    }
    // BLACK to move. Strong-side loses one tempo per move. WIN here means
    // "white still wins despite black's best defence" — so ALL black moves
    // must lead to WIN for white.
    atk = PseudoAttacks[KING][blackK];
    Bitboard whiteAtk = PseudoAttacks[KING][whiteK] | pawn_attacks_bb(WHITE, whiteP);
    bool hasLegal = false;
    while (atk) {
        Square to = pop_lsb(atk);
        if (to == whiteK) continue;
        if (whiteAtk & square_bb(to))   // square attacked by white
            if (to != whiteP) continue;   // capturing pawn handled below
        // Special-case pawn capture.
        if (to == whiteP) {
            // Pawn defended by white king?
            if (PseudoAttacks[KING][whiteK] & square_bb(whiteP)) continue;
            // Capturing the pawn = DRAW.
            hasLegal = true;
            good = false;
            break;
        }
        hasLegal = true;
        unsigned idx = index(WHITE, to, whiteK, whiteP);
        if (db[idx].res != WIN) { good = false; break; }
    }
    if (!hasLegal) return UNKNOWN;
    return good ? WIN : UNKNOWN;
}

}   // namespace detail

inline void init() {
    using namespace detail;
    static bool initialized = false;
    if (initialized) return;
    initialized = true;

    static KPKPosition db[MAX_INDEX];
    for (unsigned i = 0; i < MAX_INDEX; ++i)
        db[i] = KPKPosition(i);

    // Iterate until no changes.
    bool changed = true;
    int iters = 0;
    while (changed && iters < 32) {
        changed = false;
        for (unsigned i = 0; i < MAX_INDEX; ++i) {
            if (db[i].res != UNKNOWN) continue;
            Result r = db[i].classify(db);
            if (r != UNKNOWN) { db[i].res = r; changed = true; }
        }
        ++iters;
    }

    // Pack to s_bitbase: 1 bit per position, 1 = WIN.
    for (unsigned i = 0; i < MAX_INDEX; ++i)
        if (db[i].res == WIN)
            s_bitbase[i / 32] |= 1u << (i & 31);
}

}   // namespace hypersion::KPK
